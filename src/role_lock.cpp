#include "role_lock.h"

#include <cerrno>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace angaria
{
  RoleBusy::RoleBusy(const char* message) : std::runtime_error(message)
  {
  }
} // namespace angaria

namespace angaria::detail
{
  std::mutex RoleLock::_registryMutex;
  RoleLock* RoleLock::_registryHead = nullptr;

  RoleLock::RoleLock(const std::filesystem::path& session, Role role)
      : _statePath(session / (role == Role::Producer ? "producer.state" : "consumer.state")),
        _identity{static_cast<std::uint16_t>(role == Role::Producer ? 1 : 2), 0,
                  static_cast<std::uint64_t>(::getpid())},
        _ownerPid(::getpid())
  {
    static std::once_flag forkHandlers;
    std::call_once(forkHandlers,
                   []
                   {
                     const int error = ::pthread_atfork(before_fork, after_fork_parent, after_fork_child);
                     if (error != 0)
                     {
                       throw std::system_error(error, std::generic_category(), "register role fork handlers");
                     }
                   });

    const auto lockPath = session / (role == Role::Producer ? "producer.lock" : "consumer.lock");
    std::lock_guard guard(_registryMutex);
    _fd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (_fd == -1)
    {
      throw std::system_error(errno, std::generic_category(), "open role lock");
    }

    try
    {
      struct stat info{};
      if (::fstat(_fd, &info) == -1)
      {
        throw std::system_error(errno, std::generic_category(), "stat role lock");
      }
      if (!S_ISREG(info.st_mode) || info.st_nlink != 1)
      {
        throw std::runtime_error("invalid role lock file");
      }
      int result;
      do
      {
        result = ::flock(_fd, LOCK_EX | LOCK_NB);
      } while (result == -1 && errno == EINTR);
      if (result == -1)
      {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
        {
          throw RoleBusy(role == Role::Producer ? "producer role is busy" : "consumer role is busy");
        }
        throw std::system_error(errno, std::generic_category(), "claim role");
      }
    }
    catch (...)
    {
      ::close(_fd);
      _fd = -1;
      throw;
    }

    _next = _registryHead;
    _registryHead = this;
  }

  RoleLock::~RoleLock()
  {
    std::lock_guard guard(_registryMutex);
    auto link = &_registryHead;
    while (*link != nullptr && *link != this)
    {
      link = &(*link)->_next;
    }
    if (*link == this)
    {
      *link = _next;
    }
    if (_fd != -1)
    {
      ::close(_fd);
    }
  }

  void RoleLock::before_fork()
  {
    _registryMutex.lock();
  }

  void RoleLock::after_fork_parent()
  {
    _registryMutex.unlock();
  }

  void RoleLock::after_fork_child()
  {
    for (auto role = _registryHead; role != nullptr; role = role->_next)
    {
      ::close(role->_fd);
      role->_fd = -1;
    }
    _registryHead = nullptr;
    _registryMutex.unlock();
  }

  void RoleLock::check_owner() const
  {
    if (_fd == -1 || ::getpid() != _ownerPid)
    {
      throw std::logic_error("attach a new endpoint in the child process");
    }
  }

  void RoleLock::activate()
  {
    check_owner();
    std::uint64_t incarnation{};
    if (std::filesystem::exists(_statePath))
    {
      std::ifstream state(_statePath);
      std::string magic;
      std::uint16_t taskId{};
      std::uint64_t pid{};
      state >> magic >> taskId >> incarnation >> pid;
      if (!state || magic != "ANGARIA_ROLE_1" || taskId != _identity._taskId || incarnation == 0 || pid == 0)
      {
        throw std::runtime_error("invalid role state");
      }
      state >> std::ws;
      if (!state.eof())
      {
        throw std::runtime_error("invalid role state tail");
      }
    }
    if (incarnation == std::numeric_limits<std::uint64_t>::max())
    {
      throw std::runtime_error("role incarnation exhausted");
    }

    auto temporary = _statePath;
    temporary += ".tmp";
    std::ofstream state(temporary, std::ios::trunc);
    state << "ANGARIA_ROLE_1 " << _identity._taskId << ' ' << incarnation + 1 << ' ' << _identity._pid
          << '\n';
    state.close();
    if (!state)
    {
      throw std::runtime_error("write role state");
    }
    std::filesystem::rename(temporary, _statePath);
    _identity._incarnation = incarnation + 1;
  }

  RoleIdentity RoleLock::identity() const
  {
    check_owner();
    return _identity;
  }
} // namespace angaria::detail
