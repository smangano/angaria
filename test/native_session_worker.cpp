// Independent exec worker for test_runtime_session.cpp. A fresh process tests
// persisted Session behavior without sharing the parent controller or caches.
// Stdout is a test-only pipe protocol: A=attached/inspected, B=busy/conflict,
// C=created, D=detached status, F=injected attachment failure, R=paused claim,
// S=status reader ready, E/G=unexpected errors; some modes then send native u64s.
// Closing the stdin pipe lets a held owner exit normally; SIGKILL tests omit it.

#include "process_support.h"
#include "result_helpers.h"
#include "angaria/runtime.h"
#include "runtime_state.h"

#include <cerrno>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
  using namespace angaria;

  using angaria::test_support::require_value;

  using angaria::test_support::has_error;

  void report(const void* data, std::size_t length)
  {
    angaria::test_support::write_all(STDOUT_FILENO, data, length, "session worker report failed");
  }

  void report(char value)
  {
    report(&value, 1);
  }

  void report(std::uint64_t value)
  {
    report(&value, sizeof(value));
  }

  void wait_for_release()
  {
    pollfd event{STDIN_FILENO, POLLIN, 0};
    int result;
    do
    {
      result = ::poll(&event, 1, 5000);
    } while (result == -1 && errno == EINTR);
    if (result <= 0)
    {
      throw std::runtime_error("session worker release deadline expired");
    }
    char ignored{};
    ssize_t count;
    do
    {
      count = ::read(STDIN_FILENO, &ignored, 1);
    } while (count == -1 && errno == EINTR);
    if (count == -1)
    {
      throw std::runtime_error("session worker release failed");
    }
  }

  // Stop after acquiring native ownership but before any new incarnation/intent
  // is installed. The public API never supplies this private observation hook.
  class PausedAttachment : public angaria::detail::TaskAttachmentObserver
  {
  public:
    explicit PausedAttachment(bool fail) : _fail(fail)
    {
    }

    OperationResult after_native_claim() override
    {
      report('R');
      wait_for_release();
      if (_fail)
      {
        return Error{ErrorCode::IoError, "Injected precommit attachment failure", "test.attachment"};
      }
      return std::monostate{};
    }

  private:
    bool _fail;
  };
} // namespace

