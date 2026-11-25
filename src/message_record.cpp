#include "angaria/message_record.h"

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
    constexpr std::array<std::byte, 8> record_magic{
        std::byte{0x41}, std::byte{0x4e}, std::byte{0x47}, std::byte{0x41},
        std::byte{0x4d}, std::byte{0x53}, std::byte{0x47}, std::byte{0}};

    struct RecordFailure
    {
      Error _error;
    };

    [[noreturn]] void fail(ErrorCode code, const char* path, const char* message)
    {
      throw RecordFailure{Error{code, message, path}};
    }

    Error record_error()
    {
      try
      {
        throw;
      }
      catch (const RecordFailure& failure)
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

    template <typename T>
    T take(Result<T> result)
    {
      if (auto* error = std::get_if<Error>(&result))
      {
        throw RecordFailure{std::move(*error)};
      }
      return std::get<T>(std::move(result));
    }

    // Each caller establishes the fixed header's bounds first. Byte operations
    // avoid native alignment, object padding, and host byte-order assumptions.
    template <typename T>
    T read_integer(std::span<const std::byte> bytes, std::size_t offset)
    {
      static_assert(std::is_unsigned_v<T> && sizeof(T) <= sizeof(std::uint64_t));
      std::uint64_t value{};
      for (std::size_t index = 0; index < sizeof(T); ++index)
      {
        value |= std::uint64_t{std::to_integer<std::uint8_t>(bytes[offset + index])} << (8 * index);
      }
      return static_cast<T>(value);
    }

    template <typename T>
    void write_integer(std::span<std::byte> bytes, std::size_t offset, T value)
    {
      static_assert(std::is_unsigned_v<T> && sizeof(T) <= sizeof(std::uint64_t));
      for (std::size_t index = 0; index < sizeof(T); ++index)
      {
        bytes[offset + index] = static_cast<std::byte>((std::uint64_t{value} >> (8 * index)) & 0xff);
      }
    }

    std::int64_t signed_integer(std::uint64_t bits)
    {
      if (bits <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      {
        return static_cast<std::int64_t>(bits);
      }
      return -1 - static_cast<std::int64_t>(~bits);
    }

    std::uint16_t format_code(PayloadFormat format)
    {
      switch (format)
      {
      case PayloadFormat::Binary: return 1;
      case PayloadFormat::Json: return 2;
      case PayloadFormat::Xml: return 3;
      case PayloadFormat::Csv: return 4;
      case PayloadFormat::Text: return 5;
      }
      fail(ErrorCode::InvalidArgument, "message_record.format", "Unknown payload format");
    }

    PayloadFormat decode_format(std::uint16_t code)
    {
      switch (code)
      {
      case 1: return PayloadFormat::Binary;
      case 2: return PayloadFormat::Json;
      case 3: return PayloadFormat::Xml;
      case 4: return PayloadFormat::Csv;
      case 5: return PayloadFormat::Text;
      }
      fail(ErrorCode::InvalidArgument, "message_record.format", "Unknown encoded payload format");
    }

    std::size_t record_size(std::uint64_t payloadBytes, std::uint64_t maxRecordBytes)
    {
      constexpr auto prefix = std::uint64_t{MessageRecordCodec::header_bytes};
      if (payloadBytes > std::numeric_limits<std::uint64_t>::max() - prefix)
      {
        fail(ErrorCode::InvalidArgument, "message_record.size", "Header and payload length overflow the record size");
      }
      const auto total = prefix + payloadBytes;
      if (total > maxRecordBytes)
      {
        fail(ErrorCode::CapacityExceeded, "message_record.capacity", "Message record exceeds its configured byte limit");
      }
      if (total > std::numeric_limits<std::size_t>::max() ||
          total > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()) ||
          total > std::vector<std::byte>{}.max_size())
      {
        fail(ErrorCode::CapacityExceeded, "message_record.capacity", "Message record exceeds the native allocation range");
      }
      return static_cast<std::size_t>(total);
    }

    std::size_t validate_header(const MessageHeader& header, std::uint64_t maxRecordBytes)
    {
      if (header._schema._type == MessageType::Invalid)
      {
        fail(ErrorCode::InvalidArgument, "message_record.schema.type", "Message type must be nonzero");
      }
      if (header._schema._version == 0)
      {
        fail(ErrorCode::InvalidArgument, "message_record.schema.version", "Schema version must be nonzero");
      }
      const auto& producer = header._publication._producer;
      if (producer._session == SessionId{})
      {
        fail(ErrorCode::InvalidArgument, "message_record.publication.session", "Publication Session identity must be nonzero");
      }
      if (producer._task == TaskId::Invalid)
      {
        fail(ErrorCode::InvalidArgument, "message_record.publication.task", "Producer Task must be nonzero");
      }
      if (producer._incarnation == 0)
      {
        fail(ErrorCode::InvalidArgument, "message_record.publication.incarnation", "Producer incarnation must be nonzero");
      }
      if (header._publication._sequence == 0)
      {
        fail(ErrorCode::InvalidArgument, "message_record.publication.sequence", "Publication sequence must be nonzero");
      }
      static_cast<void>(format_code(header._format));
      return record_size(header._payloadBytes, maxRecordBytes);
    }

    void validate_payload_length(const MessageHeader& header, std::span<const std::byte> payload)
    {
      if (header._payloadBytes != payload.size())
      {
        fail(ErrorCode::InvalidArgument, "message_record.payload_size", "Declared payload length must equal the supplied byte count");
      }
    }

    MessageRecordCodec::HeaderBytes encode_header_bytes(const MessageHeader& header)
    {
      MessageRecordCodec::HeaderBytes bytes{};
      std::copy(record_magic.begin(), record_magic.end(), bytes.begin());
      write_integer<std::uint16_t>(bytes, 8, MessageRecordCodec::encoding_version);
      write_integer<std::uint16_t>(bytes, 10, static_cast<std::uint16_t>(MessageRecordCodec::header_bytes));
      write_integer<std::uint16_t>(bytes, 12, format_code(header._format));
      write_integer<std::uint16_t>(bytes, 14, static_cast<std::uint16_t>(header._publication._producer._task));
      write_integer<std::uint16_t>(bytes, 16, static_cast<std::uint16_t>(header._schema._type));
      write_integer<std::uint32_t>(bytes, 20, header._schema._version);
      write_integer<std::uint32_t>(bytes, 24, header._flags);
      write_integer<std::uint64_t>(bytes, 32, header._publication._producer._session._high);
      write_integer<std::uint64_t>(bytes, 40, header._publication._producer._session._low);
      write_integer<std::uint64_t>(bytes, 48, header._publication._producer._incarnation);
      write_integer<std::uint64_t>(bytes, 56, header._publication._sequence);
      write_integer<std::uint64_t>(bytes, 64,
          static_cast<std::uint64_t>(header._publicationTime.time_since_epoch().count()));
      write_integer<std::uint64_t>(bytes, 72, header._payloadBytes);
      return bytes;
    }

    MessageHeader read_header(std::span<const std::byte> bytes, std::uint64_t maxRecordBytes)
    {
      if (bytes.size() != MessageRecordCodec::header_bytes)
      {
        fail(ErrorCode::InvalidArgument, "message_record.header_size", "A message header must contain exactly 80 bytes");
      }
      if (!std::equal(record_magic.begin(), record_magic.end(), bytes.begin()))
      {
        fail(ErrorCode::InvalidArgument, "message_record.magic", "Invalid ANGAMSG record magic");
      }
      if (read_integer<std::uint16_t>(bytes, 8) != MessageRecordCodec::encoding_version)
      {
        fail(ErrorCode::UnsupportedPolicy, "message_record.version", "Unsupported message record encoding version");
      }
      if (read_integer<std::uint16_t>(bytes, 10) != MessageRecordCodec::header_bytes)
      {
        fail(ErrorCode::InvalidArgument, "message_record.header_size", "Encoded message header length must be 80 bytes");
      }
      if (read_integer<std::uint16_t>(bytes, 18) != 0 || read_integer<std::uint32_t>(bytes, 28) != 0)
      {
        fail(ErrorCode::InvalidArgument, "message_record.reserved", "Reserved header bytes must be zero");
      }
      MessageHeader header;
      header._format = decode_format(read_integer<std::uint16_t>(bytes, 12));
      header._publication._producer._task = static_cast<TaskId>(read_integer<std::uint16_t>(bytes, 14));
      header._schema._type = static_cast<MessageType>(read_integer<std::uint16_t>(bytes, 16));
      header._schema._version = read_integer<std::uint32_t>(bytes, 20);
      header._flags = read_integer<std::uint32_t>(bytes, 24);
      header._publication._producer._session._high = read_integer<std::uint64_t>(bytes, 32);
      header._publication._producer._session._low = read_integer<std::uint64_t>(bytes, 40);
      header._publication._producer._incarnation = read_integer<std::uint64_t>(bytes, 48);
      header._publication._sequence = read_integer<std::uint64_t>(bytes, 56);
      header._publicationTime = UtcTimestamp{std::chrono::nanoseconds{
          signed_integer(read_integer<std::uint64_t>(bytes, 64))}};
      header._payloadBytes = read_integer<std::uint64_t>(bytes, 72);
      static_cast<void>(validate_header(header, maxRecordBytes));
      return header;
    }

    MessageRecordView read_record(std::span<const std::byte> bytes, std::uint64_t maxRecordBytes)
    {
      if (bytes.size() < MessageRecordCodec::header_bytes)
      {
        fail(ErrorCode::InvalidArgument, "message_record.header_size", "Truncated message record header");
      }
      const auto header = read_header(bytes.first(MessageRecordCodec::header_bytes), maxRecordBytes);
      const auto payload = bytes.subspan(MessageRecordCodec::header_bytes);
      validate_payload_length(header, payload);
      return MessageRecordView{header, payload};
    }
  } // namespace

  MessageView MessageRecordView::message() const noexcept
  {
    return MessageView{_header._schema, _header._format, _payload, _header._flags};
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
        fail(ErrorCode::InvalidArgument, "message_record.output_size", "Output must have exactly the complete record size");
      }
      // A temporary also makes overlapping input/output well-defined: no
      // caller byte changes until every header and payload byte is complete.
      const auto bytes = take(encode(header, payload, maxRecordBytes));
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
        fail(ErrorCode::InvalidArgument, "message_record.schema_session", "Schema Session context must be nonzero");
      }
      if (schemaSession != record._header._publication._producer._session)
      {
        fail(ErrorCode::SchemaConflict, "message_record.schema_session", "Record Session does not match the supplied schema Session context");
      }
      if (record._header._format != PayloadFormat::Binary)
      {
        fail(ErrorCode::UnsupportedPolicy, "message_record.format", "Binary payload decoding requires the Binary format");
      }
      const auto expected = codec.schema();
      if (expected._type == MessageType::Invalid || expected._version == 0)
      {
        fail(ErrorCode::InvalidState, "message_record.codec", "Binary payload codec has been moved from");
      }
      if (record._header._schema != expected)
      {
        fail(ErrorCode::SchemaConflict, "message_record.schema", "Record schema does not match the exact payload codec schema");
      }
      return codec.decode(record._payload);
    }
    catch (...)
    {
      return record_error();
    }
  }
} // namespace angaria
