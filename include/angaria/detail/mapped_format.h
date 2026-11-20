#pragma once

#include "angaria/descriptors.h"

#include <array>
#include <climits>
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
  inline constexpr std::uint16_t format_minor = 0;
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

  // The returned arena base reserves all fixed tables and the application UTF-8 pool.
  // Descriptor strings/field lists, rings, and retained history use the variable arena.
  constexpr std::optional<std::uint64_t> fixed_layout_bytes(const CapacityLimits& limits)
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

    const std::array<std::array<std::uint64_t, 2>, 13> sections{{
        {limits._tasks, index_entry_bytes},
        {limits._queues, index_entry_bytes},
        {limits._sourceSets, index_entry_bytes},
        {limits._publishingSets, index_entry_bytes},
        {limits._fields, index_entry_bytes},
        {limits._messageSchemas, index_entry_bytes},
        {limits._tasks, task_state_bytes},
        {limits._queues, queue_state_bytes},
        {limits._sourceSets, source_state_bytes},
        {limits._publishingSets, publishing_state_bytes},
        {*membership_count, membership_bytes},
        {limits._symbols, symbol_entry_bytes},
        {limits._stringPoolBytes, 1},
    }};

    std::uint64_t end = superblock_bytes;
    for (const auto& section : sections)
    {
      if (section[0] == 0)
      {
        continue;
      }
      const auto start = checked_align(end, section_alignment);
      const auto bytes = checked_multiply(section[0], section[1]);
      if (!start || !bytes)
      {
        return std::nullopt;
      }
      const auto next = checked_add(*start, *bytes);
      if (!next)
      {
        return std::nullopt;
      }
      end = *next;
    }

    const auto arena = checked_align(end, section_alignment);
    if (!arena || *arena > max_log_bytes)
    {
      return std::nullopt;
    }
    return arena;
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
