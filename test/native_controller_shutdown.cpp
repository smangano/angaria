#include "native_atomic.h"
#include "native_file.h"
#include "session_controller.h"

#include <cerrno>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;
  namespace fs = std::filesystem;

  template <typename T>
  T require_value(Result<T> result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    return std::get<T>(std::move(result));
  }

  int descriptor_for(const fs::path& path)
  {
    struct stat expected{};
    if (::stat(path.c_str(), &expected) == -1)
    {
      throw std::runtime_error("could not stat shutdown descriptor target");
    }
    for (const auto& entry : fs::directory_iterator("/dev/fd"))
    {
      const int fd = std::stoi(entry.path().filename().string());
      struct stat actual{};
      if (::fstat(fd, &actual) == 0 && actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino)
      {
        return fd;
      }
    }
    throw std::runtime_error("shutdown owner did not retain its descriptor");
  }

  bool descriptor_closed(int fd)
  {
    errno = 0;
    return ::fcntl(fd, F_GETFD) == -1 && errno == EBADF;
  }

  struct ShutdownStorage
  {
    ~ShutdownStorage()
    {
      // This owner is constructed before main, but the library registries are
      // first populated during main. Its cleanup therefore runs after any
      // destructible function-local registries would have been destroyed.
      if (_ready)
      {
        // Exercise the registered fork callbacks at the same late boundary.
        // No guard is active and the child uses no inherited C++ owners.
        const auto child = ::fork();
        if (child == 0)
        {
          ::_exit(0);
        }
        int status{};
        pid_t result = -1;
        if (child > 0)
        {
          do
          {
            result = ::waitpid(child, &status, 0);
          } while (result == -1 && errno == EINTR);
        }
        if (child == -1 || result != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
          std::_Exit(2);
        }
      }

      _lease.reset();
      _controller.reset();
      if (_ready && (!descriptor_closed(_gateFd) || !descriptor_closed(_claimFd)))
      {
        std::_Exit(3);
      }
      std::error_code error;
      if (!_root.empty())
      {
        fs::remove_all(_root, error);
      }
      if (error)
      {
        std::_Exit(4);
      }
      if (_ready)
      {
        constexpr char complete[] = "controller shutdown cleanup passed\n";
        if (::write(STDOUT_FILENO, complete, sizeof(complete) - 1) != sizeof(complete) - 1)
        {
          std::_Exit(5);
        }
      }
    }

    fs::path _root;
    std::shared_ptr<mapped::SessionController> _controller;
    std::shared_ptr<mapped::TaskLease> _lease;
    int _gateFd{-1};
    int _claimFd{-1};
    bool _ready{};
  };

  ShutdownStorage shutdown_storage;
} // namespace

int main()
{
  try
  {
    auto pattern = (fs::temp_directory_path() / "angaria-shutdown-XXXXXX").string();
    const auto directory = ::mkdtemp(pattern.data());
    if (directory == nullptr)
    {
      throw std::runtime_error("could not create shutdown-test directory");
    }
    shutdown_storage._root = directory;
    fs::create_directory(shutdown_storage._root / "claims");
    mapped::SuperblockConfiguration configuration;
    configuration._session = {0x123456789abcdef0, 0xfedcba9876543210};
    configuration._liveAbiProfile = require_value(mapped::qualify_native_profile())._liveAbiProfile;
    configuration._maxLogBytes = 65536;
    configuration._limits = CapacityLimits::empty().tasks(1);
    const auto layout = mapped::make_fixed_layout(configuration._limits);
    if (!layout)
    {
      throw std::runtime_error("invalid shutdown-test layout");
    }
    {
      auto file = require_value(mapped::NativeFile::create_exclusive(shutdown_storage._root / "session.ang",
                                                                    configuration._maxLogBytes));
      require_value(file.resize(layout->_arenaBegin));
      require_value(file.write_exact(0, require_value(mapped::encode_initial_superblock(configuration))));
      require_value(mapped::NativeFile::create_exclusive(shutdown_storage._root / "mutation.lock"));
    }
    shutdown_storage._controller = require_value(mapped::SessionController::open_existing(shutdown_storage._root));
    {
      auto guard = require_value(shutdown_storage._controller->acquire_mutation());
      require_value(shutdown_storage._controller->create_task_claim_file(guard, static_cast<TaskId>(1)));
    }
    shutdown_storage._lease = require_value(shutdown_storage._controller->try_claim_task(static_cast<TaskId>(1)));
    shutdown_storage._gateFd = descriptor_for(shutdown_storage._root / "mutation.lock");
    shutdown_storage._claimFd = descriptor_for(shutdown_storage._root / "claims/task-1.lock");
    shutdown_storage._ready = true;
    // Do not reset the global owners here: normal process exit is the test.
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
