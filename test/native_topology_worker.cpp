// Independent process harness for draft topology registration. Normal modes use the public
// Registry; fault modes invoke its shared private implementation with qualification
// hooks so the parent can stop at a real resource, journal, or recovery boundary.
// Stdout carries one-byte markers followed by native u64 values on this same host.

#include "result_helpers.h"
#include "process_support.h"
#include "angaria/runtime.h"
#include "topology_state.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <poll.h>
#include <signal.h>
#include <unistd.h>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;

  using angaria::test_support::require_value;
  using angaria::test_support::report;
  using angaria::test_support::report_result;
  using angaria::test_support::wait_for_parent;
  using angaria::test_support::stop_for_kill;

  class StopAt : public mapped::JournalFaultInjector
  {
  public:
    StopAt(mapped::JournalCheckpoint point, std::uint64_t index) : _point(point), _index(index)
    {
    }

    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t index) override
    {
      if (point == _point && index == _index)
      {
        stop_for_kill();
      }
      return std::monostate{};
    }

  private:
    mapped::JournalCheckpoint _point;
    std::uint64_t _index;
  };

  // A one-shot failure permits execute's in-call repair. The persistent case
  // also interrupts accepted redo, forcing OutcomeUnknown until another caller
  // recovers without this injector. No application callback uses these hooks.
  class IoFailure : public mapped::JournalFaultInjector
  {
  public:
    explicit IoFailure(std::string mode) : _mode(std::move(mode))
    {
    }

    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t) override
    {
      if (_mode == "io_unknown")
      {
        if (point == mapped::JournalCheckpoint::Committed)
        {
          _failed = true;
        }
        if (_failed && (point == mapped::JournalCheckpoint::Committed || point == mapped::JournalCheckpoint::RedoWord))
        {
          return failure();
        }
      }
      else if (!_failed && point == (_mode == "io_before" ? mapped::JournalCheckpoint::FileGrowth :
                                                            mapped::JournalCheckpoint::Committed))
      {
        _failed = true;
        return failure();
      }
      return std::monostate{};
    }

  private:
    static OperationResult failure()
    {
      return Error{ErrorCode::IoError, "Injected registration I/O failure", "test.topology"};
    }

    std::string _mode;
    bool _failed{};
  };
} // namespace

