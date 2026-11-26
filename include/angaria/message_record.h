#pragma once

#include "angaria/message.h"
#include "angaria/payload.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace angaria
{
  // Immutable message semantics, independent of native storage offsets and
  // per-destination receipt state. The timestamp is the initial publication
  // time; destination acceptance/enqueue and dequeue times remain separate.
  struct MessageHeader
  {
    SchemaId _schema{};
    PayloadFormat _format{PayloadFormat::Binary};
    PublicationId _publication{};
    UtcTimestamp _publicationTime{};
    std::uint32_t _flags{};
    std::uint64_t _payloadBytes{};

    bool operator==(const MessageHeader&) const = default;
  };

  struct MessageRecordView
  {
    MessageHeader _header;
    std::span<const std::byte> _payload;

    MessageView message() const noexcept;
  };

  struct MessageRecord
  {
    MessageHeader _header;
    std::vector<std::byte> _payload;

    MessageRecordView view() const & noexcept;
    MessageRecordView view() const && = delete;
  };

  // Portable application/capture bytes, not a native arena RecordEnvelope or
  // Publication body. All budgets include the fixed header and payload.
  class MessageRecordCodec
  {
  public:
    static constexpr std::uint16_t encoding_version = 1;
    static constexpr std::size_t header_bytes = 80;
    using HeaderBytes = std::array<std::byte, header_bytes>;

    static Result<HeaderBytes> encode_header(const MessageHeader& header, std::uint64_t maxRecordBytes);
    static Result<MessageHeader> decode_header(std::span<const std::byte> header, std::uint64_t maxRecordBytes);
    static Result<std::vector<std::byte>> encode(const MessageHeader& header,
                                                 std::span<const std::byte> payload,
                                                 std::uint64_t maxRecordBytes);
    static OperationResult encode_into(const MessageHeader& header, std::span<const std::byte> payload,
                                         std::span<std::byte> output, std::uint64_t maxRecordBytes);
    static Result<MessageRecordView> decode_view(std::span<const std::byte> record,
                                                  std::uint64_t maxRecordBytes);
    static Result<MessageRecord> decode(std::span<const std::byte> record,
                                          std::uint64_t maxRecordBytes);
    // Retain this context from the Session supplying the schema definitions,
    // independently of the record being checked. A SchemaId alone is local to
    // a Session; neither this helper nor a caller-built snapshot authenticates it.
    static Result<std::vector<PayloadValue>> decode_binary(const MessageRecordView& record,
                                                            const BinaryPayloadCodec& codec,
                                                            SessionId schemaSession);
  };
} // namespace angaria
