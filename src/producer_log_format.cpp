#include "producer_log_format.h"

#include "angaria/detail/byte_operations.h"
#include "angaria/detail/checked_arithmetic.h"
#include "angaria/detail/error_support.h"
#include "angaria/detail/wire_codes.h"

#include <algorithm>
#include <bitset>
#include <chrono>
#include <limits>
#include <string_view>
#include <utility>

namespace angaria::detail::recorded
{
  namespace
  {
    namespace wire = angaria::detail::wire;
    constexpr std::string_view header_magic = "ANGAPLOG";
    constexpr std::string_view record_magic = "ANPR";

    template <typename T, typename Function>
    Result<T> protect(Function&& operation)
    {
      try
      {
        return operation();
      }
      catch (const CodecFailure& failure)
      {
        return failure._error;
      }
    }

    void require(bool condition, ErrorCode code, std::string_view message)
    {
      codec_require(condition, code, "producer_log", message);
    }

    void check_header(const ProducerLogHeader& header, ErrorCode code)
    {
      require((header._session._high | header._session._low) != 0 &&
                  header._producer != TaskId::Invalid && header._catalogRevision != 0 &&
                  header._liveAbiProfile != 0,
              code, "Invalid producer log identity, catalog revision or native profile");
      require(fits_host_size(header._fileBytes) &&
                  header._fileBytes >= producer_header_bytes + minimum_publication_bytes &&
                  header._fileBytes % publication_alignment == 0,
              code, "Invalid producer log byte reservation");
      require(header._publicationCapacity != 0 &&
                  header._publicationCapacity <=
                      (header._fileBytes - producer_header_bytes) / minimum_publication_bytes,
              code, "Invalid producer publication capacity");
    }

    void check_reference(const LogReference& reference, const ProducerLogHeader& header, ErrorCode code)
    {
      require(reference._session == header._session && reference._producer == header._producer,
              code, "Reference belongs to another producer log");
      require(reference._offset >= producer_header_bytes &&
                  reference._offset % publication_alignment == 0 &&
                  reference._end > reference._offset &&
                  reference._end % publication_alignment == 0 && reference._end <= header._fileBytes &&
                  reference._end - reference._offset >= minimum_publication_bytes,
              code, "Invalid producer log reference range");
    }

    void put_magic(std::span<std::byte> bytes, std::string_view magic)
    {
      for (std::size_t index = 0; index != magic.size(); ++index)
      {
        bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(magic[index]));
      }
    }

    bool matches_magic(std::span<const std::byte> bytes, std::string_view magic)
    {
      for (std::size_t index = 0; index != magic.size(); ++index)
      {
        if (bytes[index] != static_cast<std::byte>(static_cast<unsigned char>(magic[index])))
        {
          return false;
        }
      }
      return true;
    }

    template <typename Enum, std::size_t Size>
    std::uint16_t encode_code(Enum value, const std::array<Enum, Size>& codes,
                              ErrorCode error, std::uint16_t first = 1)
    {
      const auto encoded = wire::encode(value, codes, first);
      require(encoded.has_value(), error, "Unknown producer log wire value");
      return *encoded;
    }

    template <typename Enum, std::size_t Size>
    Enum decode_code(std::uint16_t value, const std::array<Enum, Size>& codes,
                     ErrorCode error, std::uint16_t first = 1)
    {
      const auto decoded = wire::decode(value, codes, first);
      require(decoded.has_value(), error, "Unknown stored producer log wire value");
      return *decoded;
    }

    PublicationRecordLayout geometry(const ProducerLogHeader& header, std::uint64_t offset,
                                     std::uint64_t destinations, std::uint64_t payload,
                                     ErrorCode code)
    {
      require(offset >= producer_header_bytes && offset % publication_alignment == 0 &&
                  destinations != 0 && destinations <= std::numeric_limits<std::uint16_t>::max(),
              code, "Invalid publication offset or destination count");
      const auto rows = checked_multiply(destinations, destination_bytes);
      const auto payloadOffset = rows ? checked_add(publication_fixed_bytes, *rows) : std::nullopt;
      const auto beforePadding = payloadOffset ? checked_add(*payloadOffset, payload) : std::nullopt;
      const auto bytes = beforePadding ? checked_align(*beforePadding, publication_alignment) : std::nullopt;
      require(bytes.has_value() && fits_host_size(*bytes) &&
                  contains_range(header._fileBytes, offset, *bytes),
              code, "Publication exceeds its producer log reservation");
      return PublicationRecordLayout{*bytes, offset + *bytes, static_cast<std::size_t>(*payloadOffset)};
    }

