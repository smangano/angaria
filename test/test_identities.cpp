// Strong identity types keep unrelated entities apart at compile time. The
// literal values below isolate equality rules. Publication IDs come from
// Task::next_publication_id(); delivery tokens describe the planned receive contract.

#include "angaria/identities.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

namespace
{
  using namespace angaria;

  static_assert(std::is_same_v<std::underlying_type_t<TaskId>, std::uint16_t>);
  static_assert(std::is_same_v<std::underlying_type_t<QueueId>, std::uint16_t>);
  static_assert(std::is_same_v<std::underlying_type_t<SourceQueueSetId>, std::uint16_t>);
  static_assert(std::is_same_v<std::underlying_type_t<PublishingQueueSetId>, std::uint16_t>);
  static_assert(std::is_same_v<std::underlying_type_t<MessageType>, std::uint16_t>);
  static_assert(std::is_same_v<std::underlying_type_t<FieldId>, std::uint32_t>);

  static_assert(!std::is_convertible_v<TaskId, QueueId>);
  static_assert(!std::is_convertible_v<SourceQueueSetId, PublishingQueueSetId>);
  static_assert(!std::is_convertible_v<MessageType, FieldId>);
  static_assert(!std::is_convertible_v<std::uint16_t, TaskId>);
  static_assert(!std::is_convertible_v<TaskId, std::uint16_t>);

  constexpr DeliveryToken EMPTY_TOKEN{};
  static_assert(EMPTY_TOKEN._consumer._session == SessionId{});
  static_assert(EMPTY_TOKEN._consumer._task == TaskId::Invalid);
  static_assert(EMPTY_TOKEN._consumer._incarnation == 0);
  static_assert(EMPTY_TOKEN._sourceSet == SourceQueueSetId::Invalid);
  static_assert(EMPTY_TOKEN._queue == QueueId::Invalid);
  static_assert(EMPTY_TOKEN._sequence == 0);
  static_assert(EMPTY_TOKEN._issuance == 0);

  // A message type can retain several immutable schema versions. Changing either
  // the type or version selects a different schema rather than changing the
  // meaning of an existing identity.
  TEST(Identities, SchemaVersionIsPartOfIdentity)
  {
    const SchemaId original{MessageType{1}, 1};

    EXPECT_EQ(original, (SchemaId{MessageType{1}, 1}));
    EXPECT_NE(original, (SchemaId{MessageType{1}, 2}));
    EXPECT_NE(original, (SchemaId{MessageType{2}, 1}));
  }

  // A sequence is unique only within its producer claim. Reusing sequence five
  // after a Task restart must not collide with sequence five from its previous
  // incarnation; comparing only the sequence would lose that distinction.
  TEST(Identities, PublicationIdentityIncludesProducerClaimAndSequence)
  {
    const PublicationId original{{{1, 2}, TaskId{3}, 4}, 5};

    EXPECT_EQ(original, (PublicationId{{{1, 2}, TaskId{3}, 4}, 5}));
    EXPECT_NE(original, (PublicationId{{{1, 2}, TaskId{3}, 6}, 5}));
    EXPECT_NE(original, (PublicationId{{{1, 2}, TaskId{3}, 4}, 6}));
  }

  // Keep the entire token returned by a receive operation for acknowledgment.
  // The claim, Source, Queue, sequence, and issuance all contribute to equality,
  // so an older issuance cannot masquerade as the current delivery.
  TEST(Identities, DeliveryTokenDistinguishesClaimAndIssuance)
  {
    const DeliveryToken original{{{1, 2}, TaskId{3}, 4}, SourceQueueSetId{5}, QueueId{6}, 7, 8};

    EXPECT_EQ(original, (DeliveryToken{{{1, 2}, TaskId{3}, 4}, SourceQueueSetId{5}, QueueId{6}, 7, 8}));

    auto changed = original;
    ++changed._consumer._session._high;
    EXPECT_NE(original, changed);

    changed = original;
    ++changed._consumer._session._low;
    EXPECT_NE(original, changed);

    changed = original;
    changed._consumer._task = TaskId{9};
    EXPECT_NE(original, changed);

    changed = original;
    ++changed._consumer._incarnation;
    EXPECT_NE(original, changed);

    changed = original;
    changed._sourceSet = SourceQueueSetId{9};
    EXPECT_NE(original, changed);

    changed = original;
    changed._queue = QueueId{9};
    EXPECT_NE(original, changed);

    changed = original;
    ++changed._sequence;
    EXPECT_NE(original, changed);

    changed = original;
    ++changed._issuance;
    EXPECT_NE(original, changed);
  }
} // namespace
