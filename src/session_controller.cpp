#include "session_controller.h"

#include "native_atomic.h"
#include "native_file_state.h"

#include <cerrno>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>

#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace angaria::detail::mapped
{
  namespace
  {
    struct CoordinationDescriptor;

    // Only these descriptors can hold the kernel locks. The child closes its
    // inherited copies without LOCK_UN, which would unlock the parent's shared
    // open file description. The handlers perform no allocation or C++ cleanup.
    struct DescriptorRegistry
    {
      DescriptorRegistry();

      const std::uint64_t _ownerPid{static_cast<std::uint64_t>(::getpid())};
      std::mutex _mutex;
      CoordinationDescriptor* _head{};
      int _registrationError{};
    };

    DescriptorRegistry* descriptorRegistry{};

    struct CoordinationDescriptor
    {
      ~CoordinationDescriptor();
      int _fd{-1};
      DescriptorRegistry* _registry{};
      CoordinationDescriptor* _next{};
      bool _linked{};
      FileIdentity _identity{};
    };

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
      }
      descriptorRegistry->_head = nullptr;
      descriptorRegistry->_mutex.unlock();
    }

    DescriptorRegistry::DescriptorRegistry()
    {
      descriptorRegistry = this;
      _registrationError = ::pthread_atfork(before_fork, after_fork_parent, after_fork_child);
    }

    DescriptorRegistry& descriptors()
    {
      static DescriptorRegistry registry;
      return registry;
    }

    CoordinationDescriptor::~CoordinationDescriptor()
    {
      if (_registry == nullptr)
      {
        return;
      }
      if (_registry->_ownerPid != static_cast<std::uint64_t>(::getpid()))
      {
        // Registered descriptors were already closed by the child handler.
        // No inherited process-local mutex may be acquired here.
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

    FileIdentity stat_identity(const struct stat& info)
    {
      return {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino)};
    }

    Result<std::unique_ptr<CoordinationDescriptor>> open_coordination(
        int directory, const std::filesystem::path& name, const std::filesystem::path& diagnostic,
        bool isDirectory, bool create = false)
    {
      auto& registry = descriptors();
      if (registry._ownerPid != static_cast<std::uint64_t>(::getpid()))
      {
        return storage_error(ErrorCode::WrongOwner, diagnostic, "Coordination resources require exec after fork");
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

    OperationResult check_named_identity(int directory, const char* name, FileIdentity identity,
                                         const std::filesystem::path& path, bool isDirectory,
                                         bool singleLink = true)
    {
      struct stat info{};
      if (::fstatat(directory, name, &info, AT_SYMLINK_NOFOLLOW) == -1)
      {
        return native_io_error(path, "stat stable session resource", errno);
      }
      if (stat_identity(info) != identity || (isDirectory && !S_ISDIR(info.st_mode)) ||
          (!isDirectory && (!S_ISREG(info.st_mode) || (singleLink && info.st_nlink != 1))))
      {
        return storage_error(ErrorCode::IoError, path, "Stable session resource was replaced or changed type");
      }
      return std::monostate{};
    }

    struct ControllerRegistry
    {
      const std::uint64_t _ownerPid{static_cast<std::uint64_t>(::getpid())};
      std::mutex _mutex;
      std::map<std::pair<std::uint64_t, std::uint64_t>, std::weak_ptr<SessionController>> _controllers;
    };

    ControllerRegistry& controllers()
    {
      static ControllerRegistry registry;
      return registry;
    }

    std::string claim_name(TaskId task)
    {
      return "task-" + std::to_string(static_cast<std::uint16_t>(task)) + ".lock";
    }

    bool lock_busy(int error)
    {
      return error == EWOULDBLOCK || error == EAGAIN;
    }

    int lock_file(int fd, int operation)
    {
      int result;
      do
      {
        result = ::flock(fd, operation);
      } while (result == -1 && errno == EINTR);
      return result;
    }
  } // namespace

  struct SessionControllerState
  {
    std::uint64_t _ownerPid{};
    std::filesystem::path _directory;
    SuperblockConfiguration _configuration;
    FileIdentity _fileIdentity;
    NativeFile _file;
    std::unique_ptr<CoordinationDescriptor> _directoryFd;
    std::unique_ptr<CoordinationDescriptor> _claimsFd;
    std::unique_ptr<CoordinationDescriptor> _mutationFd;
    std::recursive_mutex _gateMutex;
    std::thread::id _gateOwner;
    std::mutex _claimMutex;
    std::map<TaskId, std::weak_ptr<TaskLease>> _claims;
    std::map<TaskId, FileIdentity> _claimIdentities;
  };

  struct MutationGuardState
  {
    ~MutationGuardState()
    {
      if (!_localLock.owns_lock())
      {
        return;
      }
      if (_controller->_state->_ownerPid != static_cast<std::uint64_t>(::getpid()))
      {
        // An inherited guard must not unlock either the parent's flock or a
        // copied mutex. Multithreaded children must exec/_exit before cleanup.
        _localLock.release();
        return;
      }
      if (_thread != std::this_thread::get_id())
      {
        // Moving a live mutex owner to another thread violates its contract.
        std::terminate();
      }
      if (_nativeLocked)
      {
        native_gate_fence();
        if (lock_file(_controller->_state->_mutationFd->_fd, LOCK_UN) == -1)
        {
          // Continuing with unknown gate ownership would permit unsafe reuse.
          std::terminate();
        }
        _controller->_state->_gateOwner = {};
      }
    }

    std::shared_ptr<SessionController> _controller;
    std::unique_lock<std::recursive_mutex> _localLock;
    std::thread::id _thread;
    bool _nativeLocked{};
  };

  struct TaskLeaseState
  {
    std::shared_ptr<SessionController> _controller;
    std::shared_ptr<TaskLease> _parent;
    TaskId _task{};
    std::unique_ptr<CoordinationDescriptor> _descriptor;
  };

  namespace
  {
    OperationResult check_resources(const SessionControllerState& state)
    {
      for (auto result : {
               check_named_identity(state._directoryFd->_fd, "mutation.lock", state._mutationFd->_identity,
                                     state._directory / "mutation.lock", false),
               check_named_identity(state._directoryFd->_fd, "claims", state._claimsFd->_identity,
                                     state._directory / "claims", true),
               check_named_identity(state._directoryFd->_fd, "session.ang", state._fileIdentity,
                                     state._directory / "session.ang", false, false)})
      {
        if (const auto error = std::get_if<Error>(&result))
        {
          return *error;
        }
      }
      return std::monostate{};
    }

    OperationResult check_task(const SessionControllerState& state, TaskId task)
    {
      const auto id = static_cast<std::uint16_t>(task);
      if (id == 0 || id > state._configuration._limits._tasks)
      {
        return storage_error(ErrorCode::InvalidArgument, state._directory / "claims",
                             "Task identity is outside the session's capacity");
      }
      return std::monostate{};
    }

    OperationResult check_claim_identity(SessionControllerState& state, TaskId task,
                                          const CoordinationDescriptor& descriptor)
    {
      const auto existing = state._claimIdentities.find(task);
      if (existing != state._claimIdentities.end() && existing->second != descriptor._identity)
      {
        return storage_error(ErrorCode::IoError, state._directory / "claims" / claim_name(task),
                             "Stable Task claim file was replaced");
      }
      state._claimIdentities.emplace(task, descriptor._identity);
      return std::monostate{};
    }

    OperationResult check_known_claim(const SessionControllerState& state, TaskId task)
    {
      const auto entry = state._claimIdentities.find(task);
      if (entry == state._claimIdentities.end())
      {
        return std::monostate{};
      }
      const auto name = claim_name(task);
      return check_named_identity(state._claimsFd->_fd, name.c_str(), entry->second,
                                   state._directory / "claims" / name, false);
    }

    bool same_configuration(const SuperblockConfiguration& left, const SuperblockConfiguration& right)
    {
      const auto values = [](const SuperblockConfiguration& configuration)
      {
        const auto& limits = configuration._limits;
        return std::tuple{configuration._session._high, configuration._session._low,
                          configuration._maxLogBytes, configuration._storage,
                          configuration._synchronizationVersion, configuration._liveAbiProfile,
                          configuration._requiredFeatures, configuration._optionalFeatures,
                          configuration._segmentBytes, limits._tasks, limits._queues,
                          limits._sourceSets, limits._publishingSets, limits._membershipsPerSet,
                          limits._fields, limits._messageSchemas, limits._symbols, limits._stringPoolBytes};
      };
      return values(left) == values(right);
    }
  } // namespace

  MutationGuard::MutationGuard() noexcept = default;
  MutationGuard::~MutationGuard() = default;
  MutationGuard::MutationGuard(MutationGuard&&) noexcept = default;
  MutationGuard& MutationGuard::operator=(MutationGuard&&) noexcept = default;

  MutationGuard::MutationGuard(std::unique_ptr<MutationGuardState> state) noexcept
      : _state(std::move(state))
  {
  }

  OperationResult MutationGuard::check_owner() const
  {
    if (!_state || !_state->_nativeLocked)
    {
      return storage_error(ErrorCode::InvalidState, {}, "Mutation guard is empty or moved-from");
    }
    if (auto result = _state->_controller->check_owner(); std::holds_alternative<Error>(result))
    {
      return result;
    }
    if (_state->_thread != std::this_thread::get_id())
    {
      return storage_error(ErrorCode::WrongOwner, {}, "Mutation guard belongs to another thread");
    }
    return std::monostate{};
  }

  TaskLease::TaskLease(std::unique_ptr<TaskLeaseState> state) noexcept : _state(std::move(state))
  {
  }

  TaskLease::~TaskLease() = default;

  OperationResult TaskLease::check_owner() const
  {
    if (auto result = _state->_controller->check_owner(); std::holds_alternative<Error>(result))
    {
      return result;
    }
    const auto& state = *_state->_controller->_state;
    if (auto result = check_resources(state); std::holds_alternative<Error>(result))
    {
      return result;
    }
    const auto name = claim_name(_state->_task);
    return check_named_identity(state._claimsFd->_fd, name.c_str(), _state->_descriptor->_identity,
                                 state._directory / "claims" / name, false);
  }

  TaskId TaskLease::task_id() const noexcept
  {
    return _state->_task;
  }

  SessionController::SessionController(std::unique_ptr<SessionControllerState> state) noexcept
      : _state(std::move(state))
  {
  }

  SessionController::~SessionController() = default;

  Result<std::shared_ptr<SessionController>> SessionController::open_existing(
      const std::filesystem::path& directory)
  {
    auto& registry = controllers();
    if (registry._ownerPid != static_cast<std::uint64_t>(::getpid()))
    {
      return storage_error(ErrorCode::WrongOwner, directory, "Session controller requires exec after fork");
    }
    const auto native = qualify_native_profile();
    if (const auto error = std::get_if<Error>(&native))
    {
      return *error;
    }
    if (directory.empty() || directory.native().find(std::filesystem::path::value_type{}) != directory.native().npos)
    {
      return storage_error(ErrorCode::InvalidArgument, directory, "Session directory is empty or contains a null character");
    }
    std::error_code filesystemError;
    const auto canonical = std::filesystem::canonical(directory, filesystemError);
    if (filesystemError)
    {
      return storage_error(ErrorCode::IoError, directory, "Resolve session directory: " + filesystemError.message());
    }
    auto state = std::make_unique<SessionControllerState>();
    state->_ownerPid = registry._ownerPid;
    state->_directory = canonical;
    auto directoryFd = open_coordination(AT_FDCWD, canonical, canonical, true);
    if (const auto error = std::get_if<Error>(&directoryFd))
    {
      return *error;
    }
    state->_directoryFd = std::move(std::get<std::unique_ptr<CoordinationDescriptor>>(directoryFd));
    auto file = NativeFile::open_existing(canonical / "session.ang", FileAccess::ReadWrite);
    if (const auto error = std::get_if<Error>(&file))
    {
      return *error;
    }
    state->_file = std::move(std::get<NativeFile>(file));
    state->_fileIdentity = std::get<FileIdentity>(state->_file.identity());
    SuperblockBytes bytes{};
    if (auto result = state->_file.read_exact(0, bytes); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    auto configuration = decode_superblock(bytes);
    if (const auto error = std::get_if<Error>(&configuration))
    {
      return *error;
    }
    state->_configuration = std::get<SuperblockConfiguration>(configuration);
    if (auto result = check_live_compatibility(state->_configuration, std::get<LiveCompatibility>(native));
        std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    // The provisional metadata read uses native format bounds. All retained
    // access must additionally respect this session's recorded byte ceiling.
    auto bounded = NativeFile::open_existing(canonical / "session.ang", FileAccess::ReadWrite,
                                              state->_configuration._maxLogBytes);
    if (const auto error = std::get_if<Error>(&bounded))
    {
      return *error;
    }
    auto boundedFile = std::move(std::get<NativeFile>(bounded));
    if (std::get<FileIdentity>(boundedFile.identity()) != state->_fileIdentity)
    {
      return storage_error(ErrorCode::IoError, canonical / "session.ang", "Session file changed during metadata validation");
    }
    const auto layout = make_fixed_layout(state->_configuration._limits);
    const auto physicalSize = boundedFile.size();
    if (const auto error = std::get_if<Error>(&physicalSize))
    {
      return *error;
    }
    if (!layout || std::get<std::uint64_t>(physicalSize) < layout->_arenaBegin)
    {
      return storage_error(ErrorCode::IoError, canonical / "session.ang", "Session file truncates its fixed sections");
    }
    state->_file = std::move(boundedFile);
    auto mutation = open_coordination(state->_directoryFd->_fd, "mutation.lock", canonical / "mutation.lock", false);
    if (const auto error = std::get_if<Error>(&mutation))
    {
      return *error;
    }
    state->_mutationFd = std::move(std::get<std::unique_ptr<CoordinationDescriptor>>(mutation));
    auto claims = open_coordination(state->_directoryFd->_fd, "claims", canonical / "claims", true);
    if (const auto error = std::get_if<Error>(&claims))
    {
      return *error;
    }
    state->_claimsFd = std::move(std::get<std::unique_ptr<CoordinationDescriptor>>(claims));
    if (auto result = check_resources(*state); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    std::lock_guard guard(registry._mutex);
    for (auto entry = registry._controllers.begin(); entry != registry._controllers.end();)
    {
      const auto existing = entry->second.lock();
      if (!existing)
      {
        entry = registry._controllers.erase(entry);
        continue;
      }
      if (existing->session_id() == state->_configuration._session ||
          existing->file_identity() == state->_fileIdentity)
      {
        if (auto result = check_resources(*existing->_state); std::holds_alternative<Error>(result))
        {
          return std::get<Error>(std::move(result));
        }
        if (existing->session_id() != state->_configuration._session ||
            existing->file_identity() != state->_fileIdentity ||
            existing->_state->_mutationFd->_identity != state->_mutationFd->_identity ||
            existing->_state->_claimsFd->_identity != state->_claimsFd->_identity)
        {
          return storage_error(ErrorCode::Conflict, canonical,
                               "Live session identity or coordination namespace conflicts with an existing controller");
        }
        if (!same_configuration(existing->_state->_configuration, state->_configuration))
        {
          return storage_error(ErrorCode::IoError, canonical / "session.ang", "Immutable session configuration changed");
        }
        return existing;
      }
      ++entry;
    }
    const auto key = std::pair{state->_fileIdentity._device, state->_fileIdentity._inode};
    auto controller = std::shared_ptr<SessionController>(new SessionController(std::move(state)));
    registry._controllers.emplace(key, controller);
    return controller;
  }

  OperationResult SessionController::check_owner() const
  {
    if (_state->_ownerPid != static_cast<std::uint64_t>(::getpid()))
    {
      return storage_error(ErrorCode::WrongOwner, _state->_directory, "Session controller belongs to another process");
    }
    return std::monostate{};
  }

  SessionId SessionController::session_id() const noexcept
  {
    return _state->_configuration._session;
  }

  FileIdentity SessionController::file_identity() const noexcept
  {
    return _state->_fileIdentity;
  }

  const SuperblockConfiguration& SessionController::configuration() const noexcept
  {
    return _state->_configuration;
  }

  Result<MutationGuard> SessionController::acquire_mutation()
  {
    return acquire_mutation(false);
  }

  Result<MutationGuard> SessionController::try_acquire_mutation()
  {
    return acquire_mutation(true);
  }

  Result<MutationGuard> SessionController::acquire_mutation(bool nonblocking)
  {
    if (auto result = check_owner(); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    auto guard = std::make_unique<MutationGuardState>();
    guard->_controller = shared_from_this();
    guard->_thread = std::this_thread::get_id();
    guard->_localLock = std::unique_lock<std::recursive_mutex>(_state->_gateMutex, std::defer_lock);
    if (nonblocking)
    {
      if (!guard->_localLock.try_lock())
      {
        return storage_error(ErrorCode::OwnershipBusy, _state->_directory, "Mutation gate is busy in this process");
      }
    }
    else
    {
      guard->_localLock.lock();
    }
    if (_state->_gateOwner != std::thread::id{})
    {
      // A recursive mutex makes this ownership check safe without a blocking
      // bookkeeping lock in the try path. Recursive gate entry is still denied.
      return storage_error(ErrorCode::OwnershipBusy, _state->_directory,
                           "Mutation gate is already held on this thread");
    }
    if (auto result = check_resources(*_state); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    if (lock_file(_state->_mutationFd->_fd, LOCK_EX | (nonblocking ? LOCK_NB : 0)) == -1)
    {
      if (lock_busy(errno))
      {
        return storage_error(ErrorCode::OwnershipBusy, _state->_directory / "mutation.lock", "Mutation gate is busy");
      }
      return native_io_error(_state->_directory / "mutation.lock", "lock session mutation gate", errno);
    }
    guard->_nativeLocked = true;
    _state->_gateOwner = guard->_thread;
    native_gate_fence();
    if (auto result = check_resources(*_state); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    return MutationGuard(std::move(guard));
  }

  OperationResult SessionController::create_task_claim_file(const MutationGuard& guard, TaskId task)
  {
    if (auto result = guard.check_owner(); std::holds_alternative<Error>(result))
    {
      return result;
    }
    if (guard._state->_controller.get() != this)
    {
      return storage_error(ErrorCode::WrongOwner, _state->_directory, "Mutation guard belongs to another controller");
    }
    if (auto result = check_task(*_state, task); std::holds_alternative<Error>(result))
    {
      return result;
    }
    if (auto result = check_resources(*_state); std::holds_alternative<Error>(result))
    {
      return result;
    }
    std::lock_guard claimGuard(_state->_claimMutex);
    const auto name = claim_name(task);
    const auto path = _state->_directory / "claims" / name;
    const bool known = _state->_claimIdentities.contains(task);
    auto descriptor = open_coordination(_state->_claimsFd->_fd, name, path, false, !known);
    if (const auto error = std::get_if<Error>(&descriptor); error != nullptr && error->_code == ErrorCode::Conflict)
    {
      descriptor = open_coordination(_state->_claimsFd->_fd, name, path, false);
    }
    if (const auto error = std::get_if<Error>(&descriptor))
    {
      return *error;
    }
    return check_claim_identity(*_state, task, *std::get<std::unique_ptr<CoordinationDescriptor>>(descriptor));
  }

  Result<std::shared_ptr<TaskLease>> SessionController::try_claim_task(TaskId task, std::shared_ptr<TaskLease> parent)
  {
    if (auto result = check_owner(); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    if (auto result = check_task(*_state, task); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    if (parent)
    {
      if (auto result = parent->check_owner(); std::holds_alternative<Error>(result))
      {
        return std::get<Error>(std::move(result));
      }
      if (parent->_state->_controller.get() != this || parent->task_id() == task)
      {
        return storage_error(ErrorCode::InvalidArgument, _state->_directory,
                             "Parent claim must belong to another Task in this controller");
      }
    }
    if (auto result = check_resources(*_state); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    std::unique_lock guard(_state->_claimMutex, std::try_to_lock);
    if (!guard.owns_lock())
    {
      return storage_error(ErrorCode::OwnershipBusy, _state->_directory / "claims", "Task claim bookkeeping is busy");
    }
    if (auto result = check_known_claim(*_state, task); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    if (const auto entry = _state->_claims.find(task);
        entry != _state->_claims.end() && !entry->second.expired())
    {
      return storage_error(ErrorCode::OwnershipBusy, _state->_directory / "claims" / claim_name(task),
                           "Task already has a live claim in this process");
    }
    auto state = std::make_unique<TaskLeaseState>();
    state->_controller = shared_from_this();
    state->_parent = std::move(parent);
    state->_task = task;
    const auto path = _state->_directory / "claims" / claim_name(task);
    auto descriptor = open_coordination(_state->_claimsFd->_fd, claim_name(task), path, false);
    if (const auto error = std::get_if<Error>(&descriptor))
    {
      return *error;
    }
    state->_descriptor = std::move(std::get<std::unique_ptr<CoordinationDescriptor>>(descriptor));
    if (auto result = check_claim_identity(*_state, task, *state->_descriptor); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    if (lock_file(state->_descriptor->_fd, LOCK_EX | LOCK_NB) == -1)
    {
      if (lock_busy(errno))
      {
        return storage_error(ErrorCode::OwnershipBusy, path, "Task claim is busy");
      }
      return native_io_error(path, "lock Task claim", errno);
    }
    auto lease = std::shared_ptr<TaskLease>(new TaskLease(std::move(state)));
    _state->_claims.insert_or_assign(task, lease);
    return lease;
  }

  Result<bool> SessionController::probe_task_claim(TaskId task)
  {
    if (auto result = check_owner(); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    if (auto result = check_task(*_state, task); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    if (auto result = check_resources(*_state); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    std::unique_lock guard(_state->_claimMutex, std::try_to_lock);
    if (!guard.owns_lock())
    {
      return storage_error(ErrorCode::OwnershipBusy, _state->_directory / "claims", "Task claim bookkeeping is busy");
    }
    if (auto result = check_known_claim(*_state, task); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    if (const auto entry = _state->_claims.find(task);
        entry != _state->_claims.end() && !entry->second.expired())
    {
      return true;
    }
    const auto path = _state->_directory / "claims" / claim_name(task);
    auto descriptor = open_coordination(_state->_claimsFd->_fd, claim_name(task), path, false);
    if (const auto error = std::get_if<Error>(&descriptor))
    {
      return *error;
    }
    auto owner = std::move(std::get<std::unique_ptr<CoordinationDescriptor>>(descriptor));
    if (auto result = check_claim_identity(*_state, task, *owner); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    if (lock_file(owner->_fd, LOCK_EX | LOCK_NB) == -1)
    {
      if (lock_busy(errno))
      {
        return true;
      }
      return native_io_error(path, "probe Task claim", errno);
    }
    return false;
  }

  Result<NativeFile*> SessionController::file(const MutationGuard& guard)
  {
    if (auto result = guard.check_owner(); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    if (guard._state->_controller.get() != this)
    {
      return storage_error(ErrorCode::WrongOwner, _state->_directory, "Mutation guard belongs to another controller");
    }
    if (auto result = check_resources(*_state); std::holds_alternative<Error>(result))
    {
      return std::get<Error>(std::move(result));
    }
    return &_state->_file;
  }
} // namespace angaria::detail::mapped
