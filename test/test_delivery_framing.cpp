// These private, local-byte fixtures connect the delivery codecs to existing
// journal framing. They do not append records, establish acceptance, resolve
// Registry/history references, or demonstrate working public Queue delivery.

#include "result_helpers.h"
#include "delivery_state.h"
#include "journal_validation.h"
#include "publication_metadata.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;

  using angaria::test_support::require_value;

  mapped::RedoCommand write_word(std::uint64_t target)
  {
    return mapped::RedoCommand{._target = target, ._payload = std::vector<std::byte>(8)};
  }

  TEST(DeliveryFramingTest, PublicationReceiptsFitJournalGeometryAndProtectedBoundaries)
  {
    // Model a future partial snapshot: the accepted destination starts Pending,
    // while the rejected destination still owns an Unused reserved receipt.
    // No capability or live Session is implied by representing this snapshot.
    constexpr std::uint64_t offset = 8192;
    const auto layout = require_value(mapped::plan_publication_layout(offset, 2, 3, 576));
    EXPECT_EQ(layout._immutableBytes, 232u);
    EXPECT_EQ(layout._receiptsOffset, 8488u);
    EXPECT_EQ(layout._totalBytes, 576u);

    mapped::PublicationDefinition publication;
    publication._producer = TaskId{1};
    publication._publishingSet = PublishingQueueSetId{2};
    publication._schema = SchemaId{MessageType{3}, 1};
    publication._producerIncarnation = 1;
    publication._publicationSequence = 1;
    publication._publishedAt = UtcTimestamp{std::chrono::nanoseconds{-9}};
    publication._topologyRevision = 1;
    publication._schemaReference = 4096;
    publication._acceptance = AcceptancePolicy::Partial;
    publication._destinations = {
        mapped::DestinationDefinition{QueueId{1}, DestinationStatus::Accepted, ErrorCode::None,
                                      1, 0, publication._publishedAt, layout._receiptsOffset},
        mapped::DestinationDefinition{QueueId{2}, DestinationStatus::Rejected, ErrorCode::NotActive,
                                      0, 0, {}, layout._receiptsOffset + 128},
    };
    publication._payload = {std::byte{0x12}, std::byte{0}, std::byte{0xff}};
    const auto immutable = require_value(mapped::encode_publication_body(publication, offset, 576));
    EXPECT_EQ(immutable.size(), 232u);
    EXPECT_EQ(require_value(mapped::decode_publication_body(immutable, offset, 576)), publication);

    mapped::Receipt pending;
    pending._state = mapped::ReceiptState::Pending;
    const auto accepted = require_value(mapped::encode_receipt(pending));
    const auto rejected = require_value(mapped::encode_receipt(mapped::Receipt{}));
    std::vector<std::byte> body(512);
    auto next = std::copy(immutable.begin(), immutable.end(), body.begin());
    next = std::copy(accepted.begin(), accepted.end(), next);
    next = std::copy(rejected.begin(), rejected.end(), next);
    EXPECT_TRUE(std::all_of(next, body.end(), [](std::byte value)
    {
      return value == std::byte{};
    }));
    EXPECT_EQ(body.end() - next, 24);

    mapped::RecordEnvelope envelope;
    envelope._kind = 9;
    envelope._totalBytes = 576;
    envelope._immutableBytes = 232;
    envelope._generation = 1;
    envelope._readiness = 2;
    const auto fixed = *mapped::make_fixed_layout(CapacityLimits::empty());
    const std::array records{require_value(mapped::validate_journal_record(
        offset, envelope, immutable, fixed._arenaBegin))};
    EXPECT_EQ(records[0]._mutableBegin, 8488u);
    EXPECT_EQ(records[0]._mutableBytes, 256u);

    // Generic redo permits only each receipt's seven declared atomic words.
    // Immutable history, recovery reservations, and final allocation padding
    // remain outside that permission, even though all are inside this record.
    for (const auto receipt : {std::uint64_t{8488}, std::uint64_t{8616}})
    {
      for (std::uint64_t word = 0; word < 7; ++word)
      {
        const std::array commands{write_word(receipt + word * 8)};
        require_value(mapped::validate_redo_targets(commands, fixed, records));
      }
      for (std::uint64_t word = 7; word < 16; ++word)
      {
        const std::array commands{write_word(receipt + word * 8)};
        const auto result = mapped::validate_redo_targets(commands, fixed, records);
        ASSERT_TRUE(std::holds_alternative<Error>(result));
        EXPECT_EQ(std::get<Error>(result)._code, ErrorCode::IoError);
      }
    }
    for (const auto target : {offset + 64, std::uint64_t{8480}, std::uint64_t{8744}})
    {
      const std::array commands{write_word(target)};
      const auto result = mapped::validate_redo_targets(commands, fixed, records);
      ASSERT_TRUE(std::holds_alternative<Error>(result));
      EXPECT_EQ(std::get<Error>(result)._code, ErrorCode::IoError);
    }
  }

  TEST(DeliveryFramingTest, OddRingCapacityHasPaddingRatherThanAnAdditionalSlot)
  {
    // After five assignments, capacity three retains sequences 3, 4, and 5.
    // Storage rounds up by 32 bytes, but capacity stays three. Distinct fabricated
    // Publication references model separate messages in this Queue's history.
    const std::array slots{
        mapped::RingSlot{4, 9408, 9216, 2},
        mapped::RingSlot{5, 10432, 10240, 2},
        mapped::RingSlot{3, 8384, 8192, 1},
    };
    const auto suffix = require_value(mapped::encode_ring_slots(slots, 5, 192));
    ASSERT_EQ(suffix.size(), 128u);
    EXPECT_TRUE(std::all_of(suffix.begin() + 96, suffix.end(), [](std::byte value)
    {
      return value == std::byte{};
    }));
    const auto decoded = require_value(mapped::decode_ring_slots(suffix, 3, 5, 192));
    EXPECT_EQ(decoded, (std::vector<mapped::RingSlot>{slots.begin(), slots.end()}));

    mapped::RecordEnvelope envelope;
    envelope._kind = 8;
    envelope._totalBytes = 192;
    envelope._generation = 1;
    envelope._readiness = 2;
    const auto range = require_value(mapped::validate_journal_record(12288, envelope, {}, 4096));
    EXPECT_EQ(range._mutableBegin, 12352u);
    EXPECT_EQ(range._mutableBytes, suffix.size());
    EXPECT_EQ(require_value(mapped::ring_record_bytes(3, 192)), envelope._totalBytes);
  }
} // namespace
