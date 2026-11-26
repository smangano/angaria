#pragma once

#include "angaria/detail/mapped_format.h"

#include <cstdint>
#include <optional>

namespace angaria::detail::mapped
{
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
} // namespace angaria::detail::mapped
