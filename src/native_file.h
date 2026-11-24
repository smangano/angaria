#pragma once

#include "angaria/detail/mapped_format.h"
#include "angaria/results.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace angaria::detail::mapped
{
  enum class FileAccess
  {
    ReadOnly,
    ReadWrite
  };

  struct FileIdentity
  {
    std::uint64_t _device{};
    std::uint64_t _inode{};

    bool operator==(const FileIdentity&) const = default;
  };

  struct NativeFileState;
  struct MappedRegionState;

  // A move-only owner of a shared file mapping. Returned spans borrow its
  // lifetime; the mapping retains the file and protects its exposed range.
  class MappedRegion
  {
  public:
    MappedRegion() noexcept;
    ~MappedRegion();
    MappedRegion(MappedRegion&&) noexcept;
    MappedRegion& operator=(MappedRegion&&) noexcept;
    MappedRegion(const MappedRegion&) = delete;
    MappedRegion& operator=(const MappedRegion&) = delete;

    Result<std::span<const std::byte>> bytes() const;
    Result<std::span<std::byte>> writable_bytes();

  private:
    friend class NativeFile;
    explicit MappedRegion(std::unique_ptr<MappedRegionState> state) noexcept;
    std::unique_ptr<MappedRegionState> _state;
  };

  // Native storage only: opening a file does not establish a valid Session,
  // acquire its mutation gate, or validate/recover its superblock and journal.
  class NativeFile
  {
  public:
    NativeFile() noexcept = default;
    ~NativeFile() = default;
    NativeFile(NativeFile&&) noexcept = default;
    NativeFile& operator=(NativeFile&&) noexcept = default;
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;

    // Creates an empty regular file exclusively. Growth is a separate resize.
    static Result<NativeFile> create_exclusive(const std::filesystem::path& path,
                                               std::uint64_t maxBytes = max_log_bytes);
    static Result<NativeFile> open_existing(const std::filesystem::path& path, FileAccess access,
                                            std::uint64_t maxBytes = max_log_bytes);

    Result<std::uint64_t> size() const;
    Result<FileIdentity> identity() const;
    // Borrowed descriptor: callers must not close, replace, or resize it directly.
    Result<int> native_handle() const;
    OperationResult resize(std::uint64_t bytes);
    OperationResult read_exact(std::uint64_t offset, std::span<std::byte> output) const;
    OperationResult write_exact(std::uint64_t offset, std::span<const std::byte> input);
    Result<MappedRegion> map_read_only(std::uint64_t offset, std::uint64_t length) const;
    Result<MappedRegion> map_read_write(std::uint64_t offset, std::uint64_t length);

  private:
    explicit NativeFile(std::shared_ptr<NativeFileState> state) noexcept;
    Result<MappedRegion> map_region(std::uint64_t offset, std::uint64_t length, bool writable) const;
    std::shared_ptr<NativeFileState> _state;
  };
} // namespace angaria::detail::mapped
