#include "angaria/message_record.h"

#include "angaria/detail/byte_operations.h"
#include "angaria/detail/checked_arithmetic.h"
#include "angaria/detail/error_support.h"
#include "angaria/detail/wire_codes.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace angaria
{
  namespace
  {
    namespace wire = angaria::detail::wire;

    using angaria::detail::CodecFailure;
    using angaria::detail::codec_fail;
    using angaria::detail::codec_require;
    using angaria::detail::value_or_throw;
    using angaria::detail::read_unsigned_le;
    using angaria::detail::signed_from_bits;
    using angaria::detail::write_unsigned_le;

    constexpr std::array<std::byte, 8> record_magic{
        std::byte{0x41}, std::byte{0x4e}, std::byte{0x47}, std::byte{0x41},
        std::byte{0x4d}, std::byte{0x53}, std::byte{0x47}, std::byte{0}};

    Error record_error()
    {
      try
      {
        throw;
      }
      catch (const CodecFailure& failure)
      {
        return failure._error;
      }
      catch (const std::bad_alloc&)
      {
        return Error{ErrorCode::CapacityExceeded, "Insufficient memory for message record bytes", "message_record.memory"};
      }
      catch (const std::length_error&)
      {
        return Error{ErrorCode::CapacityExceeded, "Message record exceeds a native container limit", "message_record.memory"};
      }
      catch (...)
      {
        return Error{ErrorCode::InvalidState, "Unexpected message record codec failure", "message_record.internal"};
      }
    }

    std::uint16_t format_code(PayloadFormat format)
    {
      const auto encoded = wire::encode(format, wire::payload_formats);
      codec_require(encoded.has_value(), ErrorCode::InvalidArgument, "message_record.format", "Unknown payload format");
      return *encoded;
    }

    PayloadFormat decode_format(std::uint16_t code)
    {
      const auto decoded = wire::decode(code, wire::payload_formats);
      codec_require(decoded.has_value(), ErrorCode::InvalidArgument, "message_record.format", "Unknown encoded payload format");
      return *decoded;
    }

    std::size_t record_size(std::uint64_t payloadBytes, std::uint64_t maxRecordBytes)
    {
      constexpr auto prefix = std::uint64_t{MessageRecordCodec::header_bytes};
      const auto bytes = detail::checked_add(prefix, payloadBytes);
      if (!bytes)
      {
        codec_fail(ErrorCode::InvalidArgument, "message_record.size", "Header and payload length overflow the record size");
      }
      const auto total = *bytes;
      if (total > maxRecordBytes)
      {
        codec_fail(ErrorCode::CapacityExceeded, "message_record.capacity", "Message record exceeds its configured byte limit");
      }
      if (!detail::fits_host_size(total) ||
          total > std::vector<std::byte>{}.max_size())
      {
        codec_fail(ErrorCode::CapacityExceeded, "message_record.capacity", "Message record exceeds the native allocation range");
      }
      return static_cast<std::size_t>(total);
    }

    std::size_t validate_header(const MessageHeader& header, std::uint64_t maxRecordBytes)
    {
      if (header._queue || header._queueSequence || header._consumer || header._enqueuedAt || header._dequeuedAt)
      {
        codec_fail(ErrorCode::UnsupportedPolicy, "message_record.delivery", "Version 1 portable records cannot store destination delivery metadata");
      }
      if (header._schema._type == MessageType::Invalid)
      {
        codec_fail(ErrorCode::InvalidArgument, "message_record.schema.type", "Message type must be nonzero");
      }
      if (header._schema._version == 0)
      {
        codec_fail(ErrorCode::InvalidArgument, "message_record.schema.version", "Schema version must be nonzero");
      }
      const auto& producer = header._publication._producer;
      if (producer._session == SessionId{})
      {
        codec_fail(ErrorCode::InvalidArgument, "message_record.publication.session", "Publication Session identity must be nonzero");
      }
      if (producer._task == TaskId::Invalid)
      {
        codec_fail(ErrorCode::InvalidArgument, "message_record.publication.task", "Producer Task must be nonzero");
      }
      if (producer._incarnation == 0)
      {
        codec_fail(ErrorCode::InvalidArgument, "message_record.publication.incarnation", "Producer incarnation must be nonzero");
      }
      if (header._publication._sequence == 0)
      {
        codec_fail(ErrorCode::InvalidArgument, "message_record.publication.sequence", "Publication sequence must be nonzero");
      }
      static_cast<void>(format_code(header._format));
      return record_size(header._payloadBytes, maxRecordBytes);
    }

    void validate_payload_length(const MessageHeader& header, std::span<const std::byte> payload)
    {
      if (header._payloadBytes != payload.size())
      {
        codec_fail(ErrorCode::InvalidArgument, "message_record.payload_size", "Declared payload length must equal the supplied byte count");
      }
    }

    MessageRecordCodec::HeaderBytes encode_header_bytes(const MessageHeader& header)
    {
      MessageRecordCodec::HeaderBytes bytes{};
      std::copy(record_magic.begin(), record_magic.end(), bytes.begin());
      write_unsigned_le<std::uint16_t>(bytes, 8, MessageRecordCodec::encoding_version);
      write_unsigned_le<std::uint16_t>(bytes, 10, static_cast<std::uint16_t>(MessageRecordCodec::header_bytes));
      write_unsigned_le<std::uint16_t>(bytes, 12, format_code(header._format));
      write_unsigned_le<std::uint16_t>(bytes, 14, static_cast<std::uint16_t>(header._publication._producer._task));
      write_unsigned_le<std::uint16_t>(bytes, 16, static_cast<std::uint16_t>(header._schema._type));
      write_unsigned_le<std::uint32_t>(bytes, 20, header._schema._version);
      write_unsigned_le<std::uint32_t>(bytes, 24, header._flags);
      write_unsigned_le<std::uint64_t>(bytes, 32, header._publication._producer._session._high);
      write_unsigned_le<std::uint64_t>(bytes, 40, header._publication._producer._session._low);
      write_unsigned_le<std::uint64_t>(bytes, 48, header._publication._producer._incarnation);
      write_unsigned_le<std::uint64_t>(bytes, 56, header._publication._sequence);
      write_unsigned_le<std::uint64_t>(bytes, 64,
          static_cast<std::uint64_t>(header._publishedAt.time_since_epoch().count()));
      write_unsigned_le<std::uint64_t>(bytes, 72, header._payloadBytes);
      return bytes;
    }

    MessageHeader read_header(std::span<const std::byte> bytes, std::uint64_t maxRecordBytes)
    {
      if (bytes.size() != MessageRecordCodec::header_bytes)
      {
        codec_fail(ErrorCode::InvalidArgument, "message_record.header_size", "A message header must contain exactly 80 bytes");
      }
      if (!std::equal(record_magic.begin(), record_magic.end(), bytes.begin()))
      {
        codec_fail(ErrorCode::InvalidArgument, "message_record.magic", "Invalid ANGAMSG record magic");
      }
      if (read_unsigned_le<std::uint16_t>(bytes, 8) != MessageRecordCodec::encoding_version)
      {
        codec_fail(ErrorCode::UnsupportedPolicy, "message_record.version", "Unsupported message record encoding version");
      }
      if (read_unsigned_le<std::uint16_t>(bytes, 10) != MessageRecordCodec::header_bytes)
      {
        codec_fail(ErrorCode::InvalidArgument, "message_record.header_size", "Encoded message header length must be 80 bytes");
      }
      if (read_unsigned_le<std::uint16_t>(bytes, 18) != 0 || read_unsigned_le<std::uint32_t>(bytes, 28) != 0)
      {
        codec_fail(ErrorCode::InvalidArgument, "message_record.reserved", "Reserved header bytes must be zero");
      }
      MessageHeader header;
      header._format = decode_format(read_unsigned_le<std::uint16_t>(bytes, 12));
      header._publication._producer._task = static_cast<TaskId>(read_unsigned_le<std::uint16_t>(bytes, 14));
      header._schema._type = static_cast<MessageType>(read_unsigned_le<std::uint16_t>(bytes, 16));
      header._schema._version = read_unsigned_le<std::uint32_t>(bytes, 20);
      header._flags = read_unsigned_le<std::uint32_t>(bytes, 24);
      header._publication._producer._session._high = read_unsigned_le<std::uint64_t>(bytes, 32);
      header._publication._producer._session._low = read_unsigned_le<std::uint64_t>(bytes, 40);
      header._publication._producer._incarnation = read_unsigned_le<std::uint64_t>(bytes, 48);
      header._publication._sequence = read_unsigned_le<std::uint64_t>(bytes, 56);
      header._publishedAt = UtcTimestamp{std::chrono::nanoseconds{
          signed_from_bits<std::int64_t>(read_unsigned_le<std::uint64_t>(bytes, 64))}};
      header._payloadBytes = read_unsigned_le<std::uint64_t>(bytes, 72);
      static_cast<void>(validate_header(header, maxRecordBytes));
      return header;
    }

    MessageRecordView read_record(std::span<const std::byte> bytes, std::uint64_t maxRecordBytes)
    {
      if (bytes.size() < MessageRecordCodec::header_bytes)
      {
        codec_fail(ErrorCode::InvalidArgument, "message_record.header_size", "Truncated message record header");
      }
      const auto header = read_header(bytes.first(MessageRecordCodec::header_bytes), maxRecordBytes);
      const auto payload = bytes.subspan(MessageRecordCodec::header_bytes);
      validate_payload_length(header, payload);
      return MessageRecordView{header, payload};
    }
  } // namespace

  MessageView MessageRecordView::message() const noexcept
  {
    return MessageView{_header, _payload};
  }

  MessageRecordView MessageRecord::view() const & noexcept
  {
    return MessageRecordView{_header, _payload};
  }

  Result<MessageRecordCodec::HeaderBytes> MessageRecordCodec::encode_header(
      const MessageHeader& header, std::uint64_t maxRecordBytes)
  {
    try
    {
      static_cast<void>(validate_header(header, maxRecordBytes));
      return encode_header_bytes(header);
    }
    catch (...)
    {
      return record_error();
    }
  }

  Result<MessageHeader> MessageRecordCodec::decode_header(std::span<const std::byte> header,
                                                          std::uint64_t maxRecordBytes)
  {
    try
    {
      return read_header(header, maxRecordBytes);
    }
    catch (...)
    {
      return record_error();
    }
  }

  Result<std::vector<std::byte>> MessageRecordCodec::encode(const MessageHeader& header,
                                                            std::span<const std::byte> payload,
                                                            std::uint64_t maxRecordBytes)
  {
    try
    {
      const auto size = validate_header(header, maxRecordBytes);
      validate_payload_length(header, payload);
      const auto prefix = encode_header_bytes(header);
      std::vector<std::byte> bytes(size);
      std::copy(prefix.begin(), prefix.end(), bytes.begin());
      if (!payload.empty())
      {
        std::copy(payload.begin(), payload.end(), bytes.begin() + header_bytes);
      }
      return bytes;
    }
    catch (...)
    {
      return record_error();
    }
  }

  OperationResult MessageRecordCodec::encode_into(const MessageHeader& header,
                                                   std::span<const std::byte> payload,
                                                   std::span<std::byte> output,
                                                   std::uint64_t maxRecordBytes)
  {
    try
    {
      const auto size = validate_header(header, maxRecordBytes);
      validate_payload_length(header, payload);
      if (output.size() != size)
      {
        codec_fail(ErrorCode::InvalidArgument, "message_record.output_size", "Output must have exactly the complete record size");
      }
      // A temporary also makes overlapping input/output well-defined: no
      // caller byte changes until every header and payload byte is complete.
      const auto bytes = value_or_throw<CodecFailure>(encode(header, payload, maxRecordBytes));
      std::copy(bytes.begin(), bytes.end(), output.begin());
      return std::monostate{};
    }
    catch (...)
    {
      return record_error();
    }
  }

  Result<MessageRecordView> MessageRecordCodec::decode_view(std::span<const std::byte> record,
                                                            std::uint64_t maxRecordBytes)
  {
    try
    {
      return read_record(record, maxRecordBytes);
    }
    catch (...)
    {
      return record_error();
    }
  }

  Result<MessageRecord> MessageRecordCodec::decode(std::span<const std::byte> record,
                                                    std::uint64_t maxRecordBytes)
  {
    try
    {
      const auto borrowed = read_record(record, maxRecordBytes);
      MessageRecord owned;
      owned._header = borrowed._header;
      if (!borrowed._payload.empty())
      {
        owned._payload.assign(borrowed._payload.begin(), borrowed._payload.end());
      }
      return owned;
    }
    catch (...)
    {
      return record_error();
    }
  }

  Result<std::vector<PayloadValue>> MessageRecordCodec::decode_binary(
      const MessageRecordView& record, const BinaryPayloadCodec& codec, SessionId schemaSession)
  {
    try
    {
      static_cast<void>(validate_header(record._header, std::numeric_limits<std::uint64_t>::max()));
      validate_payload_length(record._header, record._payload);
      if (schemaSession == SessionId{})
      {
        codec_fail(ErrorCode::InvalidArgument, "message_record.schema_session", "Schema Session context must be nonzero");
      }
      if (schemaSession != record._header._publication._producer._session)
      {
        codec_fail(ErrorCode::SchemaConflict, "message_record.schema_session", "Record Session does not match the supplied schema Session context");
      }
      if (record._header._format != PayloadFormat::Binary)
      {
        codec_fail(ErrorCode::UnsupportedPolicy, "message_record.format", "Binary payload decoding requires the Binary format");
      }
      const auto expected = codec.schema();
      if (expected._type == MessageType::Invalid || expected._version == 0)
      {
        codec_fail(ErrorCode::InvalidState, "message_record.codec", "Binary payload codec has been moved from");
      }
      if (record._header._schema != expected)
      {
        codec_fail(ErrorCode::SchemaConflict, "message_record.schema", "Record schema does not match the exact payload codec schema");
      }
      return codec.decode(record._payload);
    }
    catch (...)
    {
      return record_error();
    }
  }
} // namespace angaria
