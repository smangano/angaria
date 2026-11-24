// Independent process harness for topology activation and updates. Normal modes use the public
// Registry; fault modes invoke its shared private implementation with qualification
// hooks so the parent can stop at a real resource, journal, or recovery boundary.
// Stdout carries one-byte markers followed by native u64 values on this same host.

#include "angaria/runtime.h"
#include "topology_mutations.h"

#include <algorithm>
#include <array>
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

  template <typename T>
  T require_value(Result<T> result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    return std::get<T>(std::move(result));
  }

  void report(const void* data, std::size_t length)
  {
    const auto* bytes = static_cast<const char*>(data);
    while (length != 0)
    {
      const auto count = ::write(STDOUT_FILENO, bytes, length);
      if (count > 0)
      {
        bytes += count;
        length -= static_cast<std::size_t>(count);
      }
      else if (count != -1 || errno != EINTR)
      {
        throw std::runtime_error("activation worker report failed");
      }
    }
  }

  void report(char value)
  {
    report(&value, 1);
  }

  void report(std::uint64_t value)
  {
    report(&value, sizeof(value));
  }

  void report_result(const OperationResult& result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      report('E');
      report(static_cast<std::uint64_t>(error->_code));
    }
    else
    {
      report('A');
      report(std::uint64_t{0});
    }
  }

  void wait_for_parent()
  {
    pollfd event{STDIN_FILENO, POLLIN, 0};
    int ready;
    do
    {
      ready = ::poll(&event, 1, 5000);
    } while (ready == -1 && errno == EINTR);
    if (ready <= 0)
    {
      throw std::runtime_error("activation worker continuation deadline expired");
    }
    char value{};
    ssize_t count;
    do
    {
      count = ::read(STDIN_FILENO, &value, 1);
    } while (count == -1 && errno == EINTR);
    if (count == -1)
    {
      throw std::runtime_error("activation worker continuation failed");
    }
  }

  [[noreturn]] void stop_for_kill()
  {
    report('R');
    for (;;)
    {
      ::pause();
    }
  }

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
      return Error{ErrorCode::IoError, "Injected activation I/O failure", "test.activation"};
    }

    std::string _mode;
    bool _failed{};
  };
  runtime::TopologyChange change_for(unsigned operation, std::span<const QueueId> destinations)
  {
    runtime::TopologyChange change;
    change._operation = static_cast<runtime::TopologyMutation>(operation);
    change._id = operation == 2 || operation == 4 || operation == 7 ? 3 : 1;
    change._owner = TaskId{1};
    change._source = SourceQueueSetId{2};
    change._priority = 7;
    change._destinations = destinations;
    return change;
  }

  OperationResult perform(Registry registry, const runtime::TopologyChange& change)
  {
    switch (change._operation)
    {
    case runtime::TopologyMutation::AdoptQueue:
      return registry.adopt_queue(QueueId{change._id}, change._owner);
    case runtime::TopologyMutation::AdoptSource:
      return registry.adopt_source_set(SourceQueueSetId{change._id}, change._owner);
    case runtime::TopologyMutation::AdoptPublishing:
      return registry.adopt_publishing_set(PublishingQueueSetId{change._id}, change._owner);
    case runtime::TopologyMutation::AssignSource:
      return registry.assign_source(QueueId{change._id}, change._source, change._priority);
    case runtime::TopologyMutation::SetDestinations:
      return registry.set_destinations(PublishingQueueSetId{change._id}, change._destinations);
    case runtime::TopologyMutation::ActivateQueue:
      return registry.activate_queue(QueueId{change._id});
    case runtime::TopologyMutation::ActivateSource:
      return registry.activate_source_set(SourceQueueSetId{change._id});
    case runtime::TopologyMutation::ActivatePublishing:
      return registry.activate_publishing_set(PublishingQueueSetId{change._id});
    }
    throw std::runtime_error("invalid activation operation");
  }

  bool changed(Registry registry, unsigned operation, std::span<const QueueId> destinations)
  {
    switch (static_cast<runtime::TopologyMutation>(operation))
    {
    case runtime::TopologyMutation::AdoptQueue:
      return require_value(registry.describe_queue(QueueId{1}))._producer == TaskId{1};
    case runtime::TopologyMutation::AdoptSource:
      return require_value(registry.describe_source_set(SourceQueueSetId{1}))._owner == TaskId{1};
    case runtime::TopologyMutation::AdoptPublishing:
      return require_value(registry.describe_publishing_set(PublishingQueueSetId{3}))._owner == TaskId{1};
    case runtime::TopologyMutation::AssignSource:
    {
      const auto descriptor = require_value(registry.describe_queue(QueueId{1}));
      return descriptor._sourceSet == SourceQueueSetId{2} && descriptor._priority == 7;
    }
    case runtime::TopologyMutation::SetDestinations:
    {
      const auto descriptor = require_value(registry.describe_publishing_set(PublishingQueueSetId{3}));
      return std::equal(descriptor._destinations.begin(), descriptor._destinations.end(), destinations.begin(), destinations.end());
    }
    case runtime::TopologyMutation::ActivateQueue:
      return require_value(registry.state(QueueId{1})) == EntityState::Active;
    case runtime::TopologyMutation::ActivateSource:
      return require_value(registry.state(SourceQueueSetId{1})) == EntityState::Active;
    case runtime::TopologyMutation::ActivatePublishing:
      return require_value(registry.state(PublishingQueueSetId{3})) == EntityState::Active;
    }
    throw std::runtime_error("invalid activation operation");
  }

  RegistrationMetadata metadata(Registry registry, unsigned operation)
  {
    if (operation == 1 || operation == 6)
    {
      return require_value(registry.metadata(SourceQueueSetId{1}));
    }
    if (operation == 2 || operation == 4 || operation == 7)
    {
      return require_value(registry.metadata(PublishingQueueSetId{3}));
    }
    return require_value(registry.metadata(QueueId{1}));
  }
} // namespace

