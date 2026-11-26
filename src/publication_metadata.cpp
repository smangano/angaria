#include "publication_metadata.h"

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
    constexpr std::uint64_t envelope_bytes = 64;

    struct CodecFailure
    {
      Error _error;
    };

    void require(bool condition, ErrorCode code, std::string_view path, std::string_view message)
    {
      if (!condition)
      {
        throw CodecFailure{Error{code, std::string{message}, std::string{path}}};
      }
    }

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

    template <typename T>
    T read_integer(std::span<const std::byte> bytes, std::size_t offset)
    {
      static_assert(std::is_unsigned_v<T> && sizeof(T) <= 8);
      std::uint64_t result{};
      for (std::size_t byte = 0; byte < sizeof(T); ++byte)
      {
        result |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + byte])} << (8 * byte);
      }
      return static_cast<T>(result);
    }

    template <typename T>
    void write_integer(std::span<std::byte> bytes, std::size_t offset, T value)
    {
      static_assert(std::is_unsigned_v<T> && sizeof(T) <= 8);
      for (std::size_t byte = 0; byte < sizeof(T); ++byte)
      {
        bytes[offset + byte] = static_cast<std::byte>((std::uint64_t{value} >> (8 * byte)) & 0xff);
      }
    }

    bool zero(std::span<const std::byte> bytes)
    {
      return std::all_of(bytes.begin(), bytes.end(), [](std::byte byte)
      {
        return byte == std::byte{};
      });
    }

    std::uint64_t add(std::uint64_t left, std::uint64_t right, ErrorCode code)
    {
      const auto result = checked_add(left, right);
      require(result.has_value(), code, "publication.layout", "Publication byte count overflows");
      return *result;
    }

    std::uint64_t multiply(std::uint64_t left, std::uint64_t right, ErrorCode code)
    {
      const auto result = checked_multiply(left, right);
      require(result.has_value(), code, "publication.layout", "Publication byte count overflows");
      return *result;
    }

    std::uint64_t align(std::uint64_t bytes, std::uint64_t alignment, ErrorCode code)
    {
      const auto result = checked_align(bytes, alignment);
      require(result.has_value(), code, "publication.layout", "Publication alignment overflows");
      return *result;
    }

    std::size_t host_size(std::uint64_t bytes, ErrorCode code)
    {
      require(bytes <= std::numeric_limits<std::size_t>::max() &&
                  bytes <= static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()),
              code, "publication.layout", "Publication byte count exceeds host bounds");
      return static_cast<std::size_t>(bytes);
    }

    void check_record_offset(std::uint64_t offset, ErrorCode code)
    {
      require(offset >= superblock_bytes && offset % section_alignment == 0 &&
                  contains_range(max_log_bytes, offset, envelope_bytes),
              code, "publication.offset", "Invalid Publication record offset");
    }

    PublicationLayout make_layout(std::uint64_t offset, std::uint64_t count,
                                   std::uint64_t payloadBytes, std::uint64_t ceiling,
                                   ErrorCode code)
    {
      check_record_offset(offset, code);
      require(count != 0 && count <= max_entity_id, code, "publication.destinations",
              "Publication must have between 1 and 65535 original destinations");
      require(ceiling != 0 && ceiling <= max_log_bytes, code, "publication.limit",
              "Invalid Publication record byte ceiling");
      const auto destinationsBytes = multiply(count, destination_row_bytes, code);
      const auto beforePayload = add(publication_fixed_bytes, destinationsBytes, code);
      const auto immutableBytes = align(add(beforePayload, payloadBytes, code), record_alignment, code);
      const auto receiptBytes = multiply(count, publication_receipt_bytes, code);
      const auto totalBytes = align(add(add(envelope_bytes, immutableBytes, code), receiptBytes, code),
                                    section_alignment, code);
      require(totalBytes <= ceiling && contains_range(max_log_bytes, offset, totalBytes),
              code, "publication.limit", "Complete Publication allocation exceeds its byte bounds");
      host_size(totalBytes, code);
      const auto bodyBegin = add(offset, envelope_bytes, code);
      return PublicationLayout{add(bodyBegin, publication_fixed_bytes, code),
                               payloadBytes == 0 ? 0 : add(bodyBegin, beforePayload, code),
                               add(bodyBegin, immutableBytes, code), immutableBytes, totalBytes};
    }

    constexpr std::array reason_codes{
        ErrorCode::None, ErrorCode::InvalidArgument, ErrorCode::InvalidState,
        ErrorCode::InvalidConfiguration, ErrorCode::ConfigurationMismatch, ErrorCode::NotFound,
        ErrorCode::Conflict, ErrorCode::CapacityExceeded, ErrorCode::OwnershipBusy,
        ErrorCode::WrongOwner, ErrorCode::ReadOnlyRegistry, ErrorCode::NotActive,
        ErrorCode::UnsupportedPolicy, ErrorCode::SchemaConflict, ErrorCode::LogFull,
        ErrorCode::StaleDelivery, ErrorCode::OutputBufferTooSmall, ErrorCode::HistoryUnavailable,
        ErrorCode::IoError, ErrorCode::OutcomeUnknown};

    std::uint16_t encode_reason(ErrorCode reason, ErrorCode code)
    {
      const auto position = std::find(reason_codes.begin(), reason_codes.end(), reason);
      require(position != reason_codes.end(), code, "publication.destination.reason",
              "Unknown destination rejection reason");
      return static_cast<std::uint16_t>(position - reason_codes.begin());
    }

    ErrorCode decode_reason(std::uint16_t reason, ErrorCode code)
    {
      require(reason < reason_codes.size(), code, "publication.destination.reason",
              "Unknown stored destination rejection reason");
      return reason_codes[reason];
    }

    constexpr std::array format_codes{
        PayloadFormat::Binary, PayloadFormat::Json, PayloadFormat::Xml,
        PayloadFormat::Csv, PayloadFormat::Text};

    std::uint16_t encode_format(PayloadFormat format, ErrorCode code)
    {
      const auto position = std::find(format_codes.begin(), format_codes.end(), format);
      require(position != format_codes.end(), code, "publication.format", "Unknown payload format");
      return static_cast<std::uint16_t>(1 + position - format_codes.begin());
    }

    PayloadFormat decode_format(std::uint16_t format, ErrorCode code)
    {
      require(format >= 1 && format <= format_codes.size(), code, "publication.format",
              "Unknown stored payload format");
      return format_codes[format - 1];
    }

    std::uint16_t encode_acceptance(AcceptancePolicy policy, ErrorCode code)
    {
      switch (policy)
      {
      case AcceptancePolicy::AllDestinations: return 1;
      case AcceptancePolicy::Partial: return 2;
      }
      require(false, code, "publication.acceptance", "Unknown acceptance policy");
      return 0;
    }

    AcceptancePolicy decode_acceptance(std::uint16_t policy, ErrorCode code)
    {
      require(policy == 1 || policy == 2, code, "publication.acceptance",
              "Unknown stored acceptance policy");
      return policy == 1 ? AcceptancePolicy::AllDestinations : AcceptancePolicy::Partial;
    }

    void check_destination(const DestinationDefinition& destination, std::uint64_t offset,
                             std::uint64_t receipt, ErrorCode code)
    {
      check_record_offset(offset, code);
      require(receipt >= add(offset, envelope_bytes + publication_fixed_bytes + destination_row_bytes, code) &&
                  receipt % record_alignment == 0 &&
                  contains_range(max_log_bytes, receipt, publication_receipt_bytes),
              code, "publication.destination.receipt", "Invalid expected receipt reference");
      require(destination._queue != QueueId::Invalid, code, "publication.destination.queue",
              "Destination Queue identity must be nonzero");
      require(destination._receiptReference == receipt, code, "publication.destination.receipt",
              "Destination must reference its original preallocated receipt slot");
      encode_reason(destination._reason, code);
      if (destination._status == DestinationStatus::Accepted)
      {
        require(destination._reason == ErrorCode::None && destination._queueSequence != 0,
                code, "publication.destination.acceptance", "Accepted destination needs a sequence and no rejection");
        if (destination._queueSequence == 1)
        {
          require(destination._previousDelivery == 0, code, "publication.destination.previous",
                  "First Queue delivery cannot have a predecessor");
        }
        else
        {
          require(destination._previousDelivery >= superblock_bytes &&
                      destination._previousDelivery % record_alignment == 0 &&
                      contains_range(offset, destination._previousDelivery, destination_row_bytes),
                  code, "publication.destination.previous", "Delivery predecessor must refer to older aligned history");
        }
      }
      else
      {
        require(destination._status == DestinationStatus::Rejected, code, "publication.destination.status",
                "Unknown outcome is not a committed destination status");
        require(destination._reason != ErrorCode::None && destination._reason != ErrorCode::OutcomeUnknown &&
                    destination._queueSequence == 0 && destination._previousDelivery == 0 &&
                    destination._enqueuedAt.time_since_epoch().count() == 0,
                code, "publication.destination.rejection", "Rejected destination must have a known reason and no acceptance metadata");
      }
    }

    void write_destination(std::span<std::byte> bytes, const DestinationDefinition& destination)
    {
      write_integer(bytes, 0, static_cast<std::uint16_t>(destination._queue));
      write_integer(bytes, 2, static_cast<std::uint16_t>(destination._status == DestinationStatus::Accepted ? 1 : 2));
      write_integer(bytes, 4, encode_reason(destination._reason, ErrorCode::InvalidArgument));
      write_integer(bytes, 8, destination._queueSequence);
      write_integer(bytes, 16, destination._previousDelivery);
      write_integer(bytes, 24, std::bit_cast<std::uint64_t>(destination._enqueuedAt.time_since_epoch().count()));
      write_integer(bytes, 32, destination._receiptReference);
    }

    DestinationDefinition read_destination(std::span<const std::byte> bytes, std::uint64_t offset,
                                            std::uint64_t receipt, ErrorCode code)
    {
      require(bytes.size() == destination_row_bytes, code, "publication.destination.bytes",
              "Destination row must have exactly 48 bytes");
      require(zero(bytes.subspan(6, 2)) && zero(bytes.subspan(40, 8)), code,
              "publication.destination.reserved", "Destination flags and reserved bytes must be zero");
      const auto status = read_integer<std::uint16_t>(bytes, 2);
      require(status == 1 || status == 2, code, "publication.destination.status", "Unknown stored destination status");
      DestinationDefinition result;
      result._queue = QueueId{read_integer<std::uint16_t>(bytes, 0)};
      result._status = status == 1 ? DestinationStatus::Accepted : DestinationStatus::Rejected;
      result._reason = decode_reason(read_integer<std::uint16_t>(bytes, 4), code);
      result._queueSequence = read_integer<std::uint64_t>(bytes, 8);
      result._previousDelivery = read_integer<std::uint64_t>(bytes, 16);
      result._enqueuedAt = UtcTimestamp{std::chrono::nanoseconds{
          std::bit_cast<std::int64_t>(read_integer<std::uint64_t>(bytes, 24))}};
      result._receiptReference = read_integer<std::uint64_t>(bytes, 32);
      check_destination(result, offset, receipt, code);
      return result;
    }

    void check_publication(const PublicationDefinition& publication, const PublicationLayout& layout,
                             std::uint64_t offset, ErrorCode code)
    {
      require(publication._producer != TaskId::Invalid && publication._schema._type != MessageType::Invalid &&
                  publication._schema._version != 0 && publication._producerIncarnation != 0 &&
                  publication._publicationSequence != 0 && publication._topologyRevision != 0,
              code, "publication.identity", "Publication producer, schema, incarnation, sequence, and topology must be nonzero");
      require(publication._schemaReference >= superblock_bytes &&
                  publication._schemaReference % section_alignment == 0 &&
                  publication._schemaReference < offset,
              code, "publication.schema", "Exact Schema reference must point to older aligned history");
      encode_format(publication._format, code);
      encode_acceptance(publication._acceptance, code);
      std::bitset<65536> queues;
      std::size_t accepted{};
      for (std::size_t index = 0; index < publication._destinations.size(); ++index)
      {
        const auto& destination = publication._destinations[index];
        check_destination(destination, offset, layout._receiptsOffset + index * publication_receipt_bytes, code);
        const auto queue = static_cast<std::uint16_t>(destination._queue);
        require(!queues[queue], code, "publication.destinations", "Original destination Queues must be unique");
        queues[queue] = true;
        accepted += destination._status == DestinationStatus::Accepted ? 1 : 0;
      }
      require(accepted != 0, code, "publication.destinations", "Fully rejected attempts do not create Publications");
      require(publication._acceptance != AcceptancePolicy::AllDestinations || accepted == publication._destinations.size(),
              code, "publication.acceptance", "All-destination acceptance cannot contain a rejection");
      require(publication._publishingSet != PublishingQueueSetId::Invalid ||
                  (publication._destinations.size() == 1 && publication._acceptance == AcceptancePolicy::AllDestinations),
              code, "publication.direct", "Direct send has one accepted destination and all-destination acceptance");
      require(publication._publishingSet != PublishingQueueSetId::Invalid ||
                  publication._publishedAt == publication._destinations.front()._enqueuedAt,
              code, "publication.direct", "Direct send shares its publication and enqueue timestamp sample");
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
      check_publication(publication, layout, recordOffset, ErrorCode::InvalidArgument);
      std::vector<std::byte> bytes(host_size(layout._immutableBytes, ErrorCode::InvalidArgument));
      write_integer(bytes, 0, static_cast<std::uint16_t>(publication._producer));
      write_integer(bytes, 2, static_cast<std::uint16_t>(publication._publishingSet));
      write_integer(bytes, 4, static_cast<std::uint16_t>(publication._schema._type));
      write_integer(bytes, 6, encode_format(publication._format, ErrorCode::InvalidArgument));
      write_integer(bytes, 8, publication._schema._version);
      write_integer(bytes, 12, publication._flags);
      write_integer(bytes, 16, publication._producerIncarnation);
      write_integer(bytes, 24, publication._publicationSequence);
      write_integer(bytes, 32, std::bit_cast<std::uint64_t>(publication._publishedAt.time_since_epoch().count()));
      write_integer(bytes, 40, publication._topologyRevision);
      write_integer(bytes, 48, static_cast<std::uint32_t>(publication._destinations.size()));
      write_integer(bytes, 56, layout._payloadOffset);
      write_integer(bytes, 64, static_cast<std::uint64_t>(publication._payload.size()));
      write_integer(bytes, 72, publication._schemaReference);
      write_integer(bytes, 80, layout._destinationsOffset);
      write_integer(bytes, 88, static_cast<std::uint64_t>(publication._destinations.size()));
      write_integer(bytes, 96, layout._receiptsOffset);
      write_integer(bytes, 104, static_cast<std::uint64_t>(publication._destinations.size()));
      write_integer(bytes, 112, encode_acceptance(publication._acceptance, ErrorCode::InvalidArgument));
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

  Result<PublicationDefinition> decode_publication_body(std::span<const std::byte> bytes,
                                                        std::uint64_t recordOffset,
                                                        std::uint64_t maxRecordBytes)
  {
    return protect<PublicationDefinition>([&]
    {
      constexpr auto code = ErrorCode::IoError;
      require(bytes.size() >= publication_fixed_bytes, code, "publication.bytes", "Truncated Publication fixed body");
      const auto count = read_integer<std::uint32_t>(bytes, 48);
      const auto payloadBytes = read_integer<std::uint64_t>(bytes, 64);
      const auto layout = make_layout(recordOffset, count, payloadBytes, maxRecordBytes, code);
      require(bytes.size() == layout._immutableBytes, code, "publication.bytes", "Publication immutable byte count is not canonical");
      require(zero(bytes.subspan(52, 4)) && zero(bytes.subspan(114, 14)), code,
              "publication.reserved", "Publication reserved bytes must be zero");
      require(read_integer<std::uint64_t>(bytes, 56) == layout._payloadOffset &&
                  read_integer<std::uint64_t>(bytes, 80) == layout._destinationsOffset &&
                  read_integer<std::uint64_t>(bytes, 88) == count &&
                  read_integer<std::uint64_t>(bytes, 96) == layout._receiptsOffset &&
                  read_integer<std::uint64_t>(bytes, 104) == count,
              code, "publication.slices", "Publication slices must match its canonical layout and destination count");
      const auto payloadBegin = publication_fixed_bytes + std::size_t{count} * destination_row_bytes;
      const auto payloadEnd = payloadBegin + host_size(payloadBytes, code);
      require(zero(bytes.subspan(payloadEnd)), code, "publication.padding", "Publication alignment padding must be zero");
      PublicationDefinition result;
      result._producer = TaskId{read_integer<std::uint16_t>(bytes, 0)};
      result._publishingSet = PublishingQueueSetId{read_integer<std::uint16_t>(bytes, 2)};
      result._schema = SchemaId{MessageType{read_integer<std::uint16_t>(bytes, 4)}, read_integer<std::uint32_t>(bytes, 8)};
      result._format = decode_format(read_integer<std::uint16_t>(bytes, 6), code);
      result._flags = read_integer<std::uint32_t>(bytes, 12);
      result._producerIncarnation = read_integer<std::uint64_t>(bytes, 16);
      result._publicationSequence = read_integer<std::uint64_t>(bytes, 24);
      result._publishedAt = UtcTimestamp{std::chrono::nanoseconds{
          std::bit_cast<std::int64_t>(read_integer<std::uint64_t>(bytes, 32))}};
      result._topologyRevision = read_integer<std::uint64_t>(bytes, 40);
      result._schemaReference = read_integer<std::uint64_t>(bytes, 72);
      result._acceptance = decode_acceptance(read_integer<std::uint16_t>(bytes, 112), code);
      result._destinations.reserve(count);
      for (std::size_t index = 0; index < count; ++index)
      {
        result._destinations.push_back(read_destination(bytes.subspan(publication_fixed_bytes + index * destination_row_bytes,
                                                                       destination_row_bytes), recordOffset,
                                                         layout._receiptsOffset + index * publication_receipt_bytes, code));
      }
      check_publication(result, layout, recordOffset, code);
      result._payload.assign(bytes.begin() + payloadBegin, bytes.begin() + payloadEnd);
      return result;
    });
  }
} // namespace angaria::detail::mapped
