#pragma once

#include "angaria/descriptors.h"

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace angaria::detail::mapped
{
  // Byte counts describe encoded sections, never sizeof a native C++ structure.
  static_assert(CHAR_BIT == 8);
  static_assert(sizeof(std::uint16_t) == 2);
  static_assert(sizeof(std::uint32_t) == 4);
  static_assert(sizeof(std::uint64_t) == 8);

  inline constexpr std::uint16_t format_major = 2;
  inline constexpr std::uint16_t format_minor = 1;
  inline constexpr std::uint64_t superblock_bytes = 4096;
  inline constexpr std::uint64_t extent_header_bytes = 4096;
  inline constexpr std::uint64_t section_alignment = 64;
  inline constexpr std::uint64_t record_alignment = 8;
  inline constexpr std::uint64_t index_entry_bytes = 64;
  inline constexpr std::uint64_t task_state_bytes = 256;
  inline constexpr std::uint64_t queue_state_bytes = 256;
  inline constexpr std::uint64_t source_state_bytes = 256;
  inline constexpr std::uint64_t publishing_state_bytes = 128;
  inline constexpr std::uint64_t membership_bytes = 32;
  inline constexpr std::uint64_t symbol_entry_bytes = 32;
  inline constexpr std::uint64_t ring_header_bytes = 64;
  inline constexpr std::uint64_t ring_slot_bytes = 32;
  inline constexpr std::uint32_t max_entity_id = 65535;
  inline constexpr std::uint32_t max_fields = 0x01000000;
  inline constexpr std::uint64_t max_log_bytes = std::numeric_limits<std::int64_t>::max();
  inline constexpr std::size_t section_count = 13;

  enum class SectionKind : std::uint16_t
  {
    TaskIndex = 1,
    QueueIndex = 2,
    SourceIndex = 3,
    PublishingIndex = 4,
    FieldIndex = 5,
    SchemaIndex = 6,
    TaskState = 7,
    QueueState = 8,
    SourceState = 9,
    PublishingState = 10,
    MembershipState = 11,
    SymbolIndex = 12,
    StringPool = 13
  };

  // Process-local layout values. Encoders write fields individually, never these objects.
  struct SectionLayout
  {
    SectionKind _kind{};
    std::uint16_t _flags{};
    std::uint32_t _stride{};
    std::uint64_t _offset{};
    std::uint64_t _count{};
    std::uint64_t _byteLength{};
  };

  struct FixedLayout
  {
    std::array<SectionLayout, section_count> _sections{};
    std::uint64_t _arenaBegin{};
  };

  constexpr std::optional<std::uint64_t> checked_add(std::uint64_t left, std::uint64_t right)
  {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
    {
      return std::nullopt;
    }
    return left + right;
  }

  constexpr std::optional<std::uint64_t> checked_multiply(std::uint64_t left, std::uint64_t right)
  {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
    {
      return std::nullopt;
    }
    return left * right;
  }

  constexpr std::optional<std::uint64_t> checked_align(std::uint64_t value, std::uint64_t alignment)
  {
    if (alignment == 0)
    {
      return std::nullopt;
    }
    const auto remainder = value % alignment;
    return checked_add(value, remainder == 0 ? 0 : alignment - remainder);
  }

  // Empty ranges may end at the extent boundary; required references reject emptiness separately.
  constexpr bool contains_range(std::uint64_t extent_bytes, std::uint64_t offset, std::uint64_t length)
  {
    return offset <= extent_bytes && length <= extent_bytes - offset;
  }

  constexpr bool valid_log_limit(std::uint64_t bytes)
  {
    return bytes != 0 && bytes <= max_log_bytes;
  }

  constexpr bool valid_segment_bytes(std::uint64_t bytes)
  {
    return bytes >= extent_header_bytes + section_alignment && bytes <= max_log_bytes &&
           bytes % section_alignment == 0;
  }

  constexpr bool valid_capacities(const CapacityLimits& limits)
  {
    const auto sets = std::uint64_t{limits._sourceSets} + limits._publishingSets;
    return limits._tasks <= max_entity_id && limits._queues <= max_entity_id && sets <= max_entity_id &&
           limits._membershipsPerSet <= max_entity_id && limits._fields <= max_fields;
  }

  // All fixed tables and the application UTF-8 pool precede the returned arena base.
  // Descriptor strings/field lists, rings, and retained history use the variable arena.
  constexpr std::optional<FixedLayout> make_fixed_layout(const CapacityLimits& limits)
  {
    if (!valid_capacities(limits))
    {
      return std::nullopt;
    }

    const auto sets = std::uint64_t{limits._sourceSets} + limits._publishingSets;
    const auto membership_count = checked_multiply(sets, limits._membershipsPerSet);
    if (!membership_count)
    {
      return std::nullopt;
    }

    FixedLayout layout{};
    layout._sections = {{
        {._kind = SectionKind::TaskIndex, ._stride = index_entry_bytes, ._count = limits._tasks},
        {._kind = SectionKind::QueueIndex, ._stride = index_entry_bytes, ._count = limits._queues},
        {._kind = SectionKind::SourceIndex, ._stride = index_entry_bytes, ._count = limits._sourceSets},
        {._kind = SectionKind::PublishingIndex, ._stride = index_entry_bytes, ._count = limits._publishingSets},
        {._kind = SectionKind::FieldIndex, ._stride = index_entry_bytes, ._count = limits._fields},
        {._kind = SectionKind::SchemaIndex, ._stride = index_entry_bytes, ._count = limits._messageSchemas},
        {._kind = SectionKind::TaskState, ._stride = task_state_bytes, ._count = limits._tasks},
        {._kind = SectionKind::QueueState, ._stride = queue_state_bytes, ._count = limits._queues},
        {._kind = SectionKind::SourceState, ._stride = source_state_bytes, ._count = limits._sourceSets},
        {._kind = SectionKind::PublishingState, ._stride = publishing_state_bytes, ._count = limits._publishingSets},
        {._kind = SectionKind::MembershipState, ._stride = membership_bytes, ._count = *membership_count},
        {._kind = SectionKind::SymbolIndex, ._stride = symbol_entry_bytes, ._count = limits._symbols},
        {._kind = SectionKind::StringPool, ._stride = 1, ._count = limits._stringPoolBytes},
    }};

    std::uint64_t end = superblock_bytes;
    for (auto& section : layout._sections)
    {
      if (section._count == 0)
      {
        continue;
      }
      const auto start = checked_align(end, section_alignment);
      const auto bytes = checked_multiply(section._count, section._stride);
      if (!start || !bytes)
      {
        return std::nullopt;
      }
      const auto next = checked_add(*start, *bytes);
      if (!next)
      {
        return std::nullopt;
      }
      section._offset = *start;
      section._byteLength = *bytes;
      end = *next;
    }

    const auto arena = checked_align(end, section_alignment);
    if (!arena || *arena > max_log_bytes)
    {
      return std::nullopt;
    }
    layout._arenaBegin = *arena;
    return layout;
  }

  constexpr std::optional<std::uint64_t> fixed_layout_bytes(const CapacityLimits& limits)
  {
    const auto layout = make_fixed_layout(limits);
    if (!layout)
    {
      return std::nullopt;
    }
    return layout->_arenaBegin;
  }

  // Ring storage is charged when a queue activates, using that queue's resolved capacity.
  constexpr std::optional<std::uint64_t> ring_storage_bytes(std::uint64_t capacity)
  {
    if (capacity == 0)
    {
      return std::nullopt;
    }
    const auto slots = checked_multiply(capacity, ring_slot_bytes);
    if (!slots)
    {
      return std::nullopt;
    }
    const auto total = checked_add(ring_header_bytes, *slots);
    if (!total)
    {
      return std::nullopt;
    }
    const auto aligned = checked_align(*total, section_alignment);
    if (!aligned || *aligned > max_log_bytes)
    {
      return std::nullopt;
    }
    return aligned;
  }
} // namespace angaria::detail::mapped
