#include "angaria/detail/mapped_format.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

// These pure layout tests explain how configured capacities reserve format-2
// storage. They allocate no files or Sessions; applications normally supply
// CapacityLimits to Session::create rather than construct these private layouts.
namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;

  constexpr auto max_unsigned = std::numeric_limits<std::uint64_t>::max();
  static_assert(mapped::fixed_layout_bytes(CapacityLimits::empty()) == mapped::superblock_bytes);
  static_assert(mapped::fixed_layout_bytes(CapacityLimits{}) == 1197056);
  static_assert(mapped::ring_storage_bytes(1024) == 32832);
  static_assert(mapped::checked_align(65, 64) == 128);
  static_assert(!mapped::contains_range(max_unsigned, max_unsigned - 1, 3));

  // Give each facility a different capacity so swapped tables or reused counts
  // cannot accidentally produce the expected layout. Offsets include padding
  // between sections, not just the sum of their payload sizes.
  TEST(MappedFormat, AsymmetricCapacitiesProduceTheSpecifiedSectionDirectory)
  {
    const auto limits = CapacityLimits::empty()
                            .tasks(2)
                            .queues(3)
                            .source_sets(1)
                            .publishing_sets(2)
                            .memberships_per_set(4)
                            .message_schemas(5)
                            .symbols(1)
                            .string_pool_bytes(67);
    const auto layout = mapped::make_fixed_layout(limits);
    ASSERT_TRUE(layout);

    // Independently calculated from format 2.0's table order, widths, and 64-byte alignment.
    const std::array<std::uint64_t, 13> offsets{
        4096, 4224, 4416, 4480, 0, 4608, 4928, 5440, 6208, 6464, 6720, 7104, 7168,
    };
    const std::array<std::uint64_t, 13> counts{2, 3, 1, 2, 0, 5, 2, 3, 1, 2, 12, 1, 67};
    const std::array<std::uint32_t, 13> strides{
        64, 64, 64, 64, 64, 64, 256, 256, 256, 128, 32, 32, 1,
    };
    const std::array<std::uint64_t, 13> lengths{
        128, 192, 64, 128, 0, 320, 512, 768, 256, 256, 384, 32, 67,
    };
    ASSERT_EQ(layout->_sections.size(), offsets.size());
    for (std::size_t i = 0; i < offsets.size(); ++i)
    {
      SCOPED_TRACE(i);
      const auto& section = layout->_sections[i];
      EXPECT_EQ(static_cast<std::uint16_t>(section._kind), i + 1);
      EXPECT_EQ(section._flags, 0u);
      EXPECT_EQ(section._offset, offsets[i]);
      EXPECT_EQ(section._count, counts[i]);
      EXPECT_EQ(section._stride, strides[i]);
      EXPECT_EQ(section._byteLength, lengths[i]);
    }
    EXPECT_EQ(layout->_arenaBegin, 7296u);
    EXPECT_EQ(mapped::fixed_layout_bytes(limits), layout->_arenaBegin);
  }

  // A zero capacity disables storage for that facility while retaining its
  // directory description. The superblock still exists even with no table rows.
  TEST(MappedFormat, EmptyDirectoryRetainsKindsAndStridesWithoutReservingBytes)
  {
    constexpr auto layout = mapped::make_fixed_layout(CapacityLimits::empty());
    static_assert(layout.has_value());
    static_assert(layout->_arenaBegin == 4096);
    const std::array<std::uint32_t, 13> strides{
        64, 64, 64, 64, 64, 64, 256, 256, 256, 128, 32, 32, 1,
    };
    ASSERT_EQ(layout->_sections.size(), strides.size());
    for (std::size_t i = 0; i < strides.size(); ++i)
    {
      SCOPED_TRACE(i);
      const auto& section = layout->_sections[i];
      EXPECT_EQ(static_cast<std::uint16_t>(section._kind), i + 1);
      EXPECT_EQ(section._stride, strides[i]);
      EXPECT_EQ(section._flags, 0u);
      EXPECT_EQ(section._offset, 0u);
      EXPECT_EQ(section._count, 0u);
      EXPECT_EQ(section._byteLength, 0u);
    }
  }

  // Budget a trading Session in two parts: fixed tables from CapacityLimits,
  // then rings when Queues activate. Reserving Queue identities alone does not
  // charge ring bytes; definitions and retained history also need log space.
  TEST(MappedFormat, TradingTopologyHasAnExplicitFixedReservation)
  {
    const auto limits = CapacityLimits{}.tasks(16).queues(128).source_sets(16);
    const auto fixed = mapped::fixed_layout_bytes(limits);

    ASSERT_TRUE(fixed);
    EXPECT_EQ(*fixed, 1197056u);
    EXPECT_EQ(*fixed % mapped::section_alignment, 0u);
    // Reserved Queue entries do not allocate rings until activation.
    EXPECT_EQ(mapped::ring_storage_bytes(1024), 32832u);
    EXPECT_LT(*fixed + 3 * *mapped::ring_storage_bytes(1024), 67108864u);
  }

  // Source and Publishing sets have distinct C++ types, but together consume
  // one bounded Queue Set ID space. Separate limits cannot each claim its full range.
  TEST(MappedFormat, QueueSetKindsShareOneIdentityNamespace)
  {
    auto limits = CapacityLimits::empty().source_sets(32768).publishing_sets(32767);
    EXPECT_TRUE(mapped::fixed_layout_bytes(limits));

    ++limits._publishingSets;
    EXPECT_FALSE(mapped::fixed_layout_bytes(limits));
    limits.source_sets(std::numeric_limits<std::uint32_t>::max())
        .publishing_sets(std::numeric_limits<std::uint32_t>::max());
    EXPECT_FALSE(mapped::valid_capacities(limits));
  }

  // Capacity checks follow each entity's encoded identity width. In particular,
  // Field and schema reservations must not inherit the smaller Task/Queue limit.
  TEST(MappedFormat, EntityCapacitiesRespectIdentifierWidths)
  {
    // Schema versions and symbols have independent 32-bit table cardinalities.
    const auto limits = CapacityLimits::empty()
                            .tasks(65535)
                            .queues(65535)
                            .memberships_per_set(65535)
                            .fields(0x01000000)
                            .message_schemas(65536)
                            .symbols(65536);
    EXPECT_TRUE(mapped::fixed_layout_bytes(limits));

    auto invalid = limits;
    ++invalid._tasks;
    EXPECT_FALSE(mapped::fixed_layout_bytes(invalid));
    invalid = limits;
    ++invalid._queues;
    EXPECT_FALSE(mapped::fixed_layout_bytes(invalid));
    invalid = limits;
    ++invalid._membershipsPerSet;
    EXPECT_FALSE(mapped::fixed_layout_bytes(invalid));
    invalid = limits;
    ++invalid._fields;
    EXPECT_FALSE(mapped::fixed_layout_bytes(invalid));
  }

  // Even a one-byte application string pool affects the aligned arena start.
  // Empty facilities consume no rows, and membership capacity alone creates no sets.
  TEST(MappedFormat, EmptyFacilitiesAndOddSizedPoolsKeepTheArenaAligned)
  {
    auto limits = CapacityLimits::empty();
    EXPECT_EQ(mapped::fixed_layout_bytes(limits), 4096u);

    limits.symbols(1).string_pool_bytes(1);
    EXPECT_EQ(mapped::fixed_layout_bytes(limits), 4224u);
    limits.symbols(0);
    EXPECT_EQ(mapped::fixed_layout_bytes(limits), 4160u);

    // Capacity reservation is independent of the currently declared topology.
    limits.memberships_per_set(32);
    EXPECT_TRUE(mapped::fixed_layout_bytes(limits));
  }

  // Layout arithmetic is checked before it can size a file or mapping. Values
  // near the integer limit must fail instead of wrapping to a small allocation.
  TEST(MappedFormat, CheckedArithmeticRejectsOverflowBeforeAllocating)
  {
    EXPECT_EQ(mapped::checked_add(max_unsigned - 1, 1), max_unsigned);
    EXPECT_FALSE(mapped::checked_add(max_unsigned, 1));
    EXPECT_EQ(mapped::checked_multiply(0, max_unsigned), 0u);
    EXPECT_EQ(mapped::checked_multiply(max_unsigned, 1), max_unsigned);
    EXPECT_FALSE(mapped::checked_multiply(max_unsigned / 2 + 1, 2));
    EXPECT_EQ(mapped::checked_align(0, 64), 0u);
    EXPECT_EQ(mapped::checked_align(64, 64), 64u);
    EXPECT_FALSE(mapped::checked_align(0, 0));
    EXPECT_FALSE(mapped::checked_align(max_unsigned, 64));
  }

  // A range ending exactly at the extent boundary is valid, including an empty
  // range there. An overflowing offset-plus-length must never pass that check.
  TEST(MappedFormat, RangeChecksCannotWrapAroundTheExtent)
  {
    EXPECT_TRUE(mapped::contains_range(100, 20, 80));
    EXPECT_TRUE(mapped::contains_range(100, 100, 0));
    EXPECT_FALSE(mapped::contains_range(100, 101, 0));
    EXPECT_FALSE(mapped::contains_range(100, 20, 81));
    EXPECT_TRUE(mapped::contains_range(max_unsigned, max_unsigned - 1, 1));
    EXPECT_FALSE(mapped::contains_range(max_unsigned, max_unsigned - 1, 3));
  }

  // The byte ceiling includes the superblock and alignment padding. A requested
  // pool that seems to fill the remaining bytes can still exceed that ceiling
  // when the arena start is rounded up.
  TEST(MappedFormat, ByteLimitsIncludeReservationAndAlignment)
  {
    EXPECT_FALSE(mapped::valid_log_limit(0));
    EXPECT_TRUE(mapped::valid_log_limit(mapped::max_log_bytes));
    EXPECT_FALSE(mapped::valid_log_limit(mapped::max_log_bytes + 1));

    auto limits = CapacityLimits::empty().string_pool_bytes(max_unsigned);
    EXPECT_FALSE(mapped::fixed_layout_bytes(limits));
    limits.string_pool_bytes(mapped::max_log_bytes - mapped::superblock_bytes);
    EXPECT_FALSE(mapped::fixed_layout_bytes(limits));
    limits._stringPoolBytes -= 63;
    EXPECT_EQ(mapped::fixed_layout_bytes(limits), mapped::max_log_bytes - 63);
  }

  // Queue capacity counts slots, while the storage budget includes the record
  // envelope and padding. Small capacities can therefore share an allocation size.
  TEST(MappedFormat, RingReservationsRejectZeroAndUnrepresentableCapacities)
  {
    EXPECT_FALSE(mapped::ring_storage_bytes(0));
    EXPECT_EQ(mapped::ring_storage_bytes(1), 128u);
    EXPECT_EQ(mapped::ring_storage_bytes(2), 128u);
    EXPECT_EQ(mapped::ring_storage_bytes(3), 192u);
    EXPECT_FALSE(mapped::ring_storage_bytes(max_unsigned));
    EXPECT_FALSE(mapped::ring_storage_bytes(mapped::max_log_bytes / mapped::ring_slot_bytes));
  }

  // These are format geometry checks for segmented storage, not a runnable
  // segmented backend. A segment must leave aligned data space after its header.
  TEST(MappedFormat, SegmentQuantaLeaveAlignedSpaceAfterTheirHeader)
  {
    EXPECT_FALSE(mapped::valid_segment_bytes(0));
    EXPECT_FALSE(mapped::valid_segment_bytes(4096));
    EXPECT_FALSE(mapped::valid_segment_bytes(4159));
    EXPECT_TRUE(mapped::valid_segment_bytes(4160));
    EXPECT_FALSE(mapped::valid_segment_bytes(4161));
    EXPECT_TRUE(mapped::valid_segment_bytes(1048576));
    EXPECT_TRUE(mapped::valid_segment_bytes(mapped::max_log_bytes - 63));
    EXPECT_FALSE(mapped::valid_segment_bytes(mapped::max_log_bytes));
    EXPECT_FALSE(mapped::valid_segment_bytes(max_unsigned));
  }
} // namespace