int main(int argc, char** argv)
{
  try
  {
    if (argc != 11)
    {
      throw std::runtime_error("usage: native_topology_worker mode directory actor kind unused unused name doc checkpoint index");
    }
    ::alarm(9);
    const std::string mode{argv[1]};
    const auto actor = static_cast<TaskId>(std::stoul(argv[3]));
    const auto kind = std::stoul(argv[4]);
    StopAt stop(static_cast<mapped::JournalCheckpoint>(std::stoul(argv[9])), std::stoull(argv[10]));
    if (mode == "recover")
    {
      // Install the recovery observer before public Session attachment could
      // finish the pending append. This mode is private qualification only.
      auto controller = require_value(mapped::SessionController::open_existing(argv[2]));
      auto guard = require_value(controller->acquire_mutation());
      auto journal = require_value(mapped::MutationJournal::open(controller, guard, &stop));
      require_value(journal.recover(guard));
      throw std::runtime_error("topology recovery did not reach the requested checkpoint");
    }
    SourceQueueSetDescriptor source;
    source._name = argv[7];
    source._owner = TaskId{1};
    source._doc = argv[8];
    QueueDescriptor queue;
    queue._name = argv[7];
    queue._producer = TaskId{1};
    queue._sourceSet = SourceQueueSetId{1};
    queue._priority = 7;
    queue._doc = argv[8];
    PublishingQueueSetDescriptor publishing;
    publishing._name = argv[7];
    publishing._owner = TaskId{1};
    publishing._destinations = {QueueId{1}};
    publishing._doc = argv[8];
    auto session = require_value(Session::attach(argv[2]));
    if (mode == "inspect")
    {
      auto registry = session.registry();
      RegistrationMetadata metadata;
      std::uint64_t id{};
      std::uint64_t count{};
      if (kind == 0)
      {
        const auto found = require_value(registry.find_source_set(source._name));
        const auto descriptor = require_value(registry.describe_source_set(found));
        if (descriptor._owner != TaskId{1} || descriptor._doc != source._doc ||
            descriptor._selection != SelectionPolicy::Strict || descriptor._wait != WaitPolicy::Spin ||
            require_value(registry.state(found)) != EntityState::Draft)
        {
          throw std::runtime_error("independent Source inspection disagrees");
        }
        id = angaria::test_support::reported_identity(found);
        count = require_value(registry.source_members(found)).size();
        metadata = require_value(registry.metadata(found));
      }
      else if (kind == 1)
      {
        const auto found = require_value(registry.find_queue(queue._name));
        const auto descriptor = require_value(registry.describe_queue(found));
        const auto members = require_value(registry.source_members(SourceQueueSetId{1}));
        if (descriptor._producer != TaskId{1} || descriptor._sourceSet != SourceQueueSetId{1} ||
            descriptor._priority != 7 || descriptor._doc != queue._doc ||
            descriptor._ringCapacity != 1024 || descriptor._delivery != DeliveryPolicy::RecoverFromLog ||
            require_value(registry.state(found)) != EntityState::Draft ||
            std::count(members.begin(), members.end(), found) != 1)
        {
          throw std::runtime_error("independent Queue/membership inspection disagrees");
        }
        id = angaria::test_support::reported_identity(found);
        count = members.size();
        metadata = require_value(registry.metadata(found));
      }
      else
      {
        const auto found = require_value(registry.find_publishing_set(publishing._name));
        const auto descriptor = require_value(registry.describe_publishing_set(found));
        if (descriptor._owner != TaskId{1} || descriptor._doc != publishing._doc ||
            descriptor._destinations != publishing._destinations || descriptor._acceptance != AcceptancePolicy::AllDestinations ||
            require_value(registry.state(found)) != EntityState::Draft)
        {
          throw std::runtime_error("independent Publishing inspection disagrees");
        }
        id = angaria::test_support::reported_identity(found);
        count = descriptor._destinations.size();
        metadata = require_value(registry.metadata(found));
      }
      report('I');
      report(id);
      report(static_cast<std::uint64_t>(metadata._createdBy));
      report(metadata._revision);
      report(count);
      return 0;
    }
    if (mode == "register")
    {
      auto owner = require_value(session.attach_task(actor));
      report('R');
      wait_for_parent();
      if (kind == 0)
      {
        report_result(owner.registry().register_source_set(source));
      }
      else if (kind == 1)
      {
        report_result(owner.registry().register_queue(queue));
      }
      else
      {
        report_result(owner.registry().register_publishing_set(publishing));
      }
      wait_for_parent();
      return 0;
    }
    auto state = std::make_shared<runtime::SessionState>();
    state->_directory = std::filesystem::canonical(argv[2]);
    state->_controller = require_value(mapped::SessionController::open_existing(argv[2]));
    auto owner = require_value(runtime::attach_runtime_task(state, actor));
    const auto perform = [&](mapped::JournalFaultInjector* injector)
    {
      if (kind == 0)
      {
        report_result(runtime::runtime_register_source_set(state, owner, source, injector));
      }
      else if (kind == 1)
      {
        report_result(runtime::runtime_register_queue(state, owner, queue, injector));
      }
      else
      {
        report_result(runtime::runtime_register_publishing_set(state, owner, publishing, injector));
      }
    };
    if (mode == "crash")
    {
      perform(&stop);
      throw std::runtime_error("topology registration missed its requested checkpoint");
    }
    if (mode == "io_before" || mode == "io_after" || mode == "io_unknown")
    {
      IoFailure failure(mode);
      perform(&failure);
      wait_for_parent();
      perform(nullptr);
      wait_for_parent();
      return 0;
    }
    throw std::runtime_error("unknown topology worker mode");
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
