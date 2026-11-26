#include "native_file_state.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace angaria::detail::mapped
{
  namespace
  {
    struct InodeRegistry
    {
      const std::uint64_t _ownerPid{static_cast<std::uint64_t>(::getpid())};
      std::mutex _mutex;
      std::map<std::pair<std::uint64_t, std::uint64_t>, std::weak_ptr<InodeState>> _inodes;
    };

    InodeRegistry& inode_registry()
    {
      // First use may precede main, including from another translation unit.
      static InodeRegistry registry;
      return registry;
    }

    std::shared_ptr<InodeState> inode_state(InodeRegistry& registry, FileIdentity identity)
    {
      std::lock_guard guard(registry._mutex);
      for (auto entry = registry._inodes.begin(); entry != registry._inodes.end();)
      {
        if (entry->second.expired())
        {
          entry = registry._inodes.erase(entry);
        }
        else
        {
          ++entry;
        }
      }
      const auto key = std::pair{identity._device, identity._inode};
      if (const auto entry = registry._inodes.find(key); entry != registry._inodes.end())
      {
        if (auto shared = entry->second.lock())
        {
          return shared;
        }
      }
      auto shared = std::make_shared<InodeState>();
      registry._inodes.insert_or_assign(key, shared);
      return shared;
    }

    Result<std::shared_ptr<NativeFileState>> open_file(const std::filesystem::path& path,
                                                      FileAccess access, std::uint64_t maxBytes,
                                                      bool create)
    {
      // A fork child must reject the inherited registry before touching its mutex.
      auto& registry = inode_registry();
      if (static_cast<std::uint64_t>(::getpid()) != registry._ownerPid)
      {
        return storage_error(ErrorCode::WrongOwner, path, "Native storage requires exec after fork");
      }
      if (path.empty() || path.native().find(std::filesystem::path::value_type{}) != path.native().npos)
      {
        return storage_error(ErrorCode::InvalidArgument, path, "File path is empty or contains a null character");
      }
      if (access != FileAccess::ReadOnly && access != FileAccess::ReadWrite)
      {
        return storage_error(ErrorCode::InvalidArgument, path, "Unknown file access mode");
      }
      if (maxBytes == 0 || maxBytes > native_max_bytes())
      {
        return storage_error(ErrorCode::InvalidArgument, path, "File byte limit exceeds native format bounds");
      }

      // Allocate the owner before opening, so all later failures close the FD.
      auto state = std::make_shared<NativeFileState>();
      state->_ownerPid = registry._ownerPid;
      state->_path = path;
      state->_access = access;
      state->_maxBytes = maxBytes;
      int flags = access == FileAccess::ReadOnly ? O_RDONLY : O_RDWR;
      flags |= O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
      if (create)
      {
        flags |= O_CREAT | O_EXCL;
      }
      do
      {
        state->_fd = ::open(path.c_str(), flags, 0600);
      } while (state->_fd == -1 && errno == EINTR);
      if (state->_fd == -1)
      {
        const int error = errno;
        if (create && error == EEXIST)
        {
          return storage_error(ErrorCode::Conflict, path, "Exclusive file creation found an existing path");
        }
        return native_io_error(path, create ? "create native file" : "open native file", error);
      }

      struct stat info{};
      if (::fstat(state->_fd, &info) == -1)
      {
        return native_io_error(path, "stat native file", errno);
      }
      if (!S_ISREG(info.st_mode))
      {
        return storage_error(ErrorCode::IoError, path, "Native storage requires a regular file");
      }
      state->_identity = stat_identity(info);
      state->_inode = inode_state(registry, state->_identity);
      {
        std::lock_guard guard(state->_inode->_mutex);
        const auto size = file_size_locked(*state);
        if (const auto error = std::get_if<Error>(&size))
        {
          return *error;
        }
      }
      return state;
    }

    std::optional<Error> validate_range(const NativeFileState& state, std::uint64_t offset,
                                       std::size_t length, std::uint64_t fileSize)
    {
      static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
      const auto bytes = static_cast<std::uint64_t>(length);
      if (!contains_range(native_max_bytes(), offset, bytes))
      {
        return storage_error(ErrorCode::InvalidArgument, state._path,
                             "Byte range exceeds native file-offset bounds");
      }
      if (!contains_range(fileSize, offset, bytes))
      {
        return storage_error(ErrorCode::IoError, state._path, "Byte range extends beyond the current file");
      }
      return std::nullopt;
    }
  } // namespace

  NativeFileState::~NativeFileState()
  {
    if (_fd != -1)
    {
      // Never retry close: the descriptor may already have been released.
      ::close(_fd);
    }
  }

  Error storage_error(ErrorCode code, const std::filesystem::path& path, std::string message)
  {
    return Error{code, std::move(message), path.empty() ? "native_file" : path.string()};
  }

  Error native_io_error(const std::filesystem::path& path, std::string_view operation, int error)
  {
    return storage_error(ErrorCode::IoError, path,
                         std::string{operation} + ": " + std::error_code(error, std::generic_category()).message());
  }

  std::uint64_t native_max_bytes()
  {
    static_assert(std::is_integral_v<off_t> && sizeof(off_t) <= sizeof(std::uint64_t));
    return std::min(max_log_bytes, static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()));
  }

  std::optional<Error> check_file_owner(const std::shared_ptr<NativeFileState>& state)
  {
    if (!state || state->_fd == -1 || !state->_inode)
    {
      return storage_error(ErrorCode::InvalidState, {}, "Native file is empty or moved-from");
    }
    if (state->_ownerPid != static_cast<std::uint64_t>(::getpid()))
    {
      return storage_error(ErrorCode::WrongOwner, state->_path, "Native file belongs to another process");
    }
    return std::nullopt;
  }

  Result<std::uint64_t> file_size_locked(const NativeFileState& state)
  {
    struct stat info{};
    if (::fstat(state._fd, &info) == -1)
    {
      return native_io_error(state._path, "stat native file", errno);
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0 || stat_identity(info) != state._identity)
    {
      return storage_error(ErrorCode::IoError, state._path, "Native file type, identity, or size changed unexpectedly");
    }
    const auto size = static_cast<std::uint64_t>(info.st_size);
    if (size > native_max_bytes() || size > state._maxBytes)
    {
      return storage_error(ErrorCode::IoError, state._path, "Native file exceeds its supported byte limit");
    }
    return size;
  }

  NativeFile::NativeFile(std::shared_ptr<NativeFileState> state) noexcept : _state(std::move(state))
  {
  }

  Result<NativeFile> NativeFile::create_exclusive(const std::filesystem::path& path,
                                                 std::uint64_t maxBytes)
  {
    auto opened = open_file(path, FileAccess::ReadWrite, maxBytes, true);
    if (const auto error = std::get_if<Error>(&opened))
    {
      return *error;
    }
    return NativeFile(std::move(std::get<std::shared_ptr<NativeFileState>>(opened)));
  }

  Result<NativeFile> NativeFile::open_existing(const std::filesystem::path& path, FileAccess access,
                                              std::uint64_t maxBytes)
  {
    auto opened = open_file(path, access, maxBytes, false);
    if (const auto error = std::get_if<Error>(&opened))
    {
      return *error;
    }
    return NativeFile(std::move(std::get<std::shared_ptr<NativeFileState>>(opened)));
  }

  Result<std::uint64_t> NativeFile::size() const
  {
    if (const auto error = check_file_owner(_state))
    {
      return *error;
    }
    std::lock_guard guard(_state->_inode->_mutex);
    return file_size_locked(*_state);
  }

  Result<FileIdentity> NativeFile::identity() const
  {
    if (const auto error = check_file_owner(_state))
    {
      return *error;
    }
    return _state->_identity;
  }

  Result<int> NativeFile::native_handle() const
  {
    if (const auto error = check_file_owner(_state))
    {
      return *error;
    }
    return _state->_fd;
  }

  OperationResult NativeFile::resize(std::uint64_t bytes)
  {
    if (const auto error = check_file_owner(_state))
    {
      return *error;
    }
    if (_state->_access != FileAccess::ReadWrite)
    {
      return storage_error(ErrorCode::InvalidState, _state->_path, "Native file was opened read-only");
    }
    if (bytes > _state->_maxBytes || bytes > native_max_bytes())
    {
      return storage_error(ErrorCode::CapacityExceeded, _state->_path, "Resize exceeds the file byte limit");
    }
    std::lock_guard guard(_state->_inode->_mutex);
    const auto actual = file_size_locked(*_state);
    if (const auto error = std::get_if<Error>(&actual))
    {
      return *error;
    }
    if (!_state->_inode->_mappingEnds.empty() && *_state->_inode->_mappingEnds.rbegin() > bytes)
    {
      return storage_error(ErrorCode::OwnershipBusy, _state->_path, "Resize would invalidate a live mapped range");
    }
    if (bytes == std::get<std::uint64_t>(actual))
    {
      return std::monostate{};
    }
    int result;
    do
    {
      result = ::ftruncate(_state->_fd, static_cast<off_t>(bytes));
    } while (result == -1 && errno == EINTR);
    if (result == -1)
    {
      return native_io_error(_state->_path, "resize native file", errno);
    }
    return std::monostate{};
  }

  OperationResult NativeFile::read_exact(std::uint64_t offset, std::span<std::byte> output) const
  {
    if (const auto error = check_file_owner(_state))
    {
      return *error;
    }
    std::lock_guard guard(_state->_inode->_mutex);
    const auto actual = file_size_locked(*_state);
    if (const auto error = std::get_if<Error>(&actual))
    {
      return *error;
    }
    if (const auto error = validate_range(*_state, offset, output.size(), std::get<std::uint64_t>(actual)))
    {
      return *error;
    }
    while (!output.empty())
    {
      const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(
          output.size(), static_cast<std::uint64_t>(std::numeric_limits<ssize_t>::max())));
      const auto count = ::pread(_state->_fd, output.data(), chunk, static_cast<off_t>(offset));
      if (count > 0)
      {
        offset += static_cast<std::uint64_t>(count);
        output = output.subspan(static_cast<std::size_t>(count));
      }
      else if (count == 0)
      {
        return storage_error(ErrorCode::IoError, _state->_path, "Unexpected end of native file during read");
      }
      else if (errno != EINTR)
      {
        return native_io_error(_state->_path, "read native file", errno);
      }
    }
    return std::monostate{};
  }

  OperationResult NativeFile::write_exact(std::uint64_t offset, std::span<const std::byte> input)
  {
    if (const auto error = check_file_owner(_state))
    {
      return *error;
    }
    if (_state->_access != FileAccess::ReadWrite)
    {
      return storage_error(ErrorCode::InvalidState, _state->_path, "Native file was opened read-only");
    }
    std::lock_guard guard(_state->_inode->_mutex);
    const auto actual = file_size_locked(*_state);
    if (const auto error = std::get_if<Error>(&actual))
    {
      return *error;
    }
    if (const auto error = validate_range(*_state, offset, input.size(), std::get<std::uint64_t>(actual)))
    {
      return *error;
    }
    while (!input.empty())
    {
      const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(
          input.size(), static_cast<std::uint64_t>(std::numeric_limits<ssize_t>::max())));
      const auto count = ::pwrite(_state->_fd, input.data(), chunk, static_cast<off_t>(offset));
      if (count > 0)
      {
        offset += static_cast<std::uint64_t>(count);
        input = input.subspan(static_cast<std::size_t>(count));
      }
      else if (count == 0)
      {
        return storage_error(ErrorCode::IoError, _state->_path, "Native file write made no progress");
      }
      else if (errno != EINTR)
      {
        return native_io_error(_state->_path, "write native file", errno);
      }
    }
    return std::monostate{};
  }
} // namespace angaria::detail::mapped
