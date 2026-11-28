#include "native_claim.h"

#include "coordination_descriptor.h"
#include "native_atomic.h"
#include "native_file_state.h"

#include <utility>
#include <variant>

#include <fcntl.h>

namespace angaria::detail::mapped
{
  struct NativeClaimState
  {
    std::filesystem::path _path;
    std::unique_ptr<CoordinationDescriptor> _descriptor;
  };

  NativeClaim::NativeClaim() noexcept = default;
  NativeClaim::~NativeClaim() = default;
  NativeClaim::NativeClaim(NativeClaim&&) noexcept = default;
  NativeClaim& NativeClaim::operator=(NativeClaim&&) noexcept = default;

  NativeClaim::NativeClaim(std::unique_ptr<NativeClaimState> state) noexcept : _state(std::move(state))
  {
  }

  Result<NativeClaim> NativeClaim::create_exclusive(const std::filesystem::path& path)
  {
    return acquire(path, true);
  }

  Result<NativeClaim> NativeClaim::acquire_existing(const std::filesystem::path& path)
  {
    return acquire(path, false);
  }

  Result<NativeClaim> NativeClaim::acquire(const std::filesystem::path& path, bool create)
  {
    auto descriptor = open_coordination(AT_FDCWD, path, path, false, create);
    if (const auto error = std::get_if<Error>(&descriptor))
    {
      return *error;
    }
    auto owner = std::move(std::get<std::unique_ptr<CoordinationDescriptor>>(descriptor));
    auto claimed = claim_coordination(*owner, path);
    if (const auto error = std::get_if<Error>(&claimed))
    {
      return *error;
    }
    auto state = std::make_unique<NativeClaimState>();
    state->_path = path;
    state->_descriptor = std::move(owner);
    native_gate_fence();
    return NativeClaim(std::move(state));
  }

  OperationResult NativeClaim::check_owner() const
  {
    if (!_state || !_state->_descriptor)
    {
      return storage_error(ErrorCode::InvalidState, {}, "Native claim is empty or moved-from");
    }
    if (!owns_coordination(*_state->_descriptor))
    {
      return storage_error(ErrorCode::WrongOwner, _state->_path, "Native claim belongs to another process");
    }
    return std::monostate{};
  }
} // namespace angaria::detail::mapped
