// Private allocation examples, not public Queue send calls. These size-only
// plans include every record, mutable receipt, manifest reference, redo frame,
// and alignment byte without constructing payload or receipt buffers.

#include "angaria/detail/append_layout.h"
#include "publication_metadata.h"
#include "result_helpers.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace
{
  namespace mapped = angaria::detail::mapped;
  using angaria::test_support::require_value;

  TEST(AppendLayoutTest, ImmutableAndMutableSuffixesUseTheJournalEnvelopeGeometry)
  {
    const auto immutable = mapped::record_storage_layout(3);
    ASSERT_TRUE(immutable);
    EXPECT_EQ(immutable->_totalBytes, 128u);
    EXPECT_EQ(immutable->_immutableBytes, 64u);
    EXPECT_EQ(immutable->_mutableOffset, 72u);
    const auto mutableSuffix = mapped::record_storage_layout(3, 128);
    ASSERT_TRUE(mutableSuffix);
    EXPECT_EQ(mutableSuffix->_totalBytes, 256u);
    EXPECT_EQ(mutableSuffix->_immutableBytes, 8u);
    EXPECT_EQ(mutableSuffix->_mutableOffset, 72u);
  }

  TEST(AppendLayoutTest, PublicationBudgetIncludesReceiptsAndTheFinalManifest)
  {
    // One destination and three application bytes require 384 stored bytes.
    // Its manifest needs another 192 bytes: fixed64, reference8, redo24,
    // envelope64, and final alignment. The last application byte is not the cap.
    const auto publication = require_value(mapped::plan_publication_layout(8192, 1, 3, 384));
    mapped::AppendLayoutPlanner planner(8192);
    ASSERT_TRUE(planner.add_record(publication._immutableBytes, 128));
    const auto layout = planner.finish(24, 8768);
    ASSERT_TRUE(layout);
    EXPECT_EQ(layout->_manifestOffset, 8576u);
    EXPECT_EQ(layout->_manifestBodyBytes, 96u);
    EXPECT_EQ(layout->_manifestBytes, 192u);
    EXPECT_EQ(layout->_tail, 8768u);
    EXPECT_FALSE(planner.finish(24, 8767));
    EXPECT_FALSE(planner.finish(24, 8576));
  }

  TEST(AppendLayoutTest, EveryRecordContributesItsOwnEnvelopePaddingAndManifestReference)
  {
    mapped::AppendLayoutPlanner planner(8192);
    ASSERT_TRUE(planner.add_record(1, 8));
    ASSERT_TRUE(planner.add_record(64));
    const auto layout = planner.finish(40, 8640);
    ASSERT_TRUE(layout);
    EXPECT_EQ(layout->_manifestOffset, 8448u);
    EXPECT_EQ(layout->_manifestBodyBytes, 120u);
    EXPECT_EQ(layout->_manifestBytes, 192u);
    EXPECT_EQ(layout->_tail, 8640u);
    EXPECT_FALSE(planner.finish(40, 8639));
  }

  TEST(AppendLayoutTest, LargeLogicalPayloadPlanningDoesNotAllocateItsDeclaredCapacity)
  {
    constexpr std::uint64_t payloadBytes = std::uint64_t{1} << 40;
    const auto publication = require_value(mapped::plan_publication_layout(
        8192, 2, payloadBytes, mapped::max_log_bytes));
    mapped::AppendLayoutPlanner planner(8192);
    ASSERT_TRUE(planner.add_record(publication._immutableBytes, 256));
    const auto layout = planner.finish(48);
    ASSERT_TRUE(layout);
    EXPECT_EQ(layout->_manifestOffset, 8192 + payloadBytes + 576);
    EXPECT_EQ(layout->_manifestBytes, 192u);
    EXPECT_EQ(layout->_tail, 8192 + payloadBytes + 768);
  }

  TEST(AppendLayoutTest, FormatCeilingAndArithmeticOverflowRejectWithoutWrappedSizes)
  {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    EXPECT_FALSE(mapped::record_storage_layout(maximum, 0));
    EXPECT_FALSE(mapped::record_storage_layout(8, maximum));
    EXPECT_FALSE(mapped::record_storage_layout(mapped::max_log_bytes, 0));
    mapped::AppendLayoutPlanner planner(8192);
    ASSERT_TRUE(planner.add_record(1));
    EXPECT_FALSE(planner.finish(maximum));
    EXPECT_FALSE(planner.finish(mapped::max_log_bytes));
  }

  TEST(AppendLayoutTest, LastAlignedAppendFitsOnlyWhenItsManifestAlsoFits)
  {
    constexpr auto alignedLimit = mapped::max_log_bytes - mapped::max_log_bytes % 64;
    mapped::AppendLayoutPlanner fitting(alignedLimit - 256);
    ASSERT_TRUE(fitting.add_record(0));
    const auto layout = fitting.finish(0);
    ASSERT_TRUE(layout);
    EXPECT_EQ(layout->_tail, alignedLimit);
    mapped::AppendLayoutPlanner missingManifest(alignedLimit - 192);
    ASSERT_TRUE(missingManifest.add_record(0));
    EXPECT_FALSE(missingManifest.finish(0));
  }

  TEST(AppendLayoutTest, InvalidOriginsAndFailedRecordsCannotBecomeSuccessfulPlans)
  {
    mapped::AppendLayoutPlanner unaligned(8193);
    EXPECT_FALSE(unaligned.add_record(8));
    EXPECT_FALSE(unaligned.finish(0));
    mapped::AppendLayoutPlanner outside(std::numeric_limits<std::uint64_t>::max());
    EXPECT_FALSE(outside.finish(0));
    mapped::AppendLayoutPlanner failed(8192);
    EXPECT_FALSE(failed.add_record(std::numeric_limits<std::uint64_t>::max()));
    EXPECT_FALSE(failed.add_record(8));
    EXPECT_FALSE(failed.finish(0));
  }
}
