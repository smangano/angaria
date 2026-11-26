#include "angaria/message.h"

#include "angaria/detail/checked_arithmetic.h"
#include "message_retention.h"
#include "runtime_state.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace angaria
{
  namespace
  {
    void check_bound_message(const MessageHeader& header, std::span<const std::byte> payload,
                                const std::shared_ptr<const detail::MessageRetention>& retention)
    {
      if (!retention)
      {
        detail::runtime_fail(ErrorCode::InvalidState, "message.context", "Message has no retained decoding context");
      }
      if (header._payloadBytes != payload.size())
      {
        detail::runtime_fail(ErrorCode::InvalidArgument, "message.payload", "Declared payload length must equal the supplied bytes");
      }
      if (header._schema != retention->_codec.schema())
      {
        detail::runtime_fail(ErrorCode::SchemaConflict, "message.schema", "Message schema differs from its retained decoding context");
      }
      if (header._publication != PublicationId{} &&
          header._publication._producer._session != retention->_session)
      {
        detail::runtime_fail(ErrorCode::SchemaConflict, "message.session", "Message publication belongs to a different schema Session");
      }
    }

    bool same_field(const FieldDescriptor& left, const FieldDescriptor& right)
    {
      return left._id == right._id && left._name == right._name &&
             left._base == right._base && left._size == right._size &&
             left._doc == right._doc && left._logical == right._logical;
    }
  } // namespace

  MessageView::MessageView(MessageHeader header, std::span<const std::byte> payload) noexcept
      : _header(std::move(header)), _payload(payload)
  {
  }

  Result<Message> MessageView::copy_message(std::uint64_t maxPayloadBytes) const
  {
    try
    {
      check_bound_message(_header, _payload, _retention);
      if (_payload.size() > maxPayloadBytes || !detail::fits_host_size(_payload.size()) ||
          _payload.size() > std::vector<std::byte>{}.max_size())
      {
        detail::runtime_fail(ErrorCode::CapacityExceeded, "message.capacity", "Message exceeds the owning copy byte limit");
      }
      Message result;
      result._header = _header;
      if (!_payload.empty())
      {
        result._payload.assign(_payload.begin(), _payload.end());
      }
      result._retention = _retention;
      return result;
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  SessionId Message::schema_session() const noexcept
  {
    return _retention ? _retention->_session : SessionId{};
  }

  Result<std::vector<PayloadValue>> Message::decode(const BinaryPayloadCodec& codec) const
  {
    try
    {
      check_bound_message(_header, _payload, _retention);
      if (_header._format != PayloadFormat::Binary)
      {
        detail::runtime_fail(ErrorCode::UnsupportedPolicy, "message.format", "Binary decoding requires the Binary payload format");
      }
      const auto expected = _retention->_codec.fields();
      const auto supplied = codec.fields();
      if (codec.schema() != _header._schema || codec.payload_size() != _retention->_codec.payload_size() ||
          !std::equal(expected.begin(), expected.end(), supplied.begin(), supplied.end(), same_field))
      {
        detail::runtime_fail(ErrorCode::SchemaConflict, "message.codec", "Codec differs from the exact retained schema definitions");
      }
      return codec.decode(_payload);
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  MessageView Message::view() const & noexcept
  {
    MessageView result{_header, _payload};
    result._retention = _retention;
    return result;
  }

  MessageContext Delivery::context() const noexcept
  {
    return {_message._header, false};
  }
} // namespace angaria

namespace angaria::detail
{
  Result<MessageView> MessageAccess::bind(MessageHeader header, std::span<const std::byte> payload,
                                           SessionId session, BinaryPayloadCodec codec)
  {
    try
    {
      if (session == SessionId{} || codec.schema()._type == MessageType::Invalid || codec.schema()._version == 0)
      {
        runtime_fail(ErrorCode::InvalidState, "message.context", "Binding requires a Session and an exact resolved schema");
      }
      MessageView result{std::move(header), payload};
      result._retention = std::make_shared<const MessageRetention>(session, std::move(codec));
      check_bound_message(result._header, result._payload, result._retention);
      return result;
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }
} // namespace angaria::detail
