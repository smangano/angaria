#include "angaria/detail/mapped_format.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;

  constexpr auto max_unsigned = std::numeric_limits<std::uint64_t>::max();
  static_assert(mapped::fixed_layout_bytes(CapacityLimits{}) == mapped::superblock_bytes);
  static_assert(mapped::ring_storage_bytes(1024) == 32832);
  static_assert(mapped::checked_align(65, 64) == 128);
  static_assert(!mapped::contains_range(max_unsigned, max_unsigned - 1, 3));

  TEST(MappedFormat, TradingTopologyHasAnExplicitFixedReservation)
  {
    const CapacityLimits limits{16, 128, 16, 16, 32, 256, 128, 1024, 1048576};
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
    CapacityLimits limits{};
    limits._sourceSets = 32768;
    limits._publishingSets = 32767;
    EXPECT_TRUE(mapped::fixed_layout_bytes(limits));

    ++limits._publishingSets;
    EXPECT_FALSE(mapped::fixed_layout_bytes(limits));
    limits._sourceSets = std::numeric_limits<std::uint32_t>::max();
    limits._publishingSets = std::numeric_limits<std::uint32_t>::max();
    EXPECT_FALSE(mapped::valid_capacities(limits));
  }

  TEST(MappedFormat, EntityCapacitiesRespectIdentifierWidths)
  {
    CapacityLimits limits{};
    limits._tasks = 65535;
    limits._queues = 65535;
    limits._membershipsPerSet = 65535;
    limits._fields = 0x01000000;
    // Schema versions and symbols have independent 32-bit table cardinalities.
    limits._messageSchemas = 65536;
    limits._symbols = 65536;
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
    CapacityLimits limits{};
    EXPECT_EQ(mapped::fixed_layout_bytes(limits), 4096u);

    limits._symbols = 1;
    limits._stringPoolBytes = 1;
    EXPECT_EQ(mapped::fixed_layout_bytes(limits), 4224u);
    limits._symbols = 0;
    EXPECT_EQ(mapped::fixed_layout_bytes(limits), 4160u);

    // Capacity reservation is independent of the currently declared topology.
    limits._membershipsPerSet = 32;
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

    CapacityLimits limits{};
    limits._stringPoolBytes = max_unsigned;
    EXPECT_FALSE(mapped::fixed_layout_bytes(limits));
    limits._stringPoolBytes = mapped::max_log_bytes - mapped::superblock_bytes;
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
