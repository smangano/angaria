#pragma once

#include "angaria/detail/mapped_format.h"
#include "angaria/results.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace angaria::detail::mapped
{
  inline constexpr std::uint16_t synchronization_version = 1;
  inline constexpr std::uint64_t journal_feature = 1;

  // The superblock is the 4096-byte session control area at offset zero of
  // session.ang: metadata, section directory, mutable roots, and operation journal.
  // See docs/mapped-format.md#superblock and docs/glossary.md.
  using SuperblockBytes = std::array<std::byte, superblock_bytes>;

  // Process-local immutable configuration values only; mutable roots and journal
  // state are excluded. This type is not a native on-disk struct.
  struct SuperblockConfiguration
  {
    SessionId _session{};
    std::uint64_t _maxLogBytes{};
    StorageMapping _storage{StorageMapping::TwoPlane};
    std::uint16_t _synchronizationVersion{synchronization_version};
    std::uint32_t _liveAbiProfile{};
    std::uint64_t _requiredFeatures{journal_feature};
    std::uint64_t _optionalFeatures{};
    CapacityLimits _limits{};
    std::uint64_t _segmentBytes{};
  };

  // The native adapter supplies its checked host profile via qualify_native_profile().
  struct LiveCompatibility
  {
    std::uint32_t _liveAbiProfile{};
    std::uint16_t _synchronizationVersion{synchronization_version};
    std::uint64_t _supportedRequiredFeatures{journal_feature};
    bool _segmentedStorage{};
    std::optional<CapacityLimits> _capacityCeilings{};
  };

  // Creates a provisional prefix; it does not create or publish a usable session.
  Result<SuperblockBytes> encode_initial_superblock(const SuperblockConfiguration& configuration);

  // Validates immutable header/directory bytes only. Mutable roots and the journal
  // must be recovered and validated before a runtime can expose session handles.
  Result<SuperblockConfiguration> decode_superblock(std::span<const std::byte> bytes);

  OperationResult check_live_compatibility(const SuperblockConfiguration& configuration,
                                           const LiveCompatibility& supported);
} // namespace angaria::detail::mapped
