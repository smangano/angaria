#include "angaria/detail/mapped_format.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

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

  TEST(MappedFormat, RangeChecksCannotWrapAroundTheExtent)
  {
    EXPECT_TRUE(mapped::contains_range(100, 20, 80));
    EXPECT_TRUE(mapped::contains_range(100, 100, 0));
    EXPECT_FALSE(mapped::contains_range(100, 101, 0));
    EXPECT_FALSE(mapped::contains_range(100, 20, 81));
    EXPECT_TRUE(mapped::contains_range(max_unsigned, max_unsigned - 1, 1));
    EXPECT_FALSE(mapped::contains_range(max_unsigned, max_unsigned - 1, 3));
  }

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

  TEST(MappedFormat, RingReservationsRejectZeroAndUnrepresentableCapacities)
  {
    EXPECT_FALSE(mapped::ring_storage_bytes(0));
    EXPECT_EQ(mapped::ring_storage_bytes(1), 128u);
    EXPECT_EQ(mapped::ring_storage_bytes(2), 128u);
    EXPECT_EQ(mapped::ring_storage_bytes(3), 192u);
    EXPECT_FALSE(mapped::ring_storage_bytes(max_unsigned));
    EXPECT_FALSE(mapped::ring_storage_bytes(mapped::max_log_bytes / mapped::ring_slot_bytes));
  }

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
