#include "coordination_descriptor.h"

#include "native_file_state.h"
#include "performance_diagnostics.h"

#include <cerrno>
#include <cstdint>
#include <mutex>
#include <utility>
#include <variant>

#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace angaria::detail::mapped
{
  struct CoordinationRegistry
  {
    CoordinationRegistry();

    const std::uint64_t _ownerPid{static_cast<std::uint64_t>(::getpid())};
    std::mutex _mutex;
    CoordinationDescriptor* _head{};
    int _registrationError{};
    bool _inheritedChild{};
  };

  namespace
  {
    CoordinationRegistry* descriptorRegistry{};

    void before_fork()
    {
      descriptorRegistry->_mutex.lock();
    }

    void after_fork_parent()
    {
      descriptorRegistry->_mutex.unlock();
    }

    void after_fork_child()
    {
      for (auto descriptor = descriptorRegistry->_head; descriptor != nullptr;
           descriptor = descriptor->_next)
      {
        ::close(descriptor->_fd);
        descriptor->_fd = -1;
        descriptor->_linked = false;
        descriptor->_exclusiveClaim = false;
      }
      descriptorRegistry->_head = nullptr;
      descriptorRegistry->_inheritedChild = true;
      descriptorRegistry->_mutex.unlock();
    }

    CoordinationRegistry& descriptors()
    {
      // Handlers cannot be unregistered. Retain their registry through static
      // destruction, including descriptors acquired after initialization.
      static auto* const registry = new CoordinationRegistry;
      return *registry;
    }
  } // namespace

  CoordinationRegistry::CoordinationRegistry()
  {
    descriptorRegistry = this;
    _registrationError = ::pthread_atfork(before_fork, after_fork_parent, after_fork_child);
  }

  CoordinationDescriptor::~CoordinationDescriptor()
  {
    if (_registry == nullptr)
    {
      return;
    }
    if (_registry->_ownerPid != static_cast<std::uint64_t>(::getpid()))
    {
      // The child handler already closed registered FDs. Never acquire an
      // inherited process-local mutex during cleanup.
      return;
    }
    std::lock_guard guard(_registry->_mutex);
    if (_linked)
    {
      auto link = &_registry->_head;
      while (*link != nullptr && *link != this)
      {
        link = &(*link)->_next;
      }
      if (*link == this)
      {
        *link = _next;
      }
    }
    if (_fd != -1)
    {
      ::close(_fd);
    }
  }

  Result<std::unique_ptr<CoordinationDescriptor>> open_coordination(
      int directory, const std::filesystem::path& name, const std::filesystem::path& diagnostic,
      bool isDirectory, bool create)
  {
    auto& registry = descriptors();
    if (registry._ownerPid != static_cast<std::uint64_t>(::getpid()))
    {
      return storage_error(ErrorCode::WrongOwner, diagnostic, "Coordination resources require exec after fork");
    }
    if (name.empty() || name.native().find(std::filesystem::path::value_type{}) != name.native().npos)
    {
      return storage_error(ErrorCode::InvalidArgument, diagnostic, "Coordination path is empty or contains a null character");
    }
    if (registry._registrationError != 0)
    {
      return native_io_error(diagnostic, "register coordination fork handlers", registry._registrationError);
    }
    auto descriptor = std::make_unique<CoordinationDescriptor>();
    descriptor->_registry = &registry;
    // Fork must not occur between open and descriptor registration.
    std::lock_guard guard(registry._mutex);
    int flags = O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    flags |= isDirectory ? O_RDONLY | O_DIRECTORY : O_RDWR;
    if (create)
    {
      flags |= O_CREAT | O_EXCL;
    }
    do
    {
      descriptor->_fd = ::openat(directory, name.c_str(), flags, 0600);
    } while (descriptor->_fd == -1 && errno == EINTR);
    if (descriptor->_fd == -1)
    {
      if (create && errno == EEXIST)
      {
        return storage_error(ErrorCode::Conflict, diagnostic, "Coordination file already exists");
      }
      return native_io_error(diagnostic, "open coordination resource", errno);
    }
    struct stat info{};
    ANGARIA_DIAGNOSTIC_COUNT(FileStatCalls, 1);
    if (::fstat(descriptor->_fd, &info) == -1)
    {
      return native_io_error(diagnostic, "stat coordination resource", errno);
    }
    if ((isDirectory && !S_ISDIR(info.st_mode)) ||
        (!isDirectory && (!S_ISREG(info.st_mode) || info.st_nlink != 1)))
    {
      return storage_error(ErrorCode::IoError, diagnostic, "Invalid coordination file type or link count");
    }
    descriptor->_identity = stat_identity(info);
    descriptor->_next = registry._head;
    registry._head = descriptor.get();
    descriptor->_linked = true;
    return descriptor;
  }

  bool coordination_busy(int error) noexcept
  {
    return error == EWOULDBLOCK || error == EAGAIN;
  }

  int lock_coordination(int descriptor, int operation) noexcept
  {
    int result;
    do
    {
      result = ::flock(descriptor, operation);
    } while (result == -1 && errno == EINTR);
    return result;
  }

  bool owns_coordination(const CoordinationDescriptor& descriptor) noexcept
  {
    // atfork closes and invalidates every inherited registered descriptor. This
    // local ownership probe needs no syscall, mutex, or Session resource scan.
    return descriptor._registry != nullptr && !descriptor._registry->_inheritedChild && descriptor._fd != -1;
  }

  OperationResult claim_coordination(CoordinationDescriptor& descriptor,
                                     const std::filesystem::path& diagnostic)
  {
    if (!owns_coordination(descriptor))
    {
      return storage_error(ErrorCode::WrongOwner, diagnostic, "Coordination claim belongs to another process");
    }
    auto& registry = *descriptor._registry;
    std::lock_guard guard(registry._mutex);
    for (auto other = registry._head; other != nullptr; other = other->_next)
    {
      if (other->_exclusiveClaim && other->_identity == descriptor._identity)
      {
        return storage_error(ErrorCode::OwnershipBusy, diagnostic, "Native claim is already held in this process");
      }
    }
    if (lock_coordination(descriptor._fd, LOCK_EX | LOCK_NB) == -1)
    {
      const auto error = errno;
      if (coordination_busy(error))
      {
        return storage_error(ErrorCode::OwnershipBusy, diagnostic, "Native claim is busy");
      }
      return native_io_error(diagnostic, "acquire native claim", error);
    }
    descriptor._exclusiveClaim = true;
    return std::monostate{};
  }
} // namespace angaria::detail::mapped
