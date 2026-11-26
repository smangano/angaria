#pragma once

#include "angaria/results.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace angaria::test_support
{
  // Test harness transport only: these native words cross pipes between exec
  // processes on the same qualified host. They are not Angaria wire encoders.
  inline void write_all(int descriptor, const void* data, std::size_t length,
                        const char* failure)
  {
    const auto* bytes = static_cast<const char*>(data);
    while (length != 0)
    {
      const auto count = ::write(descriptor, bytes, length);
      if (count > 0)
      {
        bytes += count;
        length -= static_cast<std::size_t>(count);
      }
      else if (count != -1 || errno != EINTR)
      {
        throw std::runtime_error(failure);
      }
    }
  }

  inline void report(const void* data, std::size_t length)
  {
    write_all(STDOUT_FILENO, data, length, "registration worker report failed");
  }

  inline void report(char value)
  {
    report(&value, sizeof(value));
  }

  inline void report(std::uint64_t value)
  {
    report(&value, sizeof(value));
  }

  template <typename T>
  std::uint64_t reported_identity(T id)
  {
    return static_cast<std::uint64_t>(id);
  }

  inline std::uint64_t reported_identity(SchemaId id)
  {
    return (static_cast<std::uint64_t>(id._type) << 32) | id._version;
  }

  template <typename T>
  void report_result(const Result<T>& result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      report('E');
      report(static_cast<std::uint64_t>(error->_code));
    }
    else
    {
      report('A');
      report(reported_identity(std::get<T>(result)));
    }
  }

  inline void wait_for_parent()
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

  [[noreturn]] inline void stop_for_kill()
  {
    report('R');
    for (;;)
    {
      ::pause();
    }
  }

  inline void close_descriptor(int& descriptor)
  {
    if (descriptor != -1)
    {
      // Do not retry close: a descriptor number can already have been reused.
      ::close(std::exchange(descriptor, -1));
    }
  }

  inline void close_pipe_fds(const std::array<int, 2>& output, const std::array<int, 2>& input)
  {
    for (const auto descriptor : {output[0], output[1], input[0], input[1]})
    {
      if (descriptor != -1)
      {
        ::close(descriptor);
      }
    }
  }

  struct ReportMessages
  {
    const char* _deadline;
    const char* _notReady;
    const char* _closed;
  };

  inline void receive_exact(int descriptor, void* data, std::size_t length, ReportMessages messages)
  {
    auto* bytes = static_cast<char*>(data);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (length != 0)
    {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()).count();
      if (remaining <= 0)
      {
        throw std::runtime_error(messages._deadline);
      }
      pollfd event{descriptor, POLLIN, 0};
      const auto ready = ::poll(&event, 1, static_cast<int>(remaining));
      if (ready == -1 && errno == EINTR)
      {
        continue;
      }
      if (ready <= 0)
      {
        throw std::runtime_error(messages._notReady);
      }
      const auto count = ::read(descriptor, bytes, length);
      if (count > 0)
      {
        bytes += count;
        length -= static_cast<std::size_t>(count);
      }
      else if (count != -1 || errno != EINTR)
      {
        throw std::runtime_error(messages._closed);
      }
    }
  }

  enum class WaitDelay
  {
    Poll,
    Sleep
  };

  inline int wait_for_exit(pid_t pid, const char* waitFailure, const char* deadlineFailure,
                            WaitDelay delay = WaitDelay::Poll)
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    for (;;)
    {
      int status{};
      const auto result = ::waitpid(pid, &status, WNOHANG);
      if (result == pid)
      {
        return status;
      }
      if (result == -1 && errno != EINTR)
      {
        throw std::runtime_error(waitFailure);
      }
      if (std::chrono::steady_clock::now() >= deadline)
      {
        throw std::runtime_error(deadlineFailure);
      }
      if (delay == WaitDelay::Poll)
      {
        ::poll(nullptr, 0, 1);
      }
      else
      {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
    }
  }

  inline int exit_code(int status)
  {
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  }

  struct TerminationMessages
  {
    const char* _termination;
    const char* _wait;
    const char* _deadline;
    const char* _unexpected;
  };

  inline void terminate(pid_t& pid, TerminationMessages messages)
  {
    if (::kill(pid, SIGKILL) == -1)
    {
      throw std::runtime_error(messages._termination);
    }
    const auto status = wait_for_exit(pid, messages._wait, messages._deadline);
    pid = -1;
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
    {
      throw std::runtime_error(messages._unexpected);
    }
  }

  // Block only this thread's SIGPIPE. Preserve a previously pending signal and
  // consume only one generated by the guarded write; never change disposition.
  class BlockedSigpipe
  {
  public:
    BlockedSigpipe(const char* blockFailure, const char* consumeFailure)
        : _consumeFailure(consumeFailure)
    {
      sigemptyset(&_signal);
      sigaddset(&_signal, SIGPIPE);
      const auto error = ::pthread_sigmask(SIG_BLOCK, &_signal, &_previous);
      if (error != 0)
      {
        throw std::system_error(error, std::generic_category(), blockFailure);
      }
      sigset_t pending;
      if (::sigpending(&pending) == -1)
      {
        const auto pendingError = errno;
        ::pthread_sigmask(SIG_SETMASK, &_previous, nullptr);
        throw std::system_error(pendingError, std::generic_category(), "inspect pending SIGPIPE");
      }
      _wasPending = sigismember(&pending, SIGPIPE) == 1;
    }

    ~BlockedSigpipe()
    {
      ::pthread_sigmask(SIG_SETMASK, &_previous, nullptr);
    }

    BlockedSigpipe(const BlockedSigpipe&) = delete;
    BlockedSigpipe& operator=(const BlockedSigpipe&) = delete;

    void consume_generated()
    {
      if (_wasPending)
      {
        return;
      }
      sigset_t pending;
      if (::sigpending(&pending) == -1)
      {
        throw std::system_error(errno, std::generic_category(), "inspect generated SIGPIPE");
      }
      // SIG_IGN can leave nothing pending despite an EPIPE result.
      if (sigismember(&pending, SIGPIPE) == 1)
      {
        int received{};
        int error;
        do
        {
          error = ::sigwait(&_signal, &received);
        } while (error == EINTR);
        if (error != 0)
        {
          throw std::system_error(error, std::generic_category(), _consumeFailure);
        }
      }
    }

  private:
    sigset_t _signal{};
    sigset_t _previous{};
    bool _wasPending{};
    const char* _consumeFailure;
  };

  struct WriteAttempt
  {
    ssize_t _count;
    int _error;
  };

  inline WriteAttempt write_continuation(int descriptor)
  {
    const char continuation = 'C';
    ssize_t count;
    do
    {
      count = ::write(descriptor, &continuation, 1);
    } while (count == -1 && errno == EINTR);
    return WriteAttempt{count, errno};
  }

  inline void proceed(int descriptor, const char* failure)
  {
    if (write_continuation(descriptor)._count != 1)
    {
      throw std::runtime_error(failure);
    }
  }

  inline void proceed_without_sigpipe(int descriptor, const char* failure,
                                       const char* blockFailure, const char* consumeFailure)
  {
    BlockedSigpipe blocked(blockFailure, consumeFailure);
    const auto attempt = write_continuation(descriptor);
    if (attempt._count == -1 && attempt._error == EPIPE)
    {
      blocked.consume_generated();
    }
    if (attempt._count != 1)
    {
      throw std::system_error(attempt._count == -1 ? attempt._error : EIO,
                              std::generic_category(), failure);
    }
  }
} // namespace angaria::test_support
