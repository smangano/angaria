#pragma once

#include "angaria/results.h"

#include <filesystem>
#include <memory>

namespace angaria::detail::mapped
{
  struct NativeClaimState;

  // Process-owned, crash-released exclusive lifetime claim on a stable inode.
  // A producer locks its actual data inode through a separate registered file
  // description, so inherited data mappings do not retain the lifetime lock.
  // No raw descriptor escapes this owner.
  class NativeClaim
  {
  public:
    NativeClaim() noexcept;
    ~NativeClaim();
    NativeClaim(NativeClaim&&) noexcept;
    NativeClaim& operator=(NativeClaim&&) noexcept;
    NativeClaim(const NativeClaim&) = delete;
    NativeClaim& operator=(const NativeClaim&) = delete;

    static Result<NativeClaim> create_exclusive(const std::filesystem::path& path);
    static Result<NativeClaim> acquire_existing(const std::filesystem::path& path);
    OperationResult check_owner() const;

  private:
    explicit NativeClaim(std::unique_ptr<NativeClaimState> state) noexcept;
    static Result<NativeClaim> acquire(const std::filesystem::path& path, bool create);
    std::unique_ptr<NativeClaimState> _state;
  };
} // namespace angaria::detail::mapped
