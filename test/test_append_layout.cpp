// Private allocation examples, not public Queue send calls. These size-only
// plans cover administrative records/manifests independently of the producer
// message record, whose receipts live in a separate consumer-owned file.

#include "angaria/detail/append_layout.h"
#include "producer_log_format.h"
#include "result_helpers.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <variant>

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;
  namespace recorded = angaria::detail::recorded;
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

  TEST(AppendLayoutTest, ProducerRecordBudgetHasNoAdministrativeManifestOrProcessingReceiptSuffix)
  {
    // Independently: header128 + destination32 + payload3 rounds to192 bytes.
    // The producer's accepted message record is independent of both the Session
    // administrative manifest and the consumer's processing-progress storage.
    recorded::ProducerLogHeader header{{1, 2}, TaskId{3}, 1, 4288, 1, 0x20002};
    const std::array payload{std::byte{1}, std::byte{2}, std::byte{3}};
    const std::array destinations{recorded::DestinationRecord{
        QueueId{4}, DestinationStatus::Accepted, ErrorCode::None, 1, UtcTimestamp{}}};
    recorded::PublicationRecordInput publication;
    publication._header._schema = {MessageType{1}, 1};
    publication._header._format = PayloadFormat::Binary;
    publication._header._publication = {{header._session, header._producer, 1}, 1};
    publication._header._publishedAt = UtcTimestamp{};
    publication._header._payloadBytes = payload.size();
    publication._recordOrdinal = 1;
    publication._schemaReference = 8192;
    publication._topologyRevision = 1;
    publication._destinations = destinations;
    publication._payload = payload;
    const auto layout = require_value(recorded::plan_publication_record(publication, header, 4096));
    EXPECT_EQ(layout._bytes, 192u);
    EXPECT_EQ(layout._end, 4288u);
    EXPECT_EQ(layout._payloadOffset, 160u);
    header._fileBytes = 4287;
    EXPECT_TRUE(std::holds_alternative<Error>(recorded::plan_publication_record(publication, header, 4096)));
    header._fileBytes = 4288;
    const std::array<std::byte, 65> largerPayload{};
    publication._payload = largerPayload;
    publication._header._payloadBytes = largerPayload.size();
    EXPECT_TRUE(std::holds_alternative<Error>(recorded::plan_publication_record(publication, header, 4096)));
    header._fileBytes = 4352;
    const auto larger = require_value(recorded::plan_publication_record(publication, header, 4096));
    EXPECT_EQ(larger._bytes, 256u);
    EXPECT_EQ(larger._end, 4352u);
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

  TEST(AppendLayoutTest, LargeAdministrativeDefinitionPlanningDoesNotAllocateItsDeclaredCapacity)
  {
    constexpr std::uint64_t definitionBytes = std::uint64_t{1} << 40;
    mapped::AppendLayoutPlanner planner(8192);
    ASSERT_TRUE(planner.add_record(definitionBytes));
    const auto layout = planner.finish(48);
    ASSERT_TRUE(layout);
    EXPECT_EQ(layout->_manifestOffset, 8192 + definitionBytes + 64);
    EXPECT_EQ(layout->_manifestBytes, 192u);
    EXPECT_EQ(layout->_tail, 8192 + definitionBytes + 256);
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
