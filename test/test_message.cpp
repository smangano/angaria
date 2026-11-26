// Message values own bytes; MessageView values borrow them. Native receive is
// implemented in the following step. The private binding fixture below supplies
// only retained schema context and does not simulate a Source or acknowledgment.
#include "angaria/message.h"
#include "message_retention.h"
#include "result_assertions.h"
#include "result_helpers.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  using angaria::test_support::expect_error;
  using angaria::test_support::require_value;

  constexpr SessionId schema_session{1, 2};
  constexpr SchemaId schema{MessageType{17}, 1};

  RegistrySnapshot definitions(EFieldBase base = EFieldBase::BASE_UINT32)
  {
    RegistrySnapshot result;
    result._revision = 1;
    result._currentRevision = 1;
    FieldDescriptor field;
    field._id = FieldId{0xee000001};
    field._name = "Value";
    field._base = base;
    field._size = 4;
    const RegistrationMetadata metadata{TaskId{1}, UtcTimestamp{}, 1};
    result._fields.push_back({field, metadata});
    MessageDescriptor message;
    message._schema = schema;
    message._name = "Sample";
    message._fields.push_back(field._id);
    result._messages.push_back({message, metadata});
    return result;
  }

  BinaryPayloadCodec codec(EFieldBase base = EFieldBase::BASE_UINT32)
  {
    return require_value(BinaryPayloadCodec::from_snapshot(definitions(base), schema, 4));
  }

  MessageHeader received_header()
  {
    MessageHeader result;
    result._schema = schema;
    result._publication = {{schema_session, TaskId{1}, 3}, 7};
    result._publishedAt = UtcTimestamp{std::chrono::nanoseconds{-5}};
    result._payloadBytes = 4;
    result._flags = 0xdeadbeef;
    result._queue = QueueId{4};
    result._queueSequence = 12;
    result._consumer = TaskClaim{schema_session, TaskId{2}, 6};
    // Presence matters even when a timestamp is exactly the Unix epoch.
    result._enqueuedAt = UtcTimestamp{};
    result._dequeuedAt = UtcTimestamp{std::chrono::nanoseconds{9}};
    return result;
  }

  template <typename T>
  concept HasTemporaryView = requires(T&& value)
  {
    std::move(value).view();
  };

  static_assert(std::is_copy_constructible_v<Message>);
  static_assert(std::is_same_v<decltype(Message::_header), decltype(MessageView::_header)>);
  static_assert(!HasTemporaryView<Message>);
  static_assert(!HasTemporaryView<const Message>);

  TEST(MessageTest, RawOutgoingViewsCarryAHeaderButDoNotGuessDecodingContext)
  {
    const std::array<std::byte, 4> bytes{};
    MessageHeader header;
    header._schema = schema;
    header._payloadBytes = bytes.size();
    const MessageView outgoing{header, bytes};
    EXPECT_EQ(outgoing._header, header);
    EXPECT_EQ(outgoing._payload.data(), bytes.data());
    EXPECT_EQ(outgoing._header._publication, PublicationId{});
    EXPECT_FALSE(outgoing._header._queue);
    expect_error(outgoing.copy_message(bytes.size()), ErrorCode::InvalidState);
    // A raw view is a valid send input. An owning framework copy additionally
    // requires the exact Session/schema definitions supplied by the runtime.
  }

  TEST(MessageTest, OwningCopyPreservesEveryHeaderFieldAndRetainsIndependentBytesAndSchema)
  {
    const auto expected = received_header();
    auto owned = [&]
    {
      auto source_codec = codec();
      auto bytes = require_value(source_codec.encode({std::uint32_t{42}}));
      const auto borrowed = require_value(detail::MessageAccess::bind(expected, bytes, schema_session, source_codec));
      const auto second_view = borrowed;
      EXPECT_EQ(second_view._payload.data(), bytes.data());
      auto result = require_value(borrowed.copy_message(4));
      EXPECT_EQ(result._header, expected);
      EXPECT_NE(result._payload.data(), bytes.data());
      bytes[0] = std::byte{};
      EXPECT_EQ(result._payload[0], std::byte{42});
      return result;
    }(); // Both the input bytes and the original resolved codec now close.

    EXPECT_EQ(owned.schema_session(), schema_session);
    EXPECT_EQ(owned._header, expected);
    EXPECT_EQ(std::get<std::uint32_t>(require_value(owned.decode(codec()))[0]), 42u);
    const auto view = owned.view();
    EXPECT_EQ(view._header, expected);
    EXPECT_EQ(view._payload.data(), owned._payload.data());
    const auto another_copy = require_value(view.copy_message(4));
    EXPECT_EQ(another_copy._header, expected);
    EXPECT_EQ(another_copy._payload, owned._payload);
    EXPECT_EQ(another_copy.schema_session(), schema_session);
  }

  TEST(MessageTest, CopyCapacityFailureLeavesTheBorrowAndItsContextUsable)
  {
    auto source_codec = codec();
    const auto bytes = require_value(source_codec.encode({std::uint32_t{42}}));
    const auto view = require_value(detail::MessageAccess::bind(received_header(), bytes, schema_session, source_codec));
    expect_error(view.copy_message(3), ErrorCode::CapacityExceeded);
    EXPECT_EQ(view._header, received_header());
    EXPECT_EQ(view._payload.data(), bytes.data());
    EXPECT_EQ(require_value(view.copy_message(4))._payload, bytes);
  }

  TEST(MessageTest, BindingAndDecodingRejectConflictingSessionAndExactSchemaDefinitions)
  {
    auto source_codec = codec();
    const auto bytes = require_value(source_codec.encode({std::uint32_t{42}}));
    auto header = received_header();
    header._publication._producer._session = {9, 9};
    expect_error(detail::MessageAccess::bind(header, bytes, schema_session, source_codec), ErrorCode::SchemaConflict);
    header = received_header();
    header._schema._version = 2;
    expect_error(detail::MessageAccess::bind(header, bytes, schema_session, source_codec), ErrorCode::SchemaConflict);
    header = received_header();
    header._payloadBytes = 3;
    expect_error(detail::MessageAccess::bind(header, bytes, schema_session, source_codec), ErrorCode::InvalidArgument);

    auto owned = require_value(require_value(detail::MessageAccess::bind(
        received_header(), bytes, schema_session, source_codec)).copy_message(4));
    // Schema IDs are local to a Session. Matching IDs alone cannot authorize a
    // differently resolved codec, even when the packed byte widths match.
    expect_error(owned.decode(codec(EFieldBase::BASE_INT32)), ErrorCode::SchemaConflict);
    owned._header._format = PayloadFormat::Text;
    expect_error(owned.decode(source_codec), ErrorCode::UnsupportedPolicy);
  }

  TEST(MessageTest, DeliveryContextCopiesHeaderWithoutTransferringAcknowledgmentState)
  {
    const std::array<std::byte, 4> bytes{};
    const DeliveryToken token{{schema_session, TaskId{2}, 6}, SourceQueueSetId{3}, QueueId{4}, 12, 8};
    const Delivery delivery{MessageView{received_header(), bytes}, token};
    const auto context = delivery.context();
    EXPECT_EQ(context._header, delivery._message._header);
    EXPECT_FALSE(context._replayed);
    EXPECT_EQ(delivery._token, token);
  }
} // namespace