int main(int argc, char** argv)
{
  try
  {
    if (argc != 3 && argc != 4)
    {
      throw std::runtime_error("usage: native_session_worker create|inspect|try_claim|hold_issue|hold_children|fork_reject|await_status|pause_attach|fail_attach directory [task-or-child-count]");
    }
    const std::string mode{argv[1]};
    if (mode == "create")
    {
      // Mirror the parent fixture so two fresh processes can contend for the
      // same creation path. Keep the winning creator alive until the parent
      // has inspected both reports, preventing accidental sequential reuse.
      SessionOptions options;
      options._name = "Runtime";
      options._applicationVersion = "App-2.5";
      options._directory = argv[2];
      options._maxLogBytes = 1048576;
      options._limits = CapacityLimits::empty().tasks(4);
      TaskDescriptor creator;
      creator._name = "Creator";
      creator._doc = "Creator process";
      auto result = Session::create(options, creator);
      if (has_error(result, ErrorCode::Conflict))
      {
        report('B');
        return 0;
      }
      auto created = require_value(std::move(result));
      report('C');
      wait_for_release();
      return 0;
    }

    auto session = require_value(Session::attach(argv[2]));
    if (mode == "inspect")
    {
      // Inspect without attach_task: reading an existing Registry does not
      // require taking the creator's exclusive claim from its current owner.
      const auto metadata = require_value(session.metadata());
      const auto options = require_value(session.options());
      auto registry = session.registry();
      const auto creator = require_value(registry.find_task("Creator"));
      const auto definition = require_value(registry.describe_task(creator));
      if (metadata._session != session.identity() || metadata._createdBy != TaskId{1} ||
          metadata._angariaVersion.empty() || metadata._applicationVersion != "App-2.5" ||
          metadata._creatorUser.empty() || options._name != "Runtime" ||
          options._directory != std::filesystem::canonical(argv[2]) || options._limits._tasks != 4 ||
          definition._name != "Creator" || definition._kind != TaskKind::Process ||
          definition._doc != "Creator process")
      {
        throw std::runtime_error("independent session inspection disagreed with persisted metadata");
      }
      report('A');
      return 0;
    }

    const auto id = argc == 4 && mode != "hold_children" ?
        static_cast<TaskId>(std::stoul(argv[3])) : TaskId{1};
    if (mode == "await_status")
    {
      // Attach only the Session before a replacement starts. When signaled,
      // prove the replacement still owns the gate, then let the public status
      // query wait for that replacement's commit or failed-attempt cleanup.
      auto controller = require_value(angaria::detail::mapped::SessionController::open_existing(argv[2]));
      report('S');
      wait_for_release();
      {
        auto gate = controller->try_acquire_mutation();
        report(has_error(gate, ErrorCode::OwnershipBusy) ? 'B' : 'G');
      }
      const auto status = require_value(session.registry().task_status(id));
      report(status._attached ? 'A' : 'D');
      report(status._incarnation);
      report(status._processId);
      return 0;
    }
    if (mode == "pause_attach" || mode == "fail_attach")
    {
      auto state = std::make_shared<angaria::detail::SessionState>();
      state->_directory = std::filesystem::canonical(argv[2]);
      state->_controller = require_value(angaria::detail::mapped::SessionController::open_existing(argv[2]));
      PausedAttachment pause(mode == "fail_attach");
      auto result = angaria::detail::attach_runtime_task(state, id, false, {}, &pause);
      if (mode == "fail_attach")
      {
        if (!has_error(result, ErrorCode::IoError))
        {
          throw std::runtime_error("attachment accepted an injected precommit failure");
        }
        report('F');
        wait_for_release();
        return 0;
      }
      auto task = require_value(std::move(result));
      report('A');
      report(task->_claim._incarnation);
      report(static_cast<std::uint64_t>(::getpid()));
      wait_for_release();
      return 0;
    }
    auto attached = session.attach_task(id);
    if (mode == "try_claim")
    {
      report(has_error(attached, ErrorCode::OwnershipBusy) ? 'B' :
                 std::holds_alternative<Task>(attached) ? 'A' : 'E');
      return 0;
    }
    auto task = require_value(std::move(attached));
    if (mode == "hold_children")
    {
      // These are separately registered logical Thread Tasks attached on this
      // native thread. Retaining every wrapper keeps their parent claim alive
      // until normal release or until the parent test kills this whole process.
      const auto count = argc == 4 ? std::stoul(argv[3]) : 1;
      std::vector<Task> children;
      for (unsigned index = 0; index != count; ++index)
      {
        children.push_back(require_value(session.attach_task(static_cast<TaskId>(index + 2))));
      }
      report('A');
      report(task.claim()._incarnation);
      report(static_cast<std::uint64_t>(children.size()));
      wait_for_release();
      return 0;
    }
    if (mode == "hold_issue")
    {
      // Report only after both issuance transactions complete. Killing after
      // this report leaves persisted counters and stale diagnostics to inspect.
      const auto first = require_value(task.next_publication_id());
      const auto second = require_value(task.next_publication_id());
      report('A');
      report(task.claim()._incarnation);
      report(first._sequence);
      report(second._sequence);
      wait_for_release();
      return 0;
    }
    if (mode == "fork_reject")
    {
      // Unlike the usual fresh-exec path, this mode inherits handles only to
      // verify that their use fails. The child uses _exit rather than running
      // inherited owner cleanup; applications exec before normal library use.
      const auto child = ::fork();
      if (child == -1)
      {
        throw std::runtime_error("session worker fork failed");
      }
      if (child == 0)
      {
        bool rejected = has_error(session.options(), ErrorCode::WrongOwner) &&
                        has_error(session.metadata(), ErrorCode::WrongOwner) &&
                        has_error(session.attach_task(id), ErrorCode::WrongOwner) &&
                        has_error(task.next_publication_id(), ErrorCode::WrongOwner);
        try
        {
          static_cast<void>(session.identity());
          rejected = false;
        }
        catch (const std::logic_error&)
        {
        }
        try
        {
          static_cast<void>(task.claim());
          rejected = false;
        }
        catch (const std::logic_error&)
        {
        }
        ::_exit(rejected ? 0 : 1);
      }
      int status{};
      pid_t result;
      do
      {
        result = ::waitpid(child, &status, 0);
      } while (result == -1 && errno == EINTR);
      if (result != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
      {
        throw std::runtime_error("fork child accepted inherited public Session/Task handles");
      }
      report('A');
      wait_for_release();
      return 0;
    }
    throw std::runtime_error("unknown session worker mode");
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