    void check_fields(const MessageHeader& message, std::uint64_t ordinal, std::uint64_t schema,
                       std::uint64_t topology, PublishingQueueSetId publishing, AcceptancePolicy acceptance,
                       std::size_t destinations, std::uint64_t payloadBytes,
                       const ProducerLogHeader& header, ErrorCode code)
    {
      require(message._publication._producer._session == header._session &&
                  message._publication._producer._task == header._producer &&
                  message._publication._producer._incarnation != 0 && message._publication._sequence != 0,
              code, "Invalid publication claim or sequence");
      require(message._schema._type != MessageType::Invalid && message._schema._version != 0 &&
                  schema >= producer_header_bytes && schema % 8 == 0 && fits_host_size(schema) && topology != 0,
              code, "Invalid publication schema reference or topology revision");
      require(!message._queue && !message._queueSequence && !message._consumer &&
                  !message._enqueuedAt && !message._dequeuedAt && message._payloadBytes == payloadBytes,
              code, "Publication header has delivery context or inconsistent payload size");
      require(ordinal != 0 && ordinal <= header._publicationCapacity,
              code, "Publication ordinal exceeds its reservation");
      encode_code(message._format, wire::payload_formats, code);
      encode_code(acceptance, wire::acceptance_policies, code);
      require(destinations != 0 && destinations <= std::numeric_limits<std::uint16_t>::max() &&
                  (publishing != PublishingQueueSetId::Invalid ||
                   (destinations == 1 && acceptance == AcceptancePolicy::AllDestinations)),
              code, "Invalid direct publication or Publishing destination snapshot");
    }

    void check_destination(const DestinationRecord& destination, std::uint64_t capacity, ErrorCode code)
    {
      require(destination._queue != QueueId::Invalid, code, "Invalid destination Queue identity");
      encode_code(destination._reason, wire::error_codes, code, 0);
      if (destination._status == DestinationStatus::Accepted)
      {
        require(destination._reason == ErrorCode::None && destination._queueSequence != 0 &&
                    destination._queueSequence <= capacity,
                code, "Invalid accepted destination sequence or reason");
      }
      else
      {
        require(destination._status == DestinationStatus::Rejected && destination._reason != ErrorCode::None &&
                    destination._reason != ErrorCode::OutcomeUnknown && destination._queueSequence == 0 &&
                    destination._enqueuedAt == UtcTimestamp{},
                code, "Invalid rejected destination state");
      }
    }

    template <typename Read>
    void check_multiple_destinations(std::size_t count, Read&& read, AcceptancePolicy acceptance,
                                     std::uint64_t capacity, ErrorCode code)
    {
      std::bitset<65536> seen;
      std::size_t accepted{};
      for (std::size_t index = 0; index != count; ++index)
      {
        const auto destination = read(index);
        check_destination(destination, capacity, code);
        const auto queue = static_cast<std::uint16_t>(destination._queue);
        require(!seen.test(queue), code, "Duplicate Queue in publication destinations");
        seen.set(queue);
        accepted += destination._status == DestinationStatus::Accepted;
      }
      require(accepted != 0 && (acceptance != AcceptancePolicy::AllDestinations || accepted == count),
              code, "Publication has no acceptance or violates all-destination acceptance");
    }

    template <typename Read>
    void check_destinations(std::size_t count, Read&& read, AcceptancePolicy acceptance,
                            std::uint64_t capacity, ErrorCode code)
    {
      if (count == 1)
      {
        // The common point-to-point path does not initialize uniqueness scratch.
        const auto destination = read(0);
        check_destination(destination, capacity, code);
        require(destination._status == DestinationStatus::Accepted, code, "Publication has no accepted destination");
        return;
      }
      check_multiple_destinations(count, std::forward<Read>(read), acceptance, capacity, code);
    }

