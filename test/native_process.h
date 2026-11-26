#pragma once

#include "process_support.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <unistd.h>

extern char** environ;

namespace angaria::test_support
{
  // Test coordination only. Each child executes independently and exchanges
  // native report words over pipes; these pipes never carry Angaria messages.
  class NativeProcess
  {
  public:
    explicit NativeProcess(const std::vector<std::string>& arguments)
    {
      if (arguments.empty())
      {
        throw std::invalid_argument("Native process needs an executable");
      }
      std::array<int, 2> output{-1, -1};
      std::array<int, 2> input{-1, -1};
      try
      {
        if (::pipe(output.data()) == -1 || ::pipe(input.data()) == -1)
        {
          throw std::system_error(errno, std::generic_category(), "Create native process pipes");
        }
        for (const auto descriptor : {output[0], output[1], input[0], input[1]})
        {
          if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == -1)
          {
            throw std::system_error(errno, std::generic_category(), "Mark native process pipe close-on-exec");
          }
        }
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto& argument : arguments)
        {
          argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        auto error = ::posix_spawn_file_actions_init(&actions);
        if (error == 0)
        {
          error = ::posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
          error = error == 0 ? ::posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO) : error;
          for (const auto descriptor : {output[0], output[1], input[0], input[1]})
          {
            // A caller may have started with stdin/stdout closed, allowing a
            // pipe to reuse 0 or 1. Keep the final child endpoints open.
            if (descriptor != STDIN_FILENO && descriptor != STDOUT_FILENO)
            {
              error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, descriptor) : error;
            }
          }
          error = error == 0 ? ::posix_spawn(&_pid, argv.front(), &actions, nullptr, argv.data(), environ) : error;
          ::posix_spawn_file_actions_destroy(&actions);
        }
        if (error != 0)
        {
          throw std::system_error(error, std::generic_category(), "Spawn native process");
        }
      }
      catch (...)
      {
        close_pipe_fds(output, input);
        throw;
      }
      ::close(output[1]);
      ::close(input[0]);
      _output = output[0];
      _input = input[1];
    }

    ~NativeProcess()
    {
      if (_pid > 0)
      {
        ::kill(_pid, SIGKILL);
        try
        {
          (void)wait_for_exit(_pid, "Cannot reap native process", "Native process cleanup deadline expired");
        }
        catch (...)
        {
        }
      }
      close_descriptor(_output);
      close_descriptor(_input);
    }

    NativeProcess(const NativeProcess&) = delete;
    NativeProcess& operator=(const NativeProcess&) = delete;

    void receive_bytes(void* data, std::size_t bytes)
    {
      receive_exact(_output, data, bytes,
          {"Native process report deadline expired", "Native process report unavailable",
           "Native process closed before its report"});
    }

    template <typename T>
    T receive()
    {
      static_assert(std::is_trivially_copyable_v<T>);
      T result{};
      receive_bytes(&result, sizeof(result));
      return result;
    }

    void expect(char expected)
    {
      if (receive<char>() != expected)
      {
        throw std::runtime_error("Native process reported an unexpected stage");
      }
    }

    void proceed()
    {
      proceed_without_sigpipe(_input, "Native process continuation failed",
          "Cannot block native process SIGPIPE", "Cannot consume native process SIGPIPE");
    }

    void terminate()
    {
      if (_pid <= 0)
      {
        throw std::logic_error("Native process has already been reaped");
      }
      test_support::terminate(_pid,
          {"Cannot terminate native process", "Cannot reap terminated native process",
           "Native process termination deadline expired", "Native process was not killed"});
    }

    void wait_success()
    {
      if (_pid <= 0)
      {
        throw std::logic_error("Native process has already been reaped");
      }
      const auto status = wait_for_exit(_pid, "Cannot reap native process", "Native process exit deadline expired");
      _pid = -1;
      if (exit_code(status) != 0)
      {
        throw std::runtime_error("Native process exited unsuccessfully");
      }
    }

  private:
    pid_t _pid{-1};
    int _output{-1};
    int _input{-1};
  };
} // namespace angaria::test_support
