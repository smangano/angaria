// Independent exec worker for test_session_controller.cpp. Pipe handshakes
// establish progress without assuming that elapsed time means a lock was reached:
// A = acquired/succeeded, B = busy, S = about to block, W = wrong-thread checks
// complete, and C = fork child is alive. Closing the release pipe permits orderly
// exit; crash cases kill an owner instead. This is a private test protocol.

#include "session_controller.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
  using namespace angaria;
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

  template <typename T>
  bool has_error(const Result<T>& result, ErrorCode code)
  {
    const auto* error = std::get_if<Error>(&result);
    return error != nullptr && error->_code == code;
  }

  void write_exact(int fd, const void* data, std::size_t length)
  {
    const auto* bytes = static_cast<const char*>(data);
    while (length != 0)
    {
      const auto count = ::write(fd, bytes, length);
      if (count > 0)
      {
        bytes += count;
        length -= static_cast<std::size_t>(count);
      }
      else if (count == -1 && errno == EINTR)
      {
        continue;
      }
      else
      {
        throw std::runtime_error("worker pipe write failed");
      }
    }
  }

  void report(int fd, char value)
  {
    write_exact(fd, &value, 1);
  }

  void wait_for_release(int fd)
  {
    pollfd event{fd, POLLIN, 0};
    int result;
    do
    {
      result = ::poll(&event, 1, 5000);
    } while (result == -1 && errno == EINTR);
    if (result <= 0)
    {
      throw std::runtime_error("worker release deadline expired");
    }
    char ignored{};
    ssize_t count;
    do
    {
      count = ::read(fd, &ignored, 1);
    } while (count == -1 && errno == EINTR);
    if (count == -1)
    {
      throw std::runtime_error("worker release read failed");
    }
  }

  int wait_for(pid_t child)
  {
    int status{};
    pid_t result;
    do
    {
      result = ::waitpid(child, &status, 0);
    } while (result == -1 && errno == EINTR);
    if (result != child || !WIFEXITED(status))
    {
      throw std::runtime_error("worker child failed");
    }
    return WEXITSTATUS(status);
  }
} // namespace

