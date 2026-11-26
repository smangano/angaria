#pragma once

#include "native_file.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <sys/stat.h>

namespace angaria::detail::mapped
{
  inline FileIdentity stat_identity(const struct stat& info)
  {
    static_assert(sizeof(info.st_dev) <= sizeof(std::uint64_t));
    static_assert(sizeof(info.st_ino) <= sizeof(std::uint64_t));
    return FileIdentity{static_cast<std::uint64_t>(info.st_dev),
                        static_cast<std::uint64_t>(info.st_ino)};
  }

  // Shared by all open handles to the same device/inode in this process.
  struct InodeState
  {
    std::mutex _mutex;
    std::multiset<std::uint64_t> _mappingEnds;
  };

  struct NativeFileState
  {
    ~NativeFileState();

    int _fd{-1};
    std::uint64_t _ownerPid{};
    std::filesystem::path _path;
    FileAccess _access{FileAccess::ReadOnly};
    std::uint64_t _maxBytes{};
    FileIdentity _identity;
    std::shared_ptr<InodeState> _inode;
  };

  Error storage_error(ErrorCode code, const std::filesystem::path& path, std::string message);
  Error native_io_error(const std::filesystem::path& path, std::string_view operation, int error);
  std::uint64_t native_max_bytes();
  // Must precede any acquisition of an inherited process-local mutex.
  std::optional<Error> check_file_owner(const std::shared_ptr<NativeFileState>& state);
  // Caller holds state._inode->_mutex and has validated process ownership.
  Result<std::uint64_t> file_size_locked(const NativeFileState& state);
} // namespace angaria::detail::mapped
