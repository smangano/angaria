#pragma once

#include "angaria/detail/mapped_format.h"

#include <cstdint>
#include <optional>

namespace angaria::detail::mapped
{
  struct RecordStorageLayout
  {
    std::uint64_t _immutableBytes{};
    std::uint64_t _mutableOffset{};
    std::uint64_t _totalBytes{};
  };

  // Pure native-record geometry. Callers keep their own allocation policy and
  // error paths; this does not reserve storage or establish record acceptance.
  constexpr std::optional<std::uint64_t> record_storage_bytes(std::uint64_t bodyBytes)
  {
    const auto body = checked_align(bodyBytes, record_alignment);
    const auto required = body ? checked_add(record_envelope_bytes, *body) : std::nullopt;
    const auto total = required ? checked_align(*required, section_alignment) : std::nullopt;
    if (!total || *total > max_log_bytes)
    {
      return std::nullopt;
    }
    return total;
  }

  // Immutable bytes are word-padded before a mutable suffix. A record without
  // a mutable suffix includes its final allocation padding in the immutable
  // range, matching the envelope written by the journal. The resulting record
  // must also fit one host-addressable image; format-only callers may instead
  // use record_storage_bytes above.
  constexpr std::optional<RecordStorageLayout> record_storage_layout(std::uint64_t immutableBytes,
                                                                      std::uint64_t mutableBytes = 0)
  {
    const auto immutable = checked_align(immutableBytes, record_alignment);
    const auto body = immutable ? checked_add(*immutable, mutableBytes) : std::nullopt;
    const auto total = body ? record_storage_bytes(*body) : std::nullopt;
    if (!total || !fits_host_size(*total))
    {
      return std::nullopt;
    }
    return RecordStorageLayout{mutableBytes == 0 ? *total - record_envelope_bytes : *immutable,
                               record_envelope_bytes + *immutable, *total};
  }
} // namespace angaria::detail::mapped
