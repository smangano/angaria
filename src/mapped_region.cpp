#include "native_file_state.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
#include <variant>

#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

namespace angaria::detail::mapped
{
  struct MappedRegionState
  {
    ~MappedRegionState()
    {
      if (_mappedLength == 0)
      {
        return;
      }
      // A child has its own virtual mapping but must not lock inherited mutexes.
      // Multithreaded fork requires exec/_exit before library use or cleanup.
      if (_file->_ownerPid != static_cast<std::uint64_t>(::getpid()))
      {
        ::munmap(_address, _mappedLength);
        return;
      }
      std::lock_guard guard(_file->_inode->_mutex);
      ::munmap(_address, _mappedLength);
      _file->_inode->_mappingEnds.erase(_lease);
    }

    std::shared_ptr<NativeFileState> _file;
    std::byte* _address{};
    std::size_t _mappedLength{};
    std::size_t _delta{};
    std::size_t _length{};
    bool _writable{};
    std::multiset<std::uint64_t>::iterator _lease;
  };

  MappedRegion::MappedRegion() noexcept = default;
  MappedRegion::~MappedRegion() = default;
  MappedRegion::MappedRegion(MappedRegion&&) noexcept = default;
  MappedRegion& MappedRegion::operator=(MappedRegion&&) noexcept = default;

  MappedRegion::MappedRegion(std::unique_ptr<MappedRegionState> state) noexcept
      : _state(std::move(state))
  {
  }

  Result<std::span<const std::byte>> MappedRegion::bytes() const
  {
    if (!_state)
    {
      return storage_error(ErrorCode::InvalidState, "mapped_region", "Mapping has no owned region");
    }
    if (const auto error = check_file_owner(_state->_file))
    {
      return *error;
    }
    return std::span<const std::byte>{_state->_address + _state->_delta, _state->_length};
  }

  Result<std::span<std::byte>> MappedRegion::writable_bytes()
  {
    if (!_state)
    {
      return storage_error(ErrorCode::InvalidState, "mapped_region", "Mapping has no owned region");
    }
    if (const auto error = check_file_owner(_state->_file))
    {
      return *error;
    }
    if (!_state->_writable)
    {
      return storage_error(ErrorCode::InvalidState, _state->_file->_path, "Mapping is read-only");
    }
    return std::span<std::byte>{_state->_address + _state->_delta, _state->_length};
  }

  Result<MappedRegion> NativeFile::map_read_only(std::uint64_t offset, std::uint64_t length) const
  {
    return map_region(offset, length, false);
  }

  Result<MappedRegion> NativeFile::map_read_write(std::uint64_t offset, std::uint64_t length)
  {
    return map_region(offset, length, true);
  }

  Result<MappedRegion> NativeFile::map_region(std::uint64_t offset, std::uint64_t length,
                                             bool writable) const
  {
    if (const auto error = check_file_owner(_state))
    {
      return *error;
    }
    if (writable && _state->_access != FileAccess::ReadWrite)
    {
      return storage_error(ErrorCode::InvalidState, _state->_path, "File is read-only");
    }
    if (length == 0 || !contains_range(native_max_bytes(), offset, length))
    {
      return storage_error(ErrorCode::InvalidArgument, _state->_path, "Invalid mapping offset or length");
    }
    const auto page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0)
    {
      return storage_error(ErrorCode::IoError, _state->_path, "Unable to determine the host page size");
    }
    const auto pageBytes = static_cast<std::uint64_t>(page);
    const auto delta = offset % pageBytes;
    const auto mappedLength = checked_add(delta, length);
    if (!mappedLength || *mappedLength > std::numeric_limits<std::size_t>::max() ||
        *mappedLength > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()))
    {
      return storage_error(ErrorCode::InvalidArgument, _state->_path, "Mapping exceeds host addressable range");
    }

    // Allocate the owner before acquiring a lease or mapping native resources.
    auto region = std::make_unique<MappedRegionState>();
    region->_file = _state;
    region->_delta = static_cast<std::size_t>(delta);
    region->_length = static_cast<std::size_t>(length);
    region->_writable = writable;
    std::lock_guard guard(_state->_inode->_mutex);
    const auto size = file_size_locked(*_state);
    if (const auto error = std::get_if<Error>(&size))
    {
      return *error;
    }
    if (!contains_range(std::get<std::uint64_t>(size), offset, length))
    {
      return storage_error(ErrorCode::IoError, _state->_path, "Mapping range exceeds current file size");
    }
    const auto lease = _state->_inode->_mappingEnds.insert(offset + length);
    const auto nativeLength = static_cast<std::size_t>(*mappedLength);
    const auto protection = PROT_READ | (writable ? PROT_WRITE : 0);
    void* const address = ::mmap(nullptr, nativeLength, protection, MAP_SHARED,
                                 _state->_fd, static_cast<off_t>(offset - delta));
    if (address == MAP_FAILED)
    {
      const int error = errno;
      _state->_inode->_mappingEnds.erase(lease);
      return native_io_error(_state->_path, "map file", error);
    }
    if (address == nullptr)
    {
      // A null-address mapping cannot be represented as a C++ byte span.
      ::munmap(address, nativeLength);
      _state->_inode->_mappingEnds.erase(lease);
      return storage_error(ErrorCode::IoError, _state->_path, "Native mapping returned a null address");
    }
    region->_address = static_cast<std::byte*>(address);
    region->_lease = lease;
    region->_mappedLength = nativeLength;
    return MappedRegion(std::move(region));
  }
} // namespace angaria::detail::mapped
