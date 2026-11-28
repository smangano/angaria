#pragma once

#include "angaria/descriptors.h"
#include "angaria/message.h"
#include "angaria/results.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace angaria::detail::recorded
{
  // Private format-3 codecs for local byte snapshots. They never read live
  // atomic words, open files, establish ownership, or prove record commitment.
  inline constexpr std::uint16_t log_format_major = 3;
  inline constexpr std::uint16_t log_format_minor = 0;
  inline constexpr std::uint16_t log_synchronization_version = 2;
  inline constexpr std::uint64_t owned_log_feature = 1;
  inline constexpr std::size_t producer_header_bytes = 4096;
  inline constexpr std::size_t producer_immutable_bytes = 256;
  inline constexpr std::size_t committed_tail_offset = 256;
  inline constexpr std::size_t control_alignment = 256;
  inline constexpr std::size_t log_reference_bytes = 48;
  inline constexpr std::size_t publication_fixed_bytes = 128;
  inline constexpr std::size_t destination_bytes = 32;
  inline constexpr std::size_t publication_alignment = 64;
  inline constexpr std::size_t minimum_publication_bytes = 192;

  struct ProducerLogHeader
  {
    SessionId _session{};
    TaskId _producer{};
    std::uint64_t _catalogRevision{};
    std::uint64_t _fileBytes{};
    std::uint64_t _publicationCapacity{};
    std::uint32_t _liveAbiProfile{};

    bool operator==(const ProducerLogHeader&) const = default;
  };

  struct LogReference
  {
    SessionId _session{};
    TaskId _producer{};
    std::uint64_t _offset{};
    std::uint64_t _end{};

    bool operator==(const LogReference&) const = default;
  };

  struct DestinationRecord
  {
    QueueId _queue{};
    DestinationStatus _status{DestinationStatus::Accepted};
    ErrorCode _reason{ErrorCode::None};
    std::uint64_t _queueSequence{};
    UtcTimestamp _enqueuedAt{};

    bool operator==(const DestinationRecord&) const = default;
  };

  struct PublicationRecordInput
  {
    // Authoritative publication fields only; destination/consumer fields are
    // supplied by the destination rows and later receive/progress operations.
    MessageHeader _header;
    std::uint64_t _recordOrdinal{};
    std::uint64_t _schemaReference{};
    std::uint64_t _topologyRevision{};
    PublishingQueueSetId _publishingSet{};
    AcceptancePolicy _acceptance{AcceptancePolicy::AllDestinations};
    std::span<const DestinationRecord> _destinations;
    std::span<const std::byte> _payload;
  };

  struct PublicationRecordLayout
  {
    std::uint64_t _bytes{};
    std::uint64_t _end{};
    // Offset relative to the start of the record, including an empty payload.
    std::size_t _payloadOffset{};

    bool operator==(const PublicationRecordLayout&) const = default;
  };

  class PublicationRecordView
  {
  public:
    MessageHeader _header;
    LogReference _reference;
    std::uint64_t _recordOrdinal{};
    std::uint64_t _schemaReference{};
    std::uint64_t _topologyRevision{};
    PublishingQueueSetId _publishingSet{};
    AcceptancePolicy _acceptance{AcceptancePolicy::AllDestinations};
    std::span<const std::byte> _payload;

    std::size_t destination_count() const noexcept;
    Result<DestinationRecord> destination_at(std::size_t index) const;

  private:
    friend Result<PublicationRecordView> decode_publication_record(
        std::span<const std::byte>, const ProducerLogHeader&, const LogReference&);
    std::span<const std::byte> _destinationRows;
  };

  using ProducerLogHeaderBytes = std::array<std::byte, producer_immutable_bytes>;
  using LogReferenceBytes = std::array<std::byte, log_reference_bytes>;

  // Header serialization excludes the separately accessed live committed tail.
  // Structural decoding permits a foreign nonzero ABI for offline inspection;
  // live_profile_compatibility is the separate native admission check.
  Result<ProducerLogHeaderBytes> encode_producer_header(const ProducerLogHeader& header);
  Result<ProducerLogHeader> decode_producer_header(std::span<const std::byte> bytes);
  OperationResult live_profile_compatibility(const ProducerLogHeader& header,
                                            std::uint32_t qualifiedProfile);
  OperationResult validate_committed_tail(const ProducerLogHeader& header, std::uint64_t tail);
  Result<LogReferenceBytes> encode_log_reference(const LogReference& reference,
                                               const ProducerLogHeader& header);
  Result<LogReference> decode_log_reference(std::span<const std::byte> bytes,
                                          const ProducerLogHeader& header);
  Result<PublicationRecordLayout> plan_publication_record(const PublicationRecordInput& publication,
                                                          const ProducerLogHeader& header,
                                                          std::uint64_t recordOffset);

  // Inputs and output must be disjoint. A too-small output or invalid request
  // leaves output unchanged; success touches only the planned record extent.
  // No successful encode/decode allocates. Payload copying occurs only here;
  // decode returns views borrowing the caller's complete local/committed bytes.
  Result<LogReference> encode_publication_record(const PublicationRecordInput& publication,
                                                const ProducerLogHeader& header,
                                                std::uint64_t recordOffset,
                                                std::span<std::byte> output);
  // bytes must cover exactly reference.end-reference.offset, not later records.
  // The runtime must separately acquire the stream's committed boundary and
  // establish that this reference names an accepted record before exposing it.
  Result<PublicationRecordView> decode_publication_record(std::span<const std::byte> bytes,
                                                         const ProducerLogHeader& header,
                                                         const LogReference& reference);
} // namespace angaria::detail::recorded
