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

  TEST(Identities, SchemaVersionIsPartOfIdentity)
  {
    const SchemaId original{MessageType{1}, 1};

    EXPECT_EQ(original, (SchemaId{MessageType{1}, 1}));
    EXPECT_NE(original, (SchemaId{MessageType{1}, 2}));
    EXPECT_NE(original, (SchemaId{MessageType{2}, 1}));
  }

  TEST(Identities, PublicationIdentityIncludesProducerClaimAndSequence)
  {
    const PublicationId original{{{1, 2}, TaskId{3}, 4}, 5};

    EXPECT_EQ(original, (PublicationId{{{1, 2}, TaskId{3}, 4}, 5}));
    EXPECT_NE(original, (PublicationId{{{1, 2}, TaskId{3}, 6}, 5}));
    EXPECT_NE(original, (PublicationId{{{1, 2}, TaskId{3}, 4}, 6}));
  }

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
