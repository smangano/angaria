// Independent process harness for Task registration. Normal modes use the public
// Registry; fault modes invoke its shared private implementation with qualification
// hooks so the parent can stop at a real resource, journal, or recovery boundary.
// Stdout carries one-byte markers followed by native u64 values on this same host.

#include "angaria/runtime.h"
#include "runtime_state.h"

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
        throw std::runtime_error("registration worker report failed");
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

  void report_result(const Result<TaskId>& result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      report('E');
      report(static_cast<std::uint64_t>(error->_code));
    }
    else
    {
      report('A');
      report(static_cast<std::uint64_t>(std::get<TaskId>(result)));
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
      throw std::runtime_error("registration worker continuation deadline expired");
    }
    char value{};
    ssize_t count;
    do
    {
      count = ::read(STDIN_FILENO, &value, 1);
    } while (count == -1 && errno == EINTR);
    if (count == -1)
    {
      throw std::runtime_error("registration worker continuation failed");
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

  class StopAfterClaimFile : public runtime::TaskRegistrationObserver
  {
  public:
    OperationResult claim_file_ready(TaskId) override
    {
      stop_for_kill();
    }
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
      return Error{ErrorCode::IoError, "Injected registration I/O failure", "test.registration"};
    }

    std::string _mode;
    bool _failed{};
  };
} // namespace

int main(int argc, char** argv)
{
  try
  {
    if (argc != 8)
    {
      throw std::runtime_error("usage: native_registration_worker mode directory actor name doc checkpoint index");
    }
    ::alarm(9);
    const std::string mode{argv[1]};
    const auto actor = static_cast<TaskId>(std::stoul(argv[3]));
    const auto point = static_cast<mapped::JournalCheckpoint>(std::stoul(argv[6]));
    const auto index = std::stoull(argv[7]);
    StopAt stop(point, index);

    if (mode == "recover")
    {
      // Do not attach a public Session first: that would repair the journal
      // before this process installs its selected recovery checkpoint.
      auto controller = require_value(mapped::SessionController::open_existing(argv[2]));
      auto guard = require_value(controller->acquire_mutation());
      auto journal = require_value(mapped::MutationJournal::open(controller, guard, &stop));
      require_value(journal.recover(guard));
      throw std::runtime_error("recovery did not reach the requested checkpoint");
    }

    auto session = require_value(Session::attach(argv[2]));
    TaskDescriptor descriptor;
    descriptor._name = argv[4];
    descriptor._doc = argv[5];
    if (mode == "inspect" || mode == "inspect_attach")
    {
      auto registry = session.registry();
      const auto task = require_value(registry.find_task(descriptor._name));
      const auto recorded = require_value(registry.describe_task(task));
      const auto metadata = require_value(registry.metadata(task));
      if (recorded._doc != descriptor._doc || metadata._createdBy != actor)
      {
        throw std::runtime_error("independent reader found inconsistent Task metadata");
      }
      std::uint64_t incarnation{};
      if (mode == "inspect_attach")
      {
        auto attached = require_value(session.attach_task(task));
        incarnation = attached.claim()._incarnation;
        if (require_value(attached.next_publication_id())._sequence != 1)
        {
          throw std::runtime_error("newly registered Task has unexpected issuance state");
        }
      }
      report('I');
      report(static_cast<std::uint64_t>(task));
      report(static_cast<std::uint64_t>(metadata._createdBy));
      report(metadata._revision);
      report(static_cast<std::uint64_t>(recorded._kind));
      report(recorded._process ? static_cast<std::uint64_t>(*recorded._process) : 0);
      report(incarnation);
      return 0;
    }

    if (mode == "register")
    {
      auto owner = require_value(session.attach_task(actor));
      auto controller = require_value(mapped::SessionController::open_existing(argv[2]));
      report('R');
      wait_for_parent();
      {
        // The parent holds the gate while releasing both contenders. This
        // handshake proves each reached the shared boundary before it opens.
        auto gate = controller->try_acquire_mutation();
        const auto* error = std::get_if<Error>(&gate);
        report(error && error->_code == ErrorCode::OwnershipBusy ? 'B' : 'G');
      }
      report_result(owner.registry().register_task(descriptor));
      wait_for_parent();
      return 0;
    }

    // The same internal function implements public Registry::register_task.
    // A real attached actor is retained while private hooks qualify failures.
    auto state = std::make_shared<runtime::SessionState>();
    state->_directory = std::filesystem::canonical(argv[2]);
    state->_controller = require_value(mapped::SessionController::open_existing(argv[2]));
    auto owner = require_value(runtime::attach_runtime_task(state, actor));
    if (mode == "claim")
    {
      StopAfterClaimFile claim;
      require_value(runtime::runtime_register_task(state, owner, descriptor, nullptr, &claim));
      throw std::runtime_error("registration did not reach claim-file checkpoint");
    }
    if (mode == "crash")
    {
      require_value(runtime::runtime_register_task(state, owner, descriptor, &stop));
      throw std::runtime_error("registration did not reach journal checkpoint");
    }
    if (mode == "io_before" || mode == "io_after" || mode == "io_unknown")
    {
      IoFailure failure(mode);
      report_result(runtime::runtime_register_task(state, owner, descriptor, &failure));
      wait_for_parent();
      report_result(runtime::runtime_register_task(state, owner, descriptor));
      wait_for_parent();
      return 0;
    }
    throw std::runtime_error("unknown registration worker mode");
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
