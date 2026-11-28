#include "native_file_state.h"

#include "performance_diagnostics.h"

#include <cerrno>
#include <cstdint>
#include <mutex>
#include <variant>

#include <fcntl.h>
#include <unistd.h>

namespace angaria::detail::mapped
{
  OperationResult NativeFile::reserve(std::uint64_t bytes)
  {
    ANGARIA_DIAGNOSTIC_SCOPE(FileGrowth);
    if (const auto error = check_file_owner(_state))
    {
      return *error;
    }
    if (_state->_access != FileAccess::ReadWrite)
    {
      return storage_error(ErrorCode::InvalidState, _state->_path, "Reservation requires a read-write file");
    }
    if (bytes == 0)
    {
      return storage_error(ErrorCode::InvalidArgument, _state->_path, "Reservation must have a positive byte size");
    }
    if (bytes > _state->_maxBytes || bytes > native_max_bytes())
    {
      return storage_error(ErrorCode::CapacityExceeded, _state->_path, "Reservation exceeds the native file byte limit");
    }
    std::lock_guard guard(_state->_inode->_mutex);
    const auto actual = file_size_locked(*_state);
    if (const auto error = std::get_if<Error>(&actual))
    {
      return *error;
    }
    if (!_state->_inode->_mappingEnds.empty())
    {
      return storage_error(ErrorCode::OwnershipBusy, _state->_path, "Reservation requires an unmapped file");
    }
    if (std::get<std::uint64_t>(actual) != 0)
    {
      return storage_error(ErrorCode::InvalidState, _state->_path, "Reservation requires a fresh empty file");
    }
#if defined(__linux__)
    int error;
    do
    {
      ANGARIA_DIAGNOSTIC_COUNT(FileGrowCalls, 1);
      // POSIX returns the error number directly and does not set errno.
      error = ::posix_fallocate(_state->_fd, 0, static_cast<off_t>(bytes));
    } while (error == EINTR);
    if (error != 0)
    {
      return native_io_error(_state->_path, "reserve native file storage", error);
    }
#elif defined(__APPLE__)
    fstore_t storage{};
    storage.fst_flags = F_ALLOCATEALL;
    storage.fst_posmode = F_PEOFPOSMODE;
    storage.fst_offset = 0;
    storage.fst_length = static_cast<off_t>(bytes);
    int result;
    do
    {
      ANGARIA_DIAGNOSTIC_COUNT(FileGrowCalls, 1);
      result = ::fcntl(_state->_fd, F_PREALLOCATE, &storage);
    } while (result == -1 && errno == EINTR);
    if (result == -1)
    {
      return native_io_error(_state->_path, "reserve native file storage", errno);
    }
    if (storage.fst_bytesalloc < static_cast<off_t>(bytes))
    {
      return storage_error(ErrorCode::IoError, _state->_path, "Native reservation did not allocate its complete range");
    }
    do
    {
      ANGARIA_DIAGNOSTIC_COUNT(FileGrowCalls, 1);
      result = ::ftruncate(_state->_fd, static_cast<off_t>(bytes));
    } while (result == -1 && errno == EINTR);
    if (result == -1)
    {
      return native_io_error(_state->_path, "size reserved native file", errno);
    }
#else
    return storage_error(ErrorCode::UnsupportedPolicy, _state->_path,
                         "Native physical reservation is unsupported on this platform");
#endif
    return std::monostate{};
  }
} // namespace angaria::detail::mapped
