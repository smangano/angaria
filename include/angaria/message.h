#pragma once

#include "angaria/payload.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace angaria
{
  struct Message;
  namespace detail
  {
    class MessageRetention;
    struct MessageAccess;
  } // namespace detail

  // Process-local message metadata. Native and portable encodings are defined
  // separately; this type is never copied directly into shared storage.
  struct MessageHeader
  {
    SchemaId _schema{};
    PayloadFormat _format{PayloadFormat::Binary};
    // An invalid publication identity denotes prepared outgoing input. Epoch
    // zero is a valid publication time when the identity is valid.
    PublicationId _publication{};
    UtcTimestamp _publishedAt{};
    std::uint64_t _payloadBytes{};
    std::uint32_t _flags{};
    // A publication without a selected destination has no delivery metadata.
    std::optional<QueueId> _queue;
    std::optional<std::uint64_t> _queueSequence;
    std::optional<TaskClaim> _consumer;
    std::optional<UtcTimestamp> _enqueuedAt;
    std::optional<UtcTimestamp> _dequeuedAt;

    bool operator==(const MessageHeader&) const = default;
  };

  struct MessageView
  {
    MessageHeader _header;
    std::span<const std::byte> _payload;

    MessageView() noexcept = default;
    // Raw outgoing bytes remain owned by the caller. This constructor does not
    // bind a schema catalog or extend the lifetime of the borrowed payload.
    MessageView(MessageHeader header, std::span<const std::byte> payload) noexcept;
    // Copies the complete header and bytes, retaining bound decoding metadata.
    // An unbound raw view returns InvalidState rather than guessing a Session.
    Result<Message> copy_message(std::uint64_t maxPayloadBytes) const;

  private:
    friend struct Message;
    friend struct detail::MessageAccess;
    // Owns decoding metadata only: never payload storage or a Source pin.
    std::shared_ptr<const detail::MessageRetention> _retention;
  };

  struct Message
  {
    MessageHeader _header;
    std::vector<std::byte> _payload;

    // Zero denotes an unbound manually constructed Message.
    SessionId schema_session() const noexcept;
    Result<std::vector<PayloadValue>> decode(const BinaryPayloadCodec& codec) const;
    MessageView view() const & noexcept;
    MessageView view() const && = delete;

  private:
    friend struct MessageView;
    std::shared_ptr<const detail::MessageRetention> _retention;
  };

  struct MessageContext
  {
    MessageHeader _header;
    bool _replayed{};
  };

  // The token represents the live acknowledgment obligation. Copying its
  // message does not copy or prolong that obligation.
  struct Delivery
  {
    MessageView _message;
    DeliveryToken _token{};

    MessageContext context() const noexcept;
  };

  struct DeliveryGap
  {
    QueueId _queue{};
    std::uint64_t _firstSequence{};
    std::uint64_t _lastSequence{};
  };

  struct ReceiveTimeout
  {
  };
} // namespace angaria