    DestinationRecord read_destination(std::span<const std::byte> bytes, ErrorCode code)
    {
      require(all_zero(bytes.subspan(6, 2)) && all_zero(bytes.subspan(24, 8)),
              code, "Nonzero destination reserved bytes");
      const auto status = read_unsigned_le<std::uint16_t>(bytes, 2);
      require(status == 1 || status == 2, code, "Unknown destination acceptance code");
      return DestinationRecord{
          QueueId{read_unsigned_le<std::uint16_t>(bytes, 0)},
          status == 1 ? DestinationStatus::Accepted : DestinationStatus::Rejected,
          decode_code(read_unsigned_le<std::uint16_t>(bytes, 4), wire::error_codes, code, 0),
          read_unsigned_le<std::uint64_t>(bytes, 8),
          UtcTimestamp{std::chrono::nanoseconds{signed_from_bits<std::int64_t>(
              read_unsigned_le<std::uint64_t>(bytes, 16))}}};
    }

    void write_destination(std::span<std::byte> bytes, const DestinationRecord& destination)
    {
      std::fill(bytes.begin(), bytes.end(), std::byte{});
      write_unsigned_le<std::uint16_t>(bytes, 0, static_cast<std::uint16_t>(destination._queue));
      write_unsigned_le<std::uint16_t>(bytes, 2, destination._status == DestinationStatus::Accepted ? 1 : 2);
      write_unsigned_le<std::uint16_t>(bytes, 4, *wire::encode(destination._reason, wire::error_codes, 0));
      write_unsigned_le<std::uint64_t>(bytes, 8, destination._queueSequence);
      write_unsigned_le<std::uint64_t>(bytes, 16, static_cast<std::uint64_t>(destination._enqueuedAt.time_since_epoch().count()));
    }

