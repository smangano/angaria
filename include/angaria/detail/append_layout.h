#pragma once

#include "angaria/detail/record_layout.h"

#include <cstdint>
#include <limits>
#include <optional>

namespace angaria::detail::mapped
{
  struct AppendStorageLayout
  {
    std::uint64_t _manifestOffset{};
    std::uint64_t _manifestBodyBytes{};
    std::uint64_t _manifestBytes{};
    std::uint64_t _tail{};
  };

  // Size-only planning: no payload, receipt, reference, or redo buffer is
  // constructed. Call finish only after adding every record. Callers retain
  // their own error policy and must separately validate record/command content.
  class AppendLayoutPlanner
  {
  public:
    constexpr explicit AppendLayoutPlanner(std::uint64_t tail)
        : _tail(tail), _valid(tail % section_alignment == 0 && tail <= max_log_bytes)
    {
    }

    constexpr bool add_record(std::uint64_t immutableBytes, std::uint64_t mutableBytes = 0)
    {
      const auto record = record_storage_layout(immutableBytes, mutableBytes);
      const auto tail = record ? checked_add(_tail, record->_totalBytes) : std::nullopt;
      // The journal's uint32 record count also includes the final manifest.
      if (!_valid || !tail || *tail > max_log_bytes ||
          _records >= std::numeric_limits<std::uint32_t>::max() - std::uint64_t{1})
      {
        _valid = false;
        return false;
      }
      _tail = *tail;
      ++_records;
      return true;
    }

    constexpr std::optional<AppendStorageLayout> finish(std::uint64_t commandBytes,
                                                         std::uint64_t maxLogBytes = max_log_bytes) const
    {
      const auto references = checked_multiply(_records, 8);
      const auto fixedAndReferences = references ? checked_add(64, *references) : std::nullopt;
      const auto body = fixedAndReferences ? checked_add(*fixedAndReferences, commandBytes) : std::nullopt;
      const auto manifest = body ? record_storage_bytes(*body) : std::nullopt;
      const auto tail = manifest ? checked_add(_tail, *manifest) : std::nullopt;
      if (!_valid || !tail || !fits_host_size(*manifest) || *tail > max_log_bytes || *tail > maxLogBytes)
      {
        return std::nullopt;
      }
      return AppendStorageLayout{_tail, *body, *manifest, *tail};
    }

  private:
    std::uint64_t _tail{};
    std::uint64_t _records{};
    bool _valid{};
  };
} // namespace angaria::detail::mapped
