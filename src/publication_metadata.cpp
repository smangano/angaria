#include "publication_metadata.h"

#include "angaria/detail/byte_operations.h"
#include "angaria/detail/checked_arithmetic.h"
#include "angaria/detail/error_support.h"
#include "angaria/detail/wire_codes.h"

#include "angaria/detail/mapped_format.h"

#include <algorithm>
#include <bit>
#include <bitset>
#include <chrono>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace angaria::detail::mapped
{
  namespace
  {
    namespace wire = angaria::detail::wire;

    using angaria::detail::CodecFailure;
    using angaria::detail::all_zero;
    using angaria::detail::codec_require;
    using angaria::detail::read_unsigned_le;
    using angaria::detail::write_unsigned_le;

    constexpr std::uint64_t envelope_bytes = 64;

    template <typename T, typename F>
    Result<T> protect(F&& operation)
    {
      try
      {
        return operation();
      }
      catch (const CodecFailure& failure)
      {
        return failure._error;
      }
      catch (const std::bad_alloc&)
      {
        return Error{ErrorCode::CapacityExceeded, "Cannot allocate Publication metadata", "publication"};
      }
      catch (const std::length_error&)
      {
        return Error{ErrorCode::CapacityExceeded, "Publication allocation exceeds host capacity", "publication"};
      }
    }

    std::uint64_t add(std::uint64_t left, std::uint64_t right, ErrorCode code)
    {
      const auto result = checked_add(left, right);
      codec_require(result.has_value(), code, "publication.layout", "Publication byte count overflows");
      return *result;
    }

    std::uint64_t multiply(std::uint64_t left, std::uint64_t right, ErrorCode code)
    {
      const auto result = checked_multiply(left, right);
      codec_require(result.has_value(), code, "publication.layout", "Publication byte count overflows");
      return *result;
    }

    std::uint64_t align(std::uint64_t bytes, std::uint64_t alignment, ErrorCode code)
    {
      const auto result = checked_align(bytes, alignment);
      codec_require(result.has_value(), code, "publication.layout", "Publication alignment overflows");
      return *result;
    }

    std::size_t host_size(std::uint64_t bytes, ErrorCode code)
    {
      codec_require(angaria::detail::fits_host_size(bytes),
              code, "publication.layout", "Publication byte count exceeds host bounds");
      return static_cast<std::size_t>(bytes);
    }

    void check_record_offset(std::uint64_t offset, ErrorCode code)
    {
      codec_require(offset >= superblock_bytes && offset % section_alignment == 0 &&
                  contains_range(max_log_bytes, offset, envelope_bytes),
              code, "publication.offset", "Invalid Publication record offset");
    }

    PublicationLayout make_layout(std::uint64_t offset, std::uint64_t count,
                                   std::uint64_t payloadBytes, std::uint64_t ceiling,
                                   ErrorCode code)
    {
      check_record_offset(offset, code);
      codec_require(count != 0 && count <= max_entity_id, code, "publication.destinations",
              "Publication must have between 1 and 65535 original destinations");
      codec_require(ceiling != 0 && ceiling <= max_log_bytes, code, "publication.limit",
              "Invalid Publication record byte ceiling");
      const auto destinationsBytes = multiply(count, destination_row_bytes, code);
      const auto beforePayload = add(publication_fixed_bytes, destinationsBytes, code);
      const auto immutableBytes = align(add(beforePayload, payloadBytes, code), record_alignment, code);
      const auto receiptBytes = multiply(count, publication_receipt_bytes, code);
      const auto totalBytes = align(add(add(envelope_bytes, immutableBytes, code), receiptBytes, code),
                                    section_alignment, code);
      codec_require(totalBytes <= ceiling && contains_range(max_log_bytes, offset, totalBytes),
              code, "publication.limit", "Complete Publication allocation exceeds its byte bounds");
      host_size(totalBytes, code);
      const auto bodyBegin = add(offset, envelope_bytes, code);
      return PublicationLayout{add(bodyBegin, publication_fixed_bytes, code),
                               payloadBytes == 0 ? 0 : add(bodyBegin, beforePayload, code),
                               add(bodyBegin, immutableBytes, code), immutableBytes, totalBytes};
    }

    std::uint16_t encode_reason(ErrorCode reason, ErrorCode code)
    {
      const auto encoded = wire::encode(reason, wire::error_codes, 0);
      codec_require(encoded.has_value(), code, "publication.destination.reason", "Unknown destination rejection reason");
      return *encoded;
    }

    ErrorCode decode_reason(std::uint16_t reason, ErrorCode code)
    {
      const auto decoded = wire::decode(reason, wire::error_codes, 0);
      codec_require(decoded.has_value(), code, "publication.destination.reason", "Unknown stored destination rejection reason");
      return *decoded;
    }

    std::uint16_t encode_format(PayloadFormat format, ErrorCode code)
    {
      const auto encoded = wire::encode(format, wire::payload_formats);
      codec_require(encoded.has_value(), code, "publication.format", "Unknown payload format");
      return *encoded;
    }

    PayloadFormat decode_format(std::uint16_t format, ErrorCode code)
    {
      const auto decoded = wire::decode(format, wire::payload_formats);
      codec_require(decoded.has_value(), code, "publication.format", "Unknown stored payload format");
      return *decoded;
    }

    std::uint16_t encode_acceptance(AcceptancePolicy policy, ErrorCode code)
    {
      const auto encoded = wire::encode(policy, wire::acceptance_policies);
      codec_require(encoded.has_value(), code, "publication.acceptance", "Unknown acceptance policy");
      return *encoded;
    }

    AcceptancePolicy decode_acceptance(std::uint16_t policy, ErrorCode code)
    {
      const auto decoded = wire::decode(policy, wire::acceptance_policies);
      codec_require(decoded.has_value(), code, "publication.acceptance", "Unknown stored acceptance policy");
      return *decoded;
    }

    void check_destination(const DestinationDefinition& destination, std::uint64_t offset,
                             std::uint64_t receipt, ErrorCode code)
    {
      check_record_offset(offset, code);
      codec_require(receipt >= add(offset, envelope_bytes + publication_fixed_bytes + destination_row_bytes, code) &&
                  receipt % record_alignment == 0 &&
                  contains_range(max_log_bytes, receipt, publication_receipt_bytes),
              code, "publication.destination.receipt", "Invalid expected receipt reference");
      codec_require(destination._queue != QueueId::Invalid, code, "publication.destination.queue",
              "Destination Queue identity must be nonzero");
      codec_require(destination._receiptReference == receipt, code, "publication.destination.receipt",
              "Destination must reference its original preallocated receipt slot");
      encode_reason(destination._reason, code);
      if (destination._status == DestinationStatus::Accepted)
      {
        codec_require(destination._reason == ErrorCode::None && destination._queueSequence != 0,
                code, "publication.destination.acceptance", "Accepted destination needs a sequence and no rejection");
        if (destination._queueSequence == 1)
        {
          codec_require(destination._previousDelivery == 0, code, "publication.destination.previous",
                  "First Queue delivery cannot have a predecessor");
        }
        else
        {
          codec_require(destination._previousDelivery >= superblock_bytes &&
                      destination._previousDelivery % record_alignment == 0 &&
                      contains_range(offset, destination._previousDelivery, destination_row_bytes),
                  code, "publication.destination.previous", "Delivery predecessor must refer to older aligned history");
        }
      }
      else
      {
        codec_require(destination._status == DestinationStatus::Rejected, code, "publication.destination.status",
                "Unknown outcome is not a committed destination status");
        codec_require(destination._reason != ErrorCode::None && destination._reason != ErrorCode::OutcomeUnknown &&
                    destination._queueSequence == 0 && destination._previousDelivery == 0 &&
                    destination._enqueuedAt.time_since_epoch().count() == 0,
                code, "publication.destination.rejection", "Rejected destination must have a known reason and no acceptance metadata");
      }
    }

    void write_destination(std::span<std::byte> bytes, const DestinationDefinition& destination)
    {
      write_unsigned_le(bytes, 0, static_cast<std::uint16_t>(destination._queue));
      write_unsigned_le(bytes, 2, static_cast<std::uint16_t>(destination._status == DestinationStatus::Accepted ? 1 : 2));
      write_unsigned_le(bytes, 4, encode_reason(destination._reason, ErrorCode::InvalidArgument));
      write_unsigned_le(bytes, 8, destination._queueSequence);
      write_unsigned_le(bytes, 16, destination._previousDelivery);
      write_unsigned_le(bytes, 24, std::bit_cast<std::uint64_t>(destination._enqueuedAt.time_since_epoch().count()));
      write_unsigned_le(bytes, 32, destination._receiptReference);
    }

    DestinationDefinition read_destination(std::span<const std::byte> bytes, std::uint64_t offset,
                                            std::uint64_t receipt, ErrorCode code)
    {
      codec_require(bytes.size() == destination_row_bytes, code, "publication.destination.bytes",
              "Destination row must have exactly 48 bytes");
      codec_require(all_zero(bytes.subspan(6, 2)) && all_zero(bytes.subspan(40, 8)), code,
              "publication.destination.reserved", "Destination flags and reserved bytes must be zero");
      const auto status = read_unsigned_le<std::uint16_t>(bytes, 2);
      codec_require(status == 1 || status == 2, code, "publication.destination.status", "Unknown stored destination status");
      DestinationDefinition result;
      result._queue = QueueId{read_unsigned_le<std::uint16_t>(bytes, 0)};
      result._status = status == 1 ? DestinationStatus::Accepted : DestinationStatus::Rejected;
      result._reason = decode_reason(read_unsigned_le<std::uint16_t>(bytes, 4), code);
      result._queueSequence = read_unsigned_le<std::uint64_t>(bytes, 8);
      result._previousDelivery = read_unsigned_le<std::uint64_t>(bytes, 16);
      result._enqueuedAt = UtcTimestamp{std::chrono::nanoseconds{
          std::bit_cast<std::int64_t>(read_unsigned_le<std::uint64_t>(bytes, 24))}};
      result._receiptReference = read_unsigned_le<std::uint64_t>(bytes, 32);
      check_destination(result, offset, receipt, code);
      return result;
    }

    void check_header(const PublicationHeader& publication, std::uint64_t offset, ErrorCode code)
    {
      codec_require(publication._producer != TaskId::Invalid && publication._schema._type != MessageType::Invalid &&
                  publication._schema._version != 0 && publication._producerIncarnation != 0 &&
                  publication._publicationSequence != 0 && publication._topologyRevision != 0,
              code, "publication.identity", "Publication producer, schema, incarnation, sequence, and topology must be nonzero");
      codec_require(publication._schemaReference >= superblock_bytes &&
                  publication._schemaReference % section_alignment == 0 &&
                  publication._schemaReference < offset,
              code, "publication.schema", "Exact Schema reference must point to older aligned history");
      encode_format(publication._format, code);
      encode_acceptance(publication._acceptance, code);
    }

    void check_publication(const PublicationHeader& publication,
                             std::span<const DestinationDefinition> destinations,
                             const PublicationLayout& layout, std::uint64_t offset, ErrorCode code)
    {
      check_header(publication, offset, code);
      std::size_t accepted{};
      const auto check_destination_at = [&](std::size_t index)
      {
        const auto& destination = destinations[index];
        check_destination(destination, offset, layout._receiptsOffset + index * publication_receipt_bytes, code);
        return destination._status == DestinationStatus::Accepted;
      };
      if (destinations.size() == 1)
      {
        // One destination is unique without clearing the full Queue-ID bitmap.
        accepted = check_destination_at(0) ? 1 : 0;
      }
      else
      {
        std::bitset<65536> queues;
        for (std::size_t index = 0; index < destinations.size(); ++index)
        {
          const bool isAccepted = check_destination_at(index);
          const auto queue = static_cast<std::uint16_t>(destinations[index]._queue);
          codec_require(!queues[queue], code, "publication.destinations", "Original destination Queues must be unique");
          queues[queue] = true;
          accepted += isAccepted ? 1 : 0;
        }
      }
      codec_require(accepted != 0, code, "publication.destinations", "Fully rejected attempts do not create Publications");
      codec_require(publication._acceptance != AcceptancePolicy::AllDestinations || accepted == destinations.size(),
              code, "publication.acceptance", "All-destination acceptance cannot contain a rejection");
      codec_require(publication._publishingSet != PublishingQueueSetId::Invalid ||
                  (destinations.size() == 1 && publication._acceptance == AcceptancePolicy::AllDestinations),
              code, "publication.direct", "Direct send has one accepted destination and all-destination acceptance");
      codec_require(publication._publishingSet != PublishingQueueSetId::Invalid ||
                  publication._publishedAt == destinations.front()._enqueuedAt,
              code, "publication.direct", "Direct send shares its publication and enqueue timestamp sample");
    }

    PublicationPrefix read_prefix(std::span<const std::byte> bytes, std::uint64_t immutableBytes,
                                    std::uint64_t offset, std::uint64_t maxRecordBytes)
    {
      constexpr auto code = ErrorCode::IoError;
      codec_require(bytes.size() == publication_fixed_bytes, code, "publication.bytes",
              "Publication prefix must contain exactly 128 bytes");
      PublicationPrefix result;
      result._destinationCount = read_unsigned_le<std::uint32_t>(bytes, 48);
      result._payloadBytes = read_unsigned_le<std::uint64_t>(bytes, 64);
      result._layout = make_layout(offset, result._destinationCount, result._payloadBytes, maxRecordBytes, code);
      const auto& layout = result._layout;
      codec_require(immutableBytes == layout._immutableBytes, code, "publication.bytes",
              "Publication immutable byte count is not canonical");
      codec_require(all_zero(bytes.subspan(52, 4)) && all_zero(bytes.subspan(114, 14)), code,
              "publication.reserved", "Publication reserved bytes must be zero");
      codec_require(read_unsigned_le<std::uint64_t>(bytes, 56) == layout._payloadOffset &&
                  read_unsigned_le<std::uint64_t>(bytes, 80) == layout._destinationsOffset &&
                  read_unsigned_le<std::uint64_t>(bytes, 88) == result._destinationCount &&
                  read_unsigned_le<std::uint64_t>(bytes, 96) == layout._receiptsOffset &&
                  read_unsigned_le<std::uint64_t>(bytes, 104) == result._destinationCount,
              code, "publication.slices", "Publication slices must match its canonical layout and destination count");
      auto& header = result._header;
      header._producer = TaskId{read_unsigned_le<std::uint16_t>(bytes, 0)};
      header._publishingSet = PublishingQueueSetId{read_unsigned_le<std::uint16_t>(bytes, 2)};
      header._schema = SchemaId{MessageType{read_unsigned_le<std::uint16_t>(bytes, 4)}, read_unsigned_le<std::uint32_t>(bytes, 8)};
      header._format = decode_format(read_unsigned_le<std::uint16_t>(bytes, 6), code);
      header._flags = read_unsigned_le<std::uint32_t>(bytes, 12);
      header._producerIncarnation = read_unsigned_le<std::uint64_t>(bytes, 16);
      header._publicationSequence = read_unsigned_le<std::uint64_t>(bytes, 24);
      header._publishedAt = UtcTimestamp{std::chrono::nanoseconds{
          std::bit_cast<std::int64_t>(read_unsigned_le<std::uint64_t>(bytes, 32))}};
      header._topologyRevision = read_unsigned_le<std::uint64_t>(bytes, 40);
      header._schemaReference = read_unsigned_le<std::uint64_t>(bytes, 72);
      header._acceptance = decode_acceptance(read_unsigned_le<std::uint16_t>(bytes, 112), code);
      check_header(header, offset, code);
      codec_require(header._publishingSet != PublishingQueueSetId::Invalid ||
                  (result._destinationCount == 1 && header._acceptance == AcceptancePolicy::AllDestinations),
              code, "publication.direct", "Direct send has one accepted destination and all-destination acceptance");
      return result;
    }

    PublicationMetadata read_metadata(const PublicationPrefix& prefix,
                                        std::span<const std::byte> rows,
                                        std::span<const std::byte> padding,
                                        std::uint64_t offset)
    {
      constexpr auto code = ErrorCode::IoError;
      const auto count = prefix._destinationCount;
      const auto rowsBytes = std::size_t{count} * destination_row_bytes;
      codec_require(rows.size() == rowsBytes, code, "publication.destinations",
              "Publication destination bytes disagree with the checked count");
      const auto bodyBytes = publication_fixed_bytes + rowsBytes + prefix._payloadBytes;
      codec_require(padding.size() == prefix._layout._immutableBytes - bodyBytes && all_zero(padding),
              code, "publication.padding", "Publication alignment padding must have its exact zero-byte encoding");
      PublicationMetadata result;
      static_cast<PublicationHeader&>(result) = prefix._header;
      result._layout = prefix._layout;
      result._payloadBytes = prefix._payloadBytes;
      result._destinations.reserve(count);
      for (std::size_t index = 0; index < count; ++index)
      {
        result._destinations.push_back(read_destination(rows.subspan(index * destination_row_bytes, destination_row_bytes),
                                                         offset, prefix._layout._receiptsOffset + index * publication_receipt_bytes,
                                                         code));
      }
      check_publication(result, result._destinations, result._layout, offset, code);
      return result;
    }
  }

  Result<PublicationLayout> plan_publication_layout(std::uint64_t recordOffset,
                                                    std::uint64_t destinationCount,
                                                    std::uint64_t payloadBytes,
                                                    std::uint64_t maxRecordBytes)
  {
    return protect<PublicationLayout>([&]
    {
      return make_layout(recordOffset, destinationCount, payloadBytes, maxRecordBytes, ErrorCode::InvalidArgument);
    });
  }

  Result<DestinationRowBytes> encode_destination_row(const DestinationDefinition& destination,
                                                     std::uint64_t publicationOffset,
                                                     std::uint64_t expectedReceiptReference)
  {
    return protect<DestinationRowBytes>([&]
    {
      check_destination(destination, publicationOffset, expectedReceiptReference, ErrorCode::InvalidArgument);
      DestinationRowBytes result{};
      write_destination(result, destination);
      return result;
    });
  }

  Result<DestinationDefinition> decode_destination_row(std::span<const std::byte> bytes,
                                                       std::uint64_t publicationOffset,
                                                       std::uint64_t expectedReceiptReference)
  {
    return protect<DestinationDefinition>([&]
    {
      return read_destination(bytes, publicationOffset, expectedReceiptReference, ErrorCode::IoError);
    });
  }

  Result<std::vector<std::byte>> encode_publication_body(const PublicationDefinition& publication,
                                                         std::uint64_t recordOffset,
                                                         std::uint64_t maxRecordBytes)
  {
    return protect<std::vector<std::byte>>([&]
    {
      const auto layout = make_layout(recordOffset, publication._destinations.size(), publication._payload.size(),
                                      maxRecordBytes, ErrorCode::InvalidArgument);
      check_publication(publication, publication._destinations, layout, recordOffset, ErrorCode::InvalidArgument);
      std::vector<std::byte> bytes(host_size(layout._immutableBytes, ErrorCode::InvalidArgument));
      write_unsigned_le(bytes, 0, static_cast<std::uint16_t>(publication._producer));
      write_unsigned_le(bytes, 2, static_cast<std::uint16_t>(publication._publishingSet));
      write_unsigned_le(bytes, 4, static_cast<std::uint16_t>(publication._schema._type));
      write_unsigned_le(bytes, 6, encode_format(publication._format, ErrorCode::InvalidArgument));
      write_unsigned_le(bytes, 8, publication._schema._version);
      write_unsigned_le(bytes, 12, publication._flags);
      write_unsigned_le(bytes, 16, publication._producerIncarnation);
      write_unsigned_le(bytes, 24, publication._publicationSequence);
      write_unsigned_le(bytes, 32, std::bit_cast<std::uint64_t>(publication._publishedAt.time_since_epoch().count()));
      write_unsigned_le(bytes, 40, publication._topologyRevision);
      write_unsigned_le(bytes, 48, static_cast<std::uint32_t>(publication._destinations.size()));
      write_unsigned_le(bytes, 56, layout._payloadOffset);
      write_unsigned_le(bytes, 64, static_cast<std::uint64_t>(publication._payload.size()));
      write_unsigned_le(bytes, 72, publication._schemaReference);
      write_unsigned_le(bytes, 80, layout._destinationsOffset);
      write_unsigned_le(bytes, 88, static_cast<std::uint64_t>(publication._destinations.size()));
      write_unsigned_le(bytes, 96, layout._receiptsOffset);
      write_unsigned_le(bytes, 104, static_cast<std::uint64_t>(publication._destinations.size()));
      write_unsigned_le(bytes, 112, encode_acceptance(publication._acceptance, ErrorCode::InvalidArgument));
      for (std::size_t index = 0; index < publication._destinations.size(); ++index)
      {
        write_destination(std::span<std::byte>{bytes}.subspan(publication_fixed_bytes + index * destination_row_bytes,
                                                             destination_row_bytes), publication._destinations[index]);
      }
      std::copy(publication._payload.begin(), publication._payload.end(),
                bytes.begin() + publication_fixed_bytes + publication._destinations.size() * destination_row_bytes);
      return bytes;
    });
  }

  Result<PublicationPrefix> decode_publication_prefix(std::span<const std::byte> fixedBody,
                                                      std::uint64_t immutableBytes,
                                                      std::uint64_t recordOffset,
                                                      std::uint64_t maxRecordBytes)
  {
    return protect<PublicationPrefix>([&]
    {
      return read_prefix(fixedBody, immutableBytes, recordOffset, maxRecordBytes);
    });
  }

  Result<PublicationMetadata> decode_publication_metadata(std::span<const std::byte> fixedBody,
                                                          std::span<const std::byte> destinationRows,
                                                          std::span<const std::byte> immutablePadding,
                                                          std::uint64_t immutableBytes,
                                                          std::uint64_t recordOffset,
                                                          std::uint64_t maxRecordBytes)
  {
    return protect<PublicationMetadata>([&]
    {
      const auto prefix = read_prefix(fixedBody, immutableBytes, recordOffset, maxRecordBytes);
      return read_metadata(prefix, destinationRows, immutablePadding, recordOffset);
    });
  }

  Result<PublicationDefinition> decode_publication_body(std::span<const std::byte> bytes,
                                                        std::uint64_t recordOffset,
                                                        std::uint64_t maxRecordBytes)
  {
    return protect<PublicationDefinition>([&]
    {
      constexpr auto code = ErrorCode::IoError;
      codec_require(bytes.size() >= publication_fixed_bytes, code, "publication.bytes", "Truncated Publication fixed body");
      const auto prefix = read_prefix(bytes.first(publication_fixed_bytes), bytes.size(), recordOffset, maxRecordBytes);
      const auto rowsBytes = std::size_t{prefix._destinationCount} * destination_row_bytes;
      const auto payloadBegin = publication_fixed_bytes + rowsBytes;
      const auto payloadEnd = payloadBegin + host_size(prefix._payloadBytes, code);
      auto metadata = read_metadata(prefix, bytes.subspan(publication_fixed_bytes, rowsBytes),
                                     bytes.subspan(payloadEnd), recordOffset);
      PublicationDefinition result;
      static_cast<PublicationHeader&>(result) = metadata;
      result._destinations = std::move(metadata._destinations);
      result._payload.assign(bytes.begin() + payloadBegin, bytes.begin() + payloadEnd);
      return result;
    });
  }
} // namespace angaria::detail::mapped