    PublicationRecordLayout prepare(const PublicationRecordInput& publication, const ProducerLogHeader& header,
                                     std::uint64_t offset)
    {
      check_header(header, ErrorCode::InvalidArgument);
      require(offset >= producer_header_bytes && offset % publication_alignment == 0,
              ErrorCode::InvalidArgument, "Invalid publication record offset");
      require(publication._recordOrdinal <= header._publicationCapacity,
              ErrorCode::LogFull, "Producer publication capacity is exhausted");
      check_fields(publication._header, publication._recordOrdinal, publication._schemaReference,
                   publication._topologyRevision, publication._publishingSet, publication._acceptance,
                   publication._destinations.size(), publication._payload.size(), header, ErrorCode::InvalidArgument);
      check_destinations(publication._destinations.size(), [&](std::size_t index)
      {
        return publication._destinations[index];
      }, publication._acceptance, header._publicationCapacity, ErrorCode::InvalidArgument);
      return geometry(header, offset, publication._destinations.size(), publication._payload.size(), ErrorCode::LogFull);
    }
  } // namespace

  Result<ProducerLogHeaderBytes> encode_producer_header(const ProducerLogHeader& header)
  {
    return protect<ProducerLogHeaderBytes>([&]
    {
      check_header(header, ErrorCode::InvalidArgument);
      ProducerLogHeaderBytes bytes{};
      put_magic(bytes, header_magic);
      write_unsigned_le<std::uint16_t>(bytes, 8, log_format_major);
      write_unsigned_le<std::uint16_t>(bytes, 10, log_format_minor);
      write_unsigned_le<std::uint32_t>(bytes, 12, producer_header_bytes);
      write_unsigned_le<std::uint64_t>(bytes, 16, header._session._high);
      write_unsigned_le<std::uint64_t>(bytes, 24, header._session._low);
      write_unsigned_le<std::uint16_t>(bytes, 32, static_cast<std::uint16_t>(header._producer));
      write_unsigned_le<std::uint64_t>(bytes, 40, header._catalogRevision);
      write_unsigned_le<std::uint64_t>(bytes, 48, header._fileBytes);
      write_unsigned_le<std::uint64_t>(bytes, 56, header._publicationCapacity);
      write_unsigned_le<std::uint32_t>(bytes, 64, header._liveAbiProfile);
      write_unsigned_le<std::uint16_t>(bytes, 68, log_synchronization_version);
      write_unsigned_le<std::uint16_t>(bytes, 70, 8);
      write_unsigned_le<std::uint64_t>(bytes, 72, owned_log_feature);
      write_unsigned_le<std::uint64_t>(bytes, 88, producer_header_bytes);
      return bytes;
    });
  }

  Result<ProducerLogHeader> decode_producer_header(std::span<const std::byte> bytes)
  {
    return protect<ProducerLogHeader>([&]
    {
      require(bytes.size() == producer_immutable_bytes, ErrorCode::IoError, "Expected exactly the immutable producer header");
      require(matches_magic(bytes, header_magic), ErrorCode::IoError, "Invalid producer log magic");
      require(read_unsigned_le<std::uint16_t>(bytes, 8) == log_format_major &&
                  read_unsigned_le<std::uint16_t>(bytes, 10) == log_format_minor &&
                  read_unsigned_le<std::uint16_t>(bytes, 68) == log_synchronization_version &&
                  read_unsigned_le<std::uint16_t>(bytes, 70) == 8 &&
                  read_unsigned_le<std::uint64_t>(bytes, 72) == owned_log_feature &&
                  read_unsigned_le<std::uint64_t>(bytes, 80) == 0,
              ErrorCode::UnsupportedPolicy, "Unsupported producer log format or synchronization capabilities");
      require(read_unsigned_le<std::uint32_t>(bytes, 12) == producer_header_bytes &&
                  read_unsigned_le<std::uint64_t>(bytes, 88) == producer_header_bytes &&
                  all_zero(bytes.subspan(34, 6)) && all_zero(bytes.subspan(96)),
              ErrorCode::IoError, "Invalid producer header geometry or reserved bytes");
      ProducerLogHeader header{
          SessionId{read_unsigned_le<std::uint64_t>(bytes, 16), read_unsigned_le<std::uint64_t>(bytes, 24)},
          TaskId{read_unsigned_le<std::uint16_t>(bytes, 32)}, read_unsigned_le<std::uint64_t>(bytes, 40),
          read_unsigned_le<std::uint64_t>(bytes, 48), read_unsigned_le<std::uint64_t>(bytes, 56),
          read_unsigned_le<std::uint32_t>(bytes, 64)};
      check_header(header, ErrorCode::IoError);
      return header;
    });
  }

  OperationResult live_profile_compatibility(const ProducerLogHeader& header, std::uint32_t qualifiedProfile)
  {
    return protect<std::monostate>([&]
    {
      check_header(header, ErrorCode::InvalidArgument);
      require(qualifiedProfile != 0, ErrorCode::InvalidArgument, "Native profile has not been qualified");
      require(header._liveAbiProfile == qualifiedProfile, ErrorCode::UnsupportedPolicy, "Producer log native ABI mismatch");
      return std::monostate{};
    });
  }

  OperationResult validate_committed_tail(const ProducerLogHeader& header, std::uint64_t tail)
  {
    return protect<std::monostate>([&]
    {
      check_header(header, ErrorCode::IoError);
      require(tail >= producer_header_bytes && tail <= header._fileBytes && tail % publication_alignment == 0,
              ErrorCode::IoError, "Invalid acquired producer committed tail");
      return std::monostate{};
    });
  }

  Result<LogReferenceBytes> encode_log_reference(const LogReference& reference, const ProducerLogHeader& header)
  {
    return protect<LogReferenceBytes>([&]
    {
      check_header(header, ErrorCode::InvalidArgument);
      check_reference(reference, header, ErrorCode::InvalidArgument);
      LogReferenceBytes bytes{};
      write_unsigned_le<std::uint64_t>(bytes, 0, reference._session._high);
      write_unsigned_le<std::uint64_t>(bytes, 8, reference._session._low);
      write_unsigned_le<std::uint16_t>(bytes, 16, static_cast<std::uint16_t>(reference._producer));
      write_unsigned_le<std::uint16_t>(bytes, 18, 1);
      write_unsigned_le<std::uint64_t>(bytes, 24, reference._offset);
      write_unsigned_le<std::uint64_t>(bytes, 32, reference._end);
      return bytes;
    });
  }

  Result<LogReference> decode_log_reference(std::span<const std::byte> bytes, const ProducerLogHeader& header)
  {
    return protect<LogReference>([&]
    {
      check_header(header, ErrorCode::IoError);
      require(bytes.size() == log_reference_bytes, ErrorCode::IoError, "Invalid producer reference size");
      require(read_unsigned_le<std::uint16_t>(bytes, 18) == 1, ErrorCode::UnsupportedPolicy, "Unsupported reference encoding");
      require(all_zero(bytes.subspan(20, 4)) && all_zero(bytes.subspan(40)), ErrorCode::IoError, "Nonzero reference reserved bytes");
      LogReference reference{
          SessionId{read_unsigned_le<std::uint64_t>(bytes, 0), read_unsigned_le<std::uint64_t>(bytes, 8)},
          TaskId{read_unsigned_le<std::uint16_t>(bytes, 16)},
          read_unsigned_le<std::uint64_t>(bytes, 24), read_unsigned_le<std::uint64_t>(bytes, 32)};
      check_reference(reference, header, ErrorCode::IoError);
      return reference;
    });
  }

  Result<PublicationRecordLayout> plan_publication_record(const PublicationRecordInput& publication,
                                                          const ProducerLogHeader& header, std::uint64_t recordOffset)
  {
    return protect<PublicationRecordLayout>([&]
    {
      return prepare(publication, header, recordOffset);
    });
  }

  Result<LogReference> encode_publication_record(const PublicationRecordInput& publication,
                                                const ProducerLogHeader& header, std::uint64_t recordOffset,
                                                std::span<std::byte> output)
  {
    return protect<LogReference>([&]
    {
      const auto layout = prepare(publication, header, recordOffset);
      require(output.size() >= layout._bytes, ErrorCode::OutputBufferTooSmall, "Publication output buffer is too small");
      const auto bytes = output.first(static_cast<std::size_t>(layout._bytes));
      // Clear fixed metadata/padding only; do not write the payload twice.
      std::fill_n(bytes.begin(), publication_fixed_bytes, std::byte{});
      put_magic(bytes, record_magic);
      write_unsigned_le<std::uint16_t>(bytes, 4, 1);
      write_unsigned_le<std::uint16_t>(bytes, 6, 1);
      write_unsigned_le<std::uint64_t>(bytes, 8, layout._bytes);
      write_unsigned_le<std::uint64_t>(bytes, 16, recordOffset);
      write_unsigned_le<std::uint64_t>(bytes, 24, publication._header._publication._producer._incarnation);
      write_unsigned_le<std::uint64_t>(bytes, 32, publication._header._publication._sequence);
      write_unsigned_le<std::uint64_t>(bytes, 40, static_cast<std::uint64_t>(publication._header._publishedAt.time_since_epoch().count()));
      write_unsigned_le<std::uint64_t>(bytes, 48, header._catalogRevision);
      write_unsigned_le<std::uint64_t>(bytes, 56, publication._schemaReference);
      write_unsigned_le<std::uint16_t>(bytes, 64, static_cast<std::uint16_t>(publication._header._schema._type));
      write_unsigned_le<std::uint16_t>(bytes, 66, *wire::encode(publication._header._format, wire::payload_formats));
      write_unsigned_le<std::uint32_t>(bytes, 68, publication._header._schema._version);
      write_unsigned_le<std::uint32_t>(bytes, 72, publication._header._flags);
      write_unsigned_le<std::uint16_t>(bytes, 76, static_cast<std::uint16_t>(publication._publishingSet));
      write_unsigned_le<std::uint16_t>(bytes, 78, *wire::encode(publication._acceptance, wire::acceptance_policies));
      write_unsigned_le<std::uint64_t>(bytes, 80, publication._topologyRevision);
      write_unsigned_le<std::uint32_t>(bytes, 88, static_cast<std::uint32_t>(publication._destinations.size()));
      write_unsigned_le<std::uint64_t>(bytes, 96, publication._payload.size());
      write_unsigned_le<std::uint64_t>(bytes, 104, publication._recordOrdinal);
      write_unsigned_le<std::uint16_t>(bytes, 112, static_cast<std::uint16_t>(header._producer));
      for (std::size_t index = 0; index != publication._destinations.size(); ++index)
      {
        write_destination(bytes.subspan(publication_fixed_bytes + index * destination_bytes, destination_bytes),
                          publication._destinations[index]);
      }
      std::copy(publication._payload.begin(), publication._payload.end(), bytes.begin() + layout._payloadOffset);
      std::fill(bytes.begin() + layout._payloadOffset + publication._payload.size(), bytes.end(), std::byte{});
      return LogReference{header._session, header._producer, recordOffset, layout._end};
    });
  }

  Result<PublicationRecordView> decode_publication_record(std::span<const std::byte> bytes,
                                                         const ProducerLogHeader& header, const LogReference& reference)
  {
    return protect<PublicationRecordView>([&]
    {
      check_header(header, ErrorCode::IoError);
      check_reference(reference, header, ErrorCode::IoError);
      require(bytes.size() == reference._end - reference._offset && bytes.size() >= publication_fixed_bytes,
              ErrorCode::IoError, "Publication bytes do not match the complete reference");
      require(matches_magic(bytes, record_magic), ErrorCode::IoError, "Invalid producer publication magic");
      require(read_unsigned_le<std::uint16_t>(bytes, 4) == 1 && read_unsigned_le<std::uint16_t>(bytes, 6) == 1,
              ErrorCode::UnsupportedPolicy, "Unsupported producer publication kind or encoding");
      require(read_unsigned_le<std::uint64_t>(bytes, 8) == bytes.size() &&
                  read_unsigned_le<std::uint64_t>(bytes, 16) == reference._offset &&
                  read_unsigned_le<std::uint64_t>(bytes, 48) == header._catalogRevision &&
                  read_unsigned_le<std::uint16_t>(bytes, 112) == static_cast<std::uint16_t>(header._producer) &&
                  all_zero(bytes.subspan(92, 4)) && all_zero(bytes.subspan(114, 14)),
              ErrorCode::IoError, "Publication framing, catalog identity or reserved bytes disagree");
      const auto count = read_unsigned_le<std::uint32_t>(bytes, 88);
      const auto payloadBytes = read_unsigned_le<std::uint64_t>(bytes, 96);
      const auto layout = geometry(header, reference._offset, count, payloadBytes, ErrorCode::IoError);
      require(layout._end == reference._end, ErrorCode::IoError, "Noncanonical publication length");
      PublicationRecordView view;
      view._header._schema = SchemaId{MessageType{read_unsigned_le<std::uint16_t>(bytes, 64)},
                                      read_unsigned_le<std::uint32_t>(bytes, 68)};
      view._header._format = decode_code(read_unsigned_le<std::uint16_t>(bytes, 66), wire::payload_formats, ErrorCode::IoError);
      view._header._publication = PublicationId{TaskClaim{header._session, header._producer,
          read_unsigned_le<std::uint64_t>(bytes, 24)}, read_unsigned_le<std::uint64_t>(bytes, 32)};
      view._header._publishedAt = UtcTimestamp{std::chrono::nanoseconds{signed_from_bits<std::int64_t>(
          read_unsigned_le<std::uint64_t>(bytes, 40))}};
      view._header._payloadBytes = payloadBytes;
      view._header._flags = read_unsigned_le<std::uint32_t>(bytes, 72);
      view._reference = reference;
      view._recordOrdinal = read_unsigned_le<std::uint64_t>(bytes, 104);
      view._schemaReference = read_unsigned_le<std::uint64_t>(bytes, 56);
      view._topologyRevision = read_unsigned_le<std::uint64_t>(bytes, 80);
      view._publishingSet = PublishingQueueSetId{read_unsigned_le<std::uint16_t>(bytes, 76)};
      view._acceptance = decode_code(read_unsigned_le<std::uint16_t>(bytes, 78), wire::acceptance_policies, ErrorCode::IoError);
      check_fields(view._header, view._recordOrdinal, view._schemaReference, view._topologyRevision,
                   view._publishingSet, view._acceptance, count, payloadBytes, header, ErrorCode::IoError);
      view._destinationRows = bytes.subspan(publication_fixed_bytes, count * destination_bytes);
      check_destinations(count, [&](std::size_t index)
      {
        return read_destination(view._destinationRows.subspan(index * destination_bytes, destination_bytes), ErrorCode::IoError);
      }, view._acceptance, header._publicationCapacity, ErrorCode::IoError);
      view._payload = bytes.subspan(layout._payloadOffset, static_cast<std::size_t>(payloadBytes));
      require(all_zero(bytes.subspan(layout._payloadOffset + static_cast<std::size_t>(payloadBytes))),
              ErrorCode::IoError, "Nonzero publication alignment padding");
      return view;
    });
  }

  std::size_t PublicationRecordView::destination_count() const noexcept
  {
    return _destinationRows.size() / destination_bytes;
  }

  Result<DestinationRecord> PublicationRecordView::destination_at(std::size_t index) const
  {
    return protect<DestinationRecord>([&]
    {
      require(index < destination_count(), ErrorCode::InvalidArgument, "Destination index is out of bounds");
      return read_destination(_destinationRows.subspan(index * destination_bytes, destination_bytes), ErrorCode::IoError);
    });
  }
} // namespace angaria::detail::recorded