int main(int argc, char** argv)
{
  try
  {
    if (argc != 7)
    {
      throw std::runtime_error("expected mode, directory, report fd, release fd, and argument");
    }
    const std::string mode = argv[1];
    const int report_fd = std::stoi(argv[3]);
    const int release_fd = std::stoi(argv[4]);
    // Inspect inherited descriptor numbers before opening library resources,
    // which could otherwise reuse those numbers and obscure close-on-exec.
    if (mode == "check_closed")
    {
      const int first = std::stoi(argv[5]);
      const int second = std::stoi(argv[6]);
      errno = 0;
      const bool first_closed = ::fcntl(first, F_GETFD) == -1 && errno == EBADF;
      errno = 0;
      const bool second_closed = ::fcntl(second, F_GETFD) == -1 && errno == EBADF;
      report(report_fd, first_closed && second_closed ? 'A' : 'E');
      return 0;
    }

    auto controller = require_value(mapped::SessionController::open_existing(argv[2]));
    const auto task = static_cast<TaskId>(std::stoul(argv[5]));
    if (mode == "try_gate")
    {
      const auto result = controller->try_acquire_mutation();
      report(report_fd, has_error(result, ErrorCode::OwnershipBusy) ? 'B' :
                          std::holds_alternative<mapped::MutationGuard>(result) ? 'A' : 'E');
      return 0;
    }
    if (mode == "try_claim")
    {
      const auto result = controller->try_claim_task(task);
      report(report_fd, has_error(result, ErrorCode::OwnershipBusy) ? 'B' :
                          std::holds_alternative<std::shared_ptr<mapped::TaskLease>>(result) ? 'A' : 'E');
      return 0;
    }
    if (mode == "hold_claim")
    {
      auto claim = require_value(controller->try_claim_task(task));
      report(report_fd, 'A');
      wait_for_release(release_fd);
      return 0;
    }
    if (mode == "block_gate")
    {
      report(report_fd, 'S');
      auto guard = require_value(controller->acquire_mutation());
      report(report_fd, 'A');
      wait_for_release(release_fd);
      return 0;
    }

    // Acquire the lifetime claim before the operation gate. The following
    // modes retain both while the parent chooses orderly release or SIGKILL.
    auto claim = require_value(controller->try_claim_task(task));
    auto guard = require_value(controller->acquire_mutation());
    if (mode == "hold_both")
    {
      report(report_fd, 'A');
      wait_for_release(release_fd);
      return 0;
    }
    if (mode == "destroy_guard_wrong_thread")
    {
      std::thread wrong_thread([controller, guard = std::move(guard), report_fd, release_fd]() mutable
      {
        try
        {
          if (!has_error(guard.check_owner(), ErrorCode::WrongOwner) ||
              !has_error(controller->file(guard), ErrorCode::WrongOwner))
          {
            ::_exit(72);
          }
          report(report_fd, 'W');
          wait_for_release(release_fd);
        }
        catch (...)
        {
          ::_exit(72);
        }
        // Install only after the wrong-thread checks and handshake. Exit 73
        // therefore identifies termination during this explicit destruction,
        // without producing an intentional crash report or core dump.
        std::set_terminate([]
        {
          ::_exit(73);
        });
        guard = mapped::MutationGuard{};
        ::_exit(74);
      });
      wrong_thread.join();
      throw std::runtime_error("wrong-thread guard destruction unexpectedly returned");
    }
    if (mode == "fork_reject")
    {
      const auto child = ::fork();
      if (child == -1)
      {
        throw std::runtime_error("worker fork failed");
      }
      if (child == 0)
      {
        const bool rejected = has_error(controller->check_owner(), ErrorCode::WrongOwner) &&
                              has_error(guard.check_owner(), ErrorCode::WrongOwner) &&
                              has_error(claim->check_owner(), ErrorCode::WrongOwner) &&
                              has_error(controller->try_acquire_mutation(), ErrorCode::WrongOwner) &&
                              has_error(mapped::SessionController::open_existing(argv[2]), ErrorCode::WrongOwner);
        // This worker fork is quiescent. Cleanup must not unlock its parent's
        // shared open description or copied process-local mutex.
        guard = mapped::MutationGuard{};
        claim.reset();
        ::_exit(rejected ? 0 : 1);
      }
      if (wait_for(child) != 0)
      {
        throw std::runtime_error("fork child accepted inherited ownership");
      }
      report(report_fd, 'A');
      wait_for_release(release_fd);
      return 0;
    }
    if (mode == "fork_holder")
    {
      int order[2];
      if (::pipe(order) == -1)
      {
        throw std::runtime_error("worker ordering pipe failed");
      }
      const auto child = ::fork();
      if (child == -1)
      {
        throw std::runtime_error("worker fork failed");
      }
      if (child == 0)
      {
        ::close(order[1]);
        char start{};
        if (::read(order[0], &start, 1) != 1 || ::write(report_fd, "C", 1) != 1)
        {
          ::_exit(1);
        }
        ::close(order[0]);
        // Deliberately remain alive after the owner is killed. Only native
        // syscalls are used in this fork child; inherited C++ owners stay unused.
        pollfd event{release_fd, POLLIN, 0};
        const auto ready = ::poll(&event, 1, 5000);
        ::_exit(ready > 0 ? 0 : 1);
      }
      ::close(order[0]);
      const auto child_pid = static_cast<std::uint64_t>(child);
      write_exact(report_fd, &child_pid, sizeof(child_pid));
      write_exact(order[1], "S", 1);
      ::close(order[1]);
      wait_for_release(release_fd);
      if (wait_for(child) != 0)
      {
        throw std::runtime_error("fork holder child failed");
      }
      return 0;
    }
    throw std::runtime_error("unknown worker mode");
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
