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
  inline constexpr std::uint64_t basic_fields_feature = 2;
  inline constexpr std::uint64_t native_delivery_feature = 4;
  inline constexpr std::uint64_t allow_loss_feature = 8;
  // Public Session factories select this core delivery profile explicitly.
  // The framing configuration's older default remains useful for legacy readers.
  inline constexpr std::uint16_t delivery_format_minor = 2;

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
    std::uint64_t _requiredFeatures{journal_feature | basic_fields_feature};
    std::uint64_t _optionalFeatures{};
    CapacityLimits _limits{};
    std::uint64_t _segmentBytes{};
    std::uint16_t _formatMinor{format_minor};
  };

  // The native adapter supplies its checked host profile via qualify_native_profile().
  struct LiveCompatibility
  {
    std::uint32_t _liveAbiProfile{};
    std::uint16_t _synchronizationVersion{synchronization_version};
    std::uint64_t _supportedRequiredFeatures{journal_feature | basic_fields_feature | native_delivery_feature | allow_loss_feature};
    bool _segmentedStorage{};
    std::optional<CapacityLimits> _capacityCeilings{};
    std::uint16_t _maxFormatMinor{delivery_format_minor};
  };

  // Creates a provisional prefix; it does not create or publish a usable session.
  Result<SuperblockBytes> encode_initial_superblock(const SuperblockConfiguration& configuration);

  // Validates immutable header/directory bytes only. Mutable roots and the journal
  // must be recovered and validated before a runtime can expose session handles.
  Result<SuperblockConfiguration> decode_superblock(std::span<const std::byte> bytes);

  OperationResult check_live_compatibility(const SuperblockConfiguration& configuration,
                                           const LiveCompatibility& supported);
} // namespace angaria::detail::mapped
