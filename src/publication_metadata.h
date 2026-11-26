#pragma once

#include "angaria/descriptors.h"
#include "angaria/results.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace angaria::detail::mapped
{
  inline constexpr std::size_t publication_fixed_bytes = 128;
  inline constexpr std::size_t destination_row_bytes = 48;
  inline constexpr std::size_t publication_receipt_bytes = 128;

  struct DestinationDefinition
  {
    QueueId _queue{};
    DestinationStatus _status{DestinationStatus::Accepted};
    ErrorCode _reason{ErrorCode::None};
    std::uint64_t _queueSequence{};
    std::uint64_t _previousDelivery{};
    UtcTimestamp _enqueuedAt{};
    std::uint64_t _receiptReference{};

    bool operator==(const DestinationDefinition&) const = default;
  };

  struct PublicationDefinition
  {
    TaskId _producer{};
    PublishingQueueSetId _publishingSet{};
    SchemaId _schema{};
    PayloadFormat _format{PayloadFormat::Binary};
    std::uint32_t _flags{};
    std::uint64_t _producerIncarnation{};
    std::uint64_t _publicationSequence{};
    UtcTimestamp _publishedAt{};
    std::uint64_t _topologyRevision{};
    std::uint64_t _schemaReference{};
    AcceptancePolicy _acceptance{AcceptancePolicy::AllDestinations};
    std::vector<DestinationDefinition> _destinations;
    std::vector<std::byte> _payload;

    bool operator==(const PublicationDefinition&) const = default;
  };

  struct PublicationLayout
  {
    std::uint64_t _destinationsOffset{};
    std::uint64_t _payloadOffset{};
    std::uint64_t _receiptsOffset{};
    std::uint64_t _immutableBytes{};
    std::uint64_t _totalBytes{};

    bool operator==(const PublicationLayout&) const = default;
  };

  using DestinationRowBytes = std::array<std::byte, destination_row_bytes>;

  // These codecs handle local byte copies, never live mapped atomic words.
  // They validate representation and references' shape; the runtime must still
  // resolve committed records, Queue ownership, schema identity, and capability.
  // maxRecordBytes bounds the full record, including envelope, receipt slots,
  // and allocation padding. Immutable bodies exclude all of those suffix bytes.
  Result<PublicationLayout> plan_publication_layout(std::uint64_t recordOffset,
                                                    std::uint64_t destinationCount,
                                                    std::uint64_t payloadBytes,
                                                    std::uint64_t maxRecordBytes);
  Result<DestinationRowBytes> encode_destination_row(const DestinationDefinition& destination,
                                                     std::uint64_t publicationOffset,
                                                     std::uint64_t expectedReceiptReference);
  Result<DestinationDefinition> decode_destination_row(std::span<const std::byte> bytes,
                                                       std::uint64_t publicationOffset,
                                                       std::uint64_t expectedReceiptReference);
  Result<std::vector<std::byte>> encode_publication_body(const PublicationDefinition& publication,
                                                         std::uint64_t recordOffset,
                                                         std::uint64_t maxRecordBytes);
  Result<PublicationDefinition> decode_publication_body(std::span<const std::byte> immutableBody,
                                                        std::uint64_t recordOffset,
                                                        std::uint64_t maxRecordBytes);
} // namespace angaria::detail::mapped