int main(int argc, char** argv)
{
  try
  {
    if (argc != 11)
    {
      throw std::runtime_error("usage: native_activation_worker mode directory actor operation variant unused unused unused checkpoint index");
    }
    ::alarm(9);
    const std::string mode{argv[1]};
    const auto actor = static_cast<TaskId>(std::stoul(argv[3]));
    const auto operation = static_cast<unsigned>(std::stoul(argv[4]));
    if (operation > 7)
    {
      throw std::runtime_error("invalid activation operation");
    }
    const auto variant = std::stoul(argv[5]);
    const std::array destinations{QueueId{2}, QueueId{1}};
    const auto change = change_for(operation, std::span{destinations}.first(variant == 0 ? 2 : 1));
    StopAt stop(static_cast<mapped::JournalCheckpoint>(std::stoul(argv[9])), std::stoull(argv[10]));
    if (mode == "recover")
    {
      // Attach only the private journal so the observer sees accepted partial
      // redo before public Session attachment completes it automatically.
      auto controller = require_value(mapped::SessionController::open_existing(argv[2]));
      auto guard = require_value(controller->acquire_mutation());
      auto journal = require_value(mapped::MutationJournal::open(controller, guard, &stop));
      require_value(journal.recover(guard));
      throw std::runtime_error("activation recovery missed the requested checkpoint");
    }
    auto session = require_value(Session::attach(argv[2]));
    if (mode == "inspect")
    {
      auto registry = session.registry();
      const auto attribution = metadata(registry, operation);
      report('I');
      report(static_cast<std::uint64_t>(changed(registry, operation, change._destinations)));
      report(static_cast<std::uint64_t>(attribution._createdBy));
      report(attribution._revision);
      report(static_cast<std::uint64_t>(require_value(registry.source_members(SourceQueueSetId{1})).size()));
      return 0;
    }
    if (mode == "hold_endpoints")
    {
      // Endpoint states retain the real Task claim after its original handle
      // closes. No send/receive or delivery cleanup is being modeled here.
      auto owner = require_value(session.attach_task(actor));
      const auto incarnation = owner.claim()._incarnation;
      auto writer = require_value(owner.open_writer(QueueId{1}));
      auto source = require_value(owner.open_source_set(SourceQueueSetId{1}));
      auto publishing = require_value(owner.open_publishing_set(PublishingQueueSetId{3}));
      {
        auto closing = std::move(owner);
      }
      report('H');
      report(static_cast<std::uint64_t>(writer.id()));
      report(static_cast<std::uint64_t>(source.id()));
      report(static_cast<std::uint64_t>(publishing.id()));
      report(incarnation);
      wait_for_parent();
      return 0;
    }
    if (mode == "register")
    {
      auto owner = require_value(session.attach_task(actor));
      report('R');
      wait_for_parent();
      report_result(perform(owner.registry(), change));
      wait_for_parent();
      return 0;
    }
    // Fault modes call the public implementation's private injection seam,
    // retaining a genuinely attached actor throughout lookup and exact retry.
    auto state = std::make_shared<runtime::SessionState>();
    state->_directory = std::filesystem::canonical(argv[2]);
    state->_controller = require_value(mapped::SessionController::open_existing(argv[2]));
    auto owner = require_value(runtime::attach_runtime_task(state, actor));
    const auto update = [&](mapped::JournalFaultInjector* injector)
    {
      report_result(runtime::runtime_update_topology(state, owner, change, injector));
    };
    if (mode == "crash")
    {
      update(&stop);
      throw std::runtime_error("activation update missed its requested checkpoint");
    }
    if (mode == "io_before" || mode == "io_after" || mode == "io_unknown")
    {
      IoFailure failure(mode);
      update(&failure);
      wait_for_parent();
      update(nullptr);
      wait_for_parent();
      return 0;
    }
    throw std::runtime_error("unknown activation worker mode");
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
