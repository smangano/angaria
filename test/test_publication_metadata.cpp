// Private native Publication fixtures, not working Queue send examples. The
// independent golden bodies below are written from the stored-layout contract.
// These tests establish representation checks before runtime history/ownership
// validation and public delivery are introduced in subsequent milestone steps.

#include "wire_fixture.h"
#include "result_assertions.h"
#include "publication_metadata.h"

#include "angaria/detail/mapped_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;
  constexpr std::uint64_t record_offset = 8192;
  constexpr std::uint64_t record_limit = 384;
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();

  using angaria::test_support::put_integer;

  using angaria::test_support::require_value;

  using angaria::test_support::expect_error;

  mapped::PublicationDefinition direct_publication()
  {
    mapped::PublicationDefinition publication;
    publication._producer = TaskId{65535};
    publication._schema = SchemaId{MessageType{65534}, 0xffffffffu};
    publication._flags = 0xff00a55au;
    publication._producerIncarnation = maximum;
    publication._publicationSequence = maximum;
    publication._publishedAt = UtcTimestamp{std::chrono::nanoseconds{-7}};
    publication._topologyRevision = maximum;
    publication._schemaReference = 4096;
    publication._payload = {std::byte{0}, std::byte{0xff}, std::byte{0x42}};
    mapped::DestinationDefinition destination;
    destination._queue = QueueId{65535};
    destination._queueSequence = maximum;
    // Only alignment and backward direction are checked here. A later history
    // loader must prove this reference actually names this Queue's predecessor.
    destination._previousDelivery = 7000;
    destination._enqueuedAt = publication._publishedAt;
    destination._receiptReference = 8440;
    publication._destinations.push_back(destination);
    return publication;
  }

  std::vector<std::byte> golden_direct()
  {
    // 128 fixed + 48 destination + 3 application bytes + 5 alignment bytes.
    // Receipt suffix starts at 8192 + 64 + 184 = 8440; it is not in this body.
    std::vector<std::byte> bytes(184);
    put_integer(bytes, 0, 65535, 2);
    put_integer(bytes, 4, 65534, 2);
    put_integer(bytes, 6, 1, 2);
    put_integer(bytes, 8, 0xffffffffu, 4);
    put_integer(bytes, 12, 0xff00a55au, 4);
    put_integer(bytes, 16, maximum, 8);
    put_integer(bytes, 24, maximum, 8);
    put_integer(bytes, 32, 0xfffffffffffffff9u, 8);
    put_integer(bytes, 40, maximum, 8);
    put_integer(bytes, 48, 1, 4);
    put_integer(bytes, 56, 8432, 8);
    put_integer(bytes, 64, 3, 8);
    put_integer(bytes, 72, 4096, 8);
    put_integer(bytes, 80, 8384, 8);
    put_integer(bytes, 88, 1, 8);
    put_integer(bytes, 96, 8440, 8);
    put_integer(bytes, 104, 1, 8);
    put_integer(bytes, 112, 1, 2);
    put_integer(bytes, 128, 65535, 2);
    put_integer(bytes, 130, 1, 2);
    put_integer(bytes, 136, maximum, 8);
    put_integer(bytes, 144, 7000, 8);
    put_integer(bytes, 152, 0xfffffffffffffff9u, 8);
    put_integer(bytes, 160, 8440, 8);
    bytes[177] = std::byte{0xff};
    bytes[178] = std::byte{0x42};
    return bytes;
  }

  mapped::PublicationDefinition partial_publication()
  {
    auto publication = direct_publication();
    publication._publishingSet = PublishingQueueSetId{9};
    publication._acceptance = AcceptancePolicy::Partial;
    publication._destinations.front()._receiptReference = 8488;
    mapped::DestinationDefinition rejected;
    rejected._queue = QueueId{1};
    rejected._status = DestinationStatus::Rejected;
    rejected._reason = ErrorCode::LogFull;
    rejected._receiptReference = 8616;
    publication._destinations.push_back(rejected);
    return publication;
  }

  std::vector<std::byte> golden_partial()
  {
    auto bytes = golden_direct();
    bytes.resize(232);
    std::fill(bytes.begin() + 176, bytes.end(), std::byte{});
    put_integer(bytes, 2, 9, 2);
    put_integer(bytes, 48, 2, 4);
    put_integer(bytes, 56, 8480, 8);
    put_integer(bytes, 88, 2, 8);
    put_integer(bytes, 96, 8488, 8);
    put_integer(bytes, 104, 2, 8);
    put_integer(bytes, 112, 2, 2);
    put_integer(bytes, 160, 8488, 8);
    put_integer(bytes, 176, 1, 2);
    put_integer(bytes, 178, 2, 2);
    put_integer(bytes, 180, 14, 2);
    put_integer(bytes, 208, 8616, 8);
    bytes[225] = std::byte{0xff};
    bytes[226] = std::byte{0x42};
    return bytes;
  }
}

TEST(PublicationMetadata, DirectPublicationMatchesIndependentGoldenBytes)
{
  const auto publication = direct_publication();
  const auto golden = golden_direct();
  EXPECT_EQ(require_value(mapped::encode_publication_body(publication, record_offset, record_limit)), golden);
  EXPECT_EQ(require_value(mapped::decode_publication_body(golden, record_offset, record_limit)), publication);
}

TEST(PublicationMetadata, LayoutIncludesEveryReceiptAndBothKindsOfPadding)
{
  const auto direct = require_value(mapped::plan_publication_layout(record_offset, 1, 3, record_limit));
  EXPECT_EQ(direct, (mapped::PublicationLayout{8384, 8432, 8440, 184, 384}));
  const auto partial = require_value(mapped::plan_publication_layout(record_offset, 2, 3, 576));
  EXPECT_EQ(partial, (mapped::PublicationLayout{8384, 8480, 8488, 232, 576}));
  // A caller's ceiling bounds the entire record, not merely the returned body.
  expect_error(mapped::plan_publication_layout(record_offset, 1, 3, 383), ErrorCode::InvalidArgument);
  expect_error(mapped::encode_publication_body(direct_publication(), record_offset, 383), ErrorCode::InvalidArgument);
  expect_error(mapped::decode_publication_body(golden_direct(), record_offset, 383), ErrorCode::IoError);
}

TEST(PublicationMetadata, PartialSnapshotPreservesOrderAndRejectedReceiptReservation)
{
  // This private format fixture exercises future fan-out snapshots. Public
  // Publishing and retry operations are still outside milestone 4.2.
  const auto publication = partial_publication();
  const auto golden = golden_partial();
  EXPECT_EQ(require_value(mapped::encode_publication_body(publication, record_offset, 576)), golden);
  EXPECT_EQ(require_value(mapped::decode_publication_body(golden, record_offset, 576)), publication);
}

TEST(PublicationMetadata, SingleDestinationPublishingSnapshotsPreservePoliciesAndMetadata)
{
  // Private stored-format snapshots, not public Publishing fan-out. A single
  // accepted Queue is valid under either policy, and a Publishing snapshot's
  // enqueue time need not share the direct-send timestamp sample.
  for (const auto policy : {AcceptancePolicy::AllDestinations, AcceptancePolicy::Partial})
  {
    SCOPED_TRACE(policy == AcceptancePolicy::Partial ? "Partial" : "AllDestinations");
    auto publication = direct_publication();
    publication._publishingSet = PublishingQueueSetId{9};
    publication._acceptance = policy;
    publication._destinations.front()._enqueuedAt = UtcTimestamp{std::chrono::nanoseconds{-6}};
    auto golden = golden_direct();
    put_integer(golden, 2, 9, 2);
    put_integer(golden, 112, policy == AcceptancePolicy::Partial ? 2 : 1, 2);
    put_integer(golden, 152, 0xfffffffffffffffau, 8);

    EXPECT_EQ(require_value(mapped::encode_publication_body(publication, record_offset, record_limit)), golden);
    EXPECT_EQ(require_value(mapped::decode_publication_body(golden, record_offset, record_limit)), publication);
    const auto metadata = require_value(mapped::decode_publication_metadata(
        std::span{golden}.first(128), std::span{golden}.subspan(128, 48),
        std::span{golden}.subspan(179, 5), 184, record_offset, record_limit));
    EXPECT_EQ(static_cast<const mapped::PublicationHeader&>(metadata),
              static_cast<const mapped::PublicationHeader&>(publication));
    EXPECT_EQ(metadata._destinations, publication._destinations);
    EXPECT_EQ(metadata._payloadBytes, 3u);
  }
}

TEST(PublicationMetadata, SingleDestinationPublishingSnapshotsRejectInvalidRowsAndFullyRejectedAttempts)
{
  // Deliberately corrupt independent bytes as well as typed input. Omitting a
  // uniqueness bitmap for one row must retain row validation and the rule that
  // even a Partial snapshot must contain an accepted destination.
  for (unsigned variant = 0; variant != 5; ++variant)
  {
    SCOPED_TRACE(variant);
    auto publication = direct_publication();
    publication._publishingSet = PublishingQueueSetId{9};
    publication._acceptance = AcceptancePolicy::Partial;
    auto golden = golden_direct();
    put_integer(golden, 2, 9, 2);
    put_integer(golden, 112, 2, 2);
    auto& destination = publication._destinations.front();
    switch (variant)
    {
    case 0:
      destination._queue = QueueId::Invalid;
      put_integer(golden, 128, 0, 2);
      break;
    case 1:
      destination._status = DestinationStatus::Unknown;
      put_integer(golden, 130, 0, 2);
      break;
    case 2:
      destination._reason = ErrorCode::LogFull;
      put_integer(golden, 132, 14, 2);
      break;
    case 3:
      destination._receiptReference = 8448;
      put_integer(golden, 160, 8448, 8);
      break;
    case 4:
      // This row is a valid rejection in isolation, but cannot constitute a
      // committed Publication without any accepted Queue.
      destination._status = DestinationStatus::Rejected;
      destination._reason = ErrorCode::LogFull;
      destination._queueSequence = 0;
      destination._previousDelivery = 0;
      destination._enqueuedAt = UtcTimestamp{};
      put_integer(golden, 130, 2, 2);
      put_integer(golden, 132, 14, 2);
      put_integer(golden, 136, 0, 8);
      put_integer(golden, 144, 0, 8);
      put_integer(golden, 152, 0, 8);
      break;
    }
    expect_error(mapped::encode_publication_body(publication, record_offset, record_limit), ErrorCode::InvalidArgument);
    expect_error(mapped::decode_publication_body(golden, record_offset, record_limit), ErrorCode::IoError);
    expect_error(mapped::decode_publication_metadata(
        std::span{golden}.first(128), std::span{golden}.subspan(128, 48),
        std::span{golden}.subspan(179, 5), 184, record_offset, record_limit), ErrorCode::IoError);
  }
}

TEST(PublicationMetadata, DestinationCodecMatchesIndependentRow)
{
  const auto golden = golden_direct();
  const auto row = std::span<const std::byte>{golden}.subspan(128, 48);
  const auto destination = direct_publication()._destinations.front();
  const auto encoded = require_value(mapped::encode_destination_row(destination, record_offset, 8440));
  EXPECT_TRUE(std::equal(encoded.begin(), encoded.end(), row.begin(), row.end()));
  EXPECT_EQ(require_value(mapped::decode_destination_row(row, record_offset, 8440)), destination);
  expect_error(mapped::decode_destination_row(row.first(47), record_offset, 8440), ErrorCode::IoError);
  expect_error(mapped::decode_destination_row(golden, record_offset, 8440), ErrorCode::IoError);
}

TEST(PublicationMetadata, EmptyPayloadUsesZeroSliceWithoutPortableHeader)
{
  auto publication = direct_publication();
  publication._payload.clear();
  publication._destinations.front()._receiptReference = 8432;
  const auto layout = require_value(mapped::plan_publication_layout(record_offset, 1, 0, record_limit));
  EXPECT_EQ(layout._payloadOffset, 0u);
  EXPECT_EQ(layout._immutableBytes, 176u);
  const auto bytes = require_value(mapped::encode_publication_body(publication, record_offset, record_limit));
  EXPECT_TRUE(std::all_of(bytes.begin() + 56, bytes.begin() + 72, [](std::byte byte)
  {
    return byte == std::byte{};
  }));
  EXPECT_EQ(require_value(mapped::decode_publication_body(bytes, record_offset, record_limit)), publication);
}

TEST(PublicationMetadata, PayloadFormatsUseStableCodesAndRemainOpaque)
{
  const std::array formats{PayloadFormat::Binary, PayloadFormat::Json, PayloadFormat::Xml,
                           PayloadFormat::Csv, PayloadFormat::Text};
  for (std::size_t index = 0; index < formats.size(); ++index)
  {
    auto publication = direct_publication();
    publication._format = formats[index];
    auto golden = golden_direct();
    put_integer(golden, 6, index + 1, 2);
    EXPECT_EQ(require_value(mapped::encode_publication_body(publication, record_offset, record_limit)), golden);
    EXPECT_EQ(require_value(mapped::decode_publication_body(golden, record_offset, record_limit)), publication);
  }
  // Binary schema validation belongs to message admission, not this byte codec.
  // The same arbitrary three-byte payload is preserved for all known formats.
}

TEST(PublicationMetadata, RejectionReasonsUseExplicitStableCodes)
{
  const std::array reasons{
      ErrorCode::InvalidArgument, ErrorCode::InvalidState, ErrorCode::InvalidConfiguration,
      ErrorCode::ConfigurationMismatch, ErrorCode::NotFound, ErrorCode::Conflict,
      ErrorCode::CapacityExceeded, ErrorCode::OwnershipBusy, ErrorCode::WrongOwner,
      ErrorCode::ReadOnlyRegistry, ErrorCode::NotActive, ErrorCode::UnsupportedPolicy,
      ErrorCode::SchemaConflict, ErrorCode::LogFull, ErrorCode::StaleDelivery,
      ErrorCode::OutputBufferTooSmall, ErrorCode::HistoryUnavailable, ErrorCode::IoError};
  for (std::size_t index = 0; index < reasons.size(); ++index)
  {
    auto publication = partial_publication();
    publication._destinations.back()._reason = reasons[index];
    auto golden = golden_partial();
    put_integer(golden, 180, index + 1, 2);
    EXPECT_EQ(require_value(mapped::encode_publication_body(publication, record_offset, 576)), golden);
    EXPECT_EQ(require_value(mapped::decode_publication_body(golden, record_offset, 576)), publication);
  }
}

TEST(PublicationMetadata, TimestampsPreserveEpochNegativeAndFullSignedRange)
{
  for (const auto time : {std::int64_t{0}, std::int64_t{-1}, std::numeric_limits<std::int64_t>::min(),
                          std::numeric_limits<std::int64_t>::max()})
  {
    auto publication = direct_publication();
    publication._publishedAt = UtcTimestamp{std::chrono::nanoseconds{time}};
    publication._destinations.front()._enqueuedAt = publication._publishedAt;
    const auto bytes = require_value(mapped::encode_publication_body(publication, record_offset, record_limit));
    EXPECT_EQ(require_value(mapped::decode_publication_body(bytes, record_offset, record_limit)), publication);
  }
}

TEST(PublicationMetadata, DirectSendUsesOneTimestampSample)
{
  auto publication = direct_publication();
  publication._destinations.front()._enqueuedAt = UtcTimestamp{};
  expect_error(mapped::encode_publication_body(publication, record_offset, record_limit), ErrorCode::InvalidArgument);
  auto bytes = golden_direct();
  put_integer(bytes, 152, 0, 8);
  expect_error(mapped::decode_publication_body(bytes, record_offset, record_limit), ErrorCode::IoError);
}

TEST(PublicationMetadata, DecoderReadsUnalignedLocalBytes)
{
  auto bytes = golden_direct();
  bytes.insert(bytes.begin(), std::byte{0x55});
  EXPECT_EQ(require_value(mapped::decode_publication_body(std::span<const std::byte>{bytes}.subspan(1),
                                                         record_offset, record_limit)), direct_publication());
}

TEST(PublicationMetadata, RejectsTruncationAndTrailingImmutableBytes)
{
  const auto bytes = golden_direct();
  for (std::size_t size = 0; size < bytes.size(); ++size)
  {
    expect_error(mapped::decode_publication_body(std::span<const std::byte>{bytes}.first(size),
                                                  record_offset, record_limit), ErrorCode::IoError);
  }
  auto extended = bytes;
  extended.push_back(std::byte{});
  expect_error(mapped::decode_publication_body(extended, record_offset, record_limit), ErrorCode::IoError);
}

TEST(PublicationMetadata, RejectsEveryNonzeroReservedOrPaddingByte)
{
  const std::array ranges{std::pair{52u, 56u}, std::pair{114u, 128u}, std::pair{134u, 136u},
                          std::pair{168u, 176u}, std::pair{179u, 184u}};
  for (const auto& [begin, end] : ranges)
  {
    for (auto offset = begin; offset < end; ++offset)
    {
      auto bytes = golden_direct();
      bytes[offset] = std::byte{1};
      expect_error(mapped::decode_publication_body(bytes, record_offset, record_limit), ErrorCode::IoError);
    }
  }
}

TEST(PublicationMetadata, RejectsNoncanonicalSlicesAndRedundantCounts)
{
  // Moving a slice into the fixed body, aliasing an array, or pointing it outside
  // the body is invalid even when its numeric offset could otherwise fit u64.
  for (const auto offset : {56u, 80u, 88u, 96u, 104u, 160u})
  {
    for (const auto value : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{8192}, maximum})
    {
      auto bytes = golden_direct();
      put_integer(bytes, offset, value, 8);
      if ((offset == 88 || offset == 104) && value == 1)
      {
        continue;
      }
      expect_error(mapped::decode_publication_body(bytes, record_offset, record_limit), ErrorCode::IoError);
    }
  }
  for (const auto count : {0u, 2u, 65536u, 0xffffffffu})
  {
    auto bytes = golden_direct();
    put_integer(bytes, 48, count, 4);
    expect_error(mapped::decode_publication_body(bytes, record_offset, record_limit), ErrorCode::IoError);
  }
}

TEST(PublicationMetadata, RejectsUnknownWireCodes)
{
  for (const auto offset : {6u, 112u, 130u, 132u})
  {
    for (const auto value : {std::uint64_t{0xffff}, std::uint64_t{0x100}})
    {
      auto bytes = golden_direct();
      put_integer(bytes, offset, value, 2);
      expect_error(mapped::decode_publication_body(bytes, record_offset, record_limit), ErrorCode::IoError);
    }
  }
  for (const auto offset : {6u, 112u, 130u})
  {
    auto bytes = golden_direct();
    put_integer(bytes, offset, 0, 2);
    expect_error(mapped::decode_publication_body(bytes, record_offset, record_limit), ErrorCode::IoError);
  }
}

TEST(PublicationMetadata, RejectsZeroRequiredIdentitiesAndCounters)
{
  const std::array fields{std::pair{0u, 2u}, std::pair{4u, 2u}, std::pair{8u, 4u},
                          std::pair{16u, 8u}, std::pair{24u, 8u}, std::pair{40u, 8u},
                          std::pair{128u, 2u}, std::pair{136u, 8u}};
  for (const auto& [offset, width] : fields)
  {
    auto bytes = golden_direct();
    put_integer(bytes, offset, 0, width);
    expect_error(mapped::decode_publication_body(bytes, record_offset, record_limit), ErrorCode::IoError);
  }
}

TEST(PublicationMetadata, RejectsForwardMisalignedAndZeroReferences)
{
  for (const auto reference : {std::uint64_t{0}, std::uint64_t{4097}, record_offset, maximum})
  {
    auto bytes = golden_direct();
    put_integer(bytes, 72, reference, 8);
    expect_error(mapped::decode_publication_body(bytes, record_offset, record_limit), ErrorCode::IoError);
  }
  for (const auto reference : {std::uint64_t{0}, std::uint64_t{4097}, record_offset, maximum})
  {
    auto bytes = golden_direct();
    put_integer(bytes, 144, reference, 8);
    expect_error(mapped::decode_publication_body(bytes, record_offset, record_limit), ErrorCode::IoError);
  }
}

TEST(PublicationMetadata, FirstQueueSequenceHasNoPredecessor)
{
  auto publication = direct_publication();
  publication._destinations.front()._queueSequence = 1;
  expect_error(mapped::encode_publication_body(publication, record_offset, record_limit), ErrorCode::InvalidArgument);
  publication._destinations.front()._previousDelivery = 0;
  const auto bytes = require_value(mapped::encode_publication_body(publication, record_offset, record_limit));
  EXPECT_EQ(require_value(mapped::decode_publication_body(bytes, record_offset, record_limit)), publication);
  publication._destinations.front()._queueSequence = 2;
  expect_error(mapped::encode_publication_body(publication, record_offset, record_limit), ErrorCode::InvalidArgument);
}

TEST(PublicationMetadata, EntirePredecessorRowMustPrecedeCurrentRecord)
{
  auto publication = direct_publication();
  publication._destinations.front()._previousDelivery = record_offset - 8;
  expect_error(mapped::encode_publication_body(publication, record_offset, record_limit), ErrorCode::InvalidArgument);
  auto bytes = golden_direct();
  put_integer(bytes, 144, record_offset - 8, 8);
  expect_error(mapped::decode_publication_body(bytes, record_offset, record_limit), ErrorCode::IoError);
  publication._destinations.front()._previousDelivery = record_offset - 48;
  const auto encoded = require_value(mapped::encode_publication_body(publication, record_offset, record_limit));
  EXPECT_EQ(require_value(mapped::decode_publication_body(encoded, record_offset, record_limit)), publication);
}

TEST(PublicationMetadata, CommittedAcceptanceCannotBeUnknownOrEntirelyRejected)
{
  auto publication = direct_publication();
  publication._destinations.front()._status = DestinationStatus::Unknown;
  expect_error(mapped::encode_publication_body(publication, record_offset, record_limit), ErrorCode::InvalidArgument);
  publication = partial_publication();
  publication._destinations.front() = publication._destinations.back();
  publication._destinations.front()._queue = QueueId{2};
  publication._destinations.front()._receiptReference = 8488;
  expect_error(mapped::encode_publication_body(publication, record_offset, 576), ErrorCode::InvalidArgument);
}

TEST(PublicationMetadata, RejectsDuplicateQueuesAndAllDestinationPartialOutcomes)
{
  auto publication = partial_publication();
  publication._destinations.back()._queue = publication._destinations.front()._queue;
  expect_error(mapped::encode_publication_body(publication, record_offset, 576), ErrorCode::InvalidArgument);
  auto bytes = golden_partial();
  put_integer(bytes, 176, 65535, 2);
  expect_error(mapped::decode_publication_body(bytes, record_offset, 576), ErrorCode::IoError);
  publication = partial_publication();
  publication._acceptance = AcceptancePolicy::AllDestinations;
  expect_error(mapped::encode_publication_body(publication, record_offset, 576), ErrorCode::InvalidArgument);
  bytes = golden_partial();
  put_integer(bytes, 112, 1, 2);
  expect_error(mapped::decode_publication_body(bytes, record_offset, 576), ErrorCode::IoError);
}

TEST(PublicationMetadata, DirectSendCannotMasqueradeAsPublishingSnapshot)
{
  auto publication = partial_publication();
  publication._publishingSet = PublishingQueueSetId::Invalid;
  expect_error(mapped::encode_publication_body(publication, record_offset, 576), ErrorCode::InvalidArgument);
  publication = direct_publication();
  publication._acceptance = AcceptancePolicy::Partial;
  expect_error(mapped::encode_publication_body(publication, record_offset, record_limit), ErrorCode::InvalidArgument);
}

TEST(PublicationMetadata, RejectedRowsHaveNoAcceptanceMetadata)
{
  for (const auto offset : {184u, 192u, 200u})
  {
    auto bytes = golden_partial();
    put_integer(bytes, offset, 1, 8);
    expect_error(mapped::decode_publication_body(bytes, record_offset, 576), ErrorCode::IoError);
  }
  for (const auto reason : {std::uint64_t{0}, std::uint64_t{19}, std::uint64_t{20}, std::uint64_t{65535}})
  {
    auto bytes = golden_partial();
    put_integer(bytes, 180, reason, 2);
    expect_error(mapped::decode_publication_body(bytes, record_offset, 576), ErrorCode::IoError);
  }
  auto bytes = golden_direct();
  put_integer(bytes, 132, 14, 2);
  expect_error(mapped::decode_publication_body(bytes, record_offset, record_limit), ErrorCode::IoError);
}

TEST(PublicationMetadata, InvalidTypedEnumsAndReceiptReferencesFailBeforeEncoding)
{
  auto publication = direct_publication();
  publication._format = static_cast<PayloadFormat>(999);
  expect_error(mapped::encode_publication_body(publication, record_offset, record_limit), ErrorCode::InvalidArgument);
  publication = direct_publication();
  publication._acceptance = static_cast<AcceptancePolicy>(999);
  expect_error(mapped::encode_publication_body(publication, record_offset, record_limit), ErrorCode::InvalidArgument);
  publication = direct_publication();
  publication._destinations.front()._reason = static_cast<ErrorCode>(999);
  expect_error(mapped::encode_publication_body(publication, record_offset, record_limit), ErrorCode::InvalidArgument);
  publication = direct_publication();
  publication._destinations.front()._receiptReference = 8448;
  expect_error(mapped::encode_publication_body(publication, record_offset, record_limit), ErrorCode::InvalidArgument);
}

TEST(PublicationMetadata, LayoutRejectsOverflowAndImpossibleWidthsWithoutAllocating)
{
  for (const auto count : {std::uint64_t{0}, std::uint64_t{65536}, maximum})
  {
    expect_error(mapped::plan_publication_layout(record_offset, count, 0, mapped::max_log_bytes), ErrorCode::InvalidArgument);
  }
  EXPECT_GT(require_value(mapped::plan_publication_layout(record_offset, 65535, 0, mapped::max_log_bytes))._totalBytes, 0u);
  for (const auto payload : {maximum, maximum - 175, mapped::max_log_bytes})
  {
    expect_error(mapped::plan_publication_layout(record_offset, 1, payload, mapped::max_log_bytes), ErrorCode::InvalidArgument);
    auto bytes = golden_direct();
    put_integer(bytes, 64, payload, 8);
    expect_error(mapped::decode_publication_body(bytes, record_offset, mapped::max_log_bytes), ErrorCode::IoError);
  }
  for (const auto limit : {std::uint64_t{0}, maximum})
  {
    expect_error(mapped::plan_publication_layout(record_offset, 1, 3, limit), ErrorCode::InvalidArgument);
  }
}

TEST(PublicationMetadata, RecordOffsetsRespectNativeAndHostBounds)
{
  for (const auto offset : {std::uint64_t{0}, std::uint64_t{4095}, std::uint64_t{8193},
                            std::uint64_t{8200}, mapped::max_log_bytes - 63, maximum})
  {
    expect_error(mapped::plan_publication_layout(offset, 1, 3, record_limit), ErrorCode::InvalidArgument);
    expect_error(mapped::decode_publication_body(golden_direct(), offset, record_limit), ErrorCode::IoError);
  }
  // The last full 64-aligned allocation below INT64_MAX is still representable.
  const auto offset = (mapped::max_log_bytes - record_limit) / 64 * 64;
  const auto layout = require_value(mapped::plan_publication_layout(offset, 1, 3, record_limit));
  auto publication = direct_publication();
  publication._destinations.front()._receiptReference = layout._receiptsOffset;
  const auto bytes = require_value(mapped::encode_publication_body(publication, offset, record_limit));
  EXPECT_EQ(require_value(mapped::decode_publication_body(bytes, offset, record_limit)), publication);
}

TEST(PublicationMetadata, BoundedMetadataMatchesIndependentGoldenBodyWithoutPayload)
{
  // Metadata readers receive three separate, bounded pieces. The three payload
  // bytes between rows and padding are intentionally never supplied to them.
  const auto golden = golden_direct();
  const auto fixed = std::span{golden}.first(128);
  const auto rows = std::span{golden}.subspan(128, 48);
  const auto padding = std::span{golden}.subspan(179, 5);
  const auto prefix = require_value(mapped::decode_publication_prefix(fixed, 184, record_offset, record_limit));
  EXPECT_EQ(prefix._header, static_cast<const mapped::PublicationHeader&>(direct_publication()));
  EXPECT_EQ(prefix._destinationCount, 1u);
  EXPECT_EQ(prefix._payloadBytes, 3u);
  EXPECT_EQ(prefix._layout, (mapped::PublicationLayout{8384, 8432, 8440, 184, 384}));
  const auto metadata = require_value(mapped::decode_publication_metadata(
      fixed, rows, padding, 184, record_offset, record_limit));
  EXPECT_EQ(static_cast<const mapped::PublicationHeader&>(metadata), prefix._header);
  EXPECT_EQ(metadata._destinations, direct_publication()._destinations);
  EXPECT_EQ(metadata._layout, prefix._layout);
  EXPECT_EQ(metadata._payloadBytes, 3u);
}

TEST(PublicationMetadata, LargeLogicalPayloadNeedsOnlyItsHeaderRowsAndAlignmentBytes)
{
  // This private shape fixture describes a 1 TiB + 3-byte opaque payload using
  // just 128 header bytes, 48 row bytes, and 5 alignment bytes. No large span,
  // vector, file, or payload allocation is constructed to represent that size.
  constexpr auto payloadBase = std::uint64_t{1} << 40;
  auto fixed = golden_direct();
  fixed.resize(128);
  put_integer(fixed, 64, payloadBase + 3, 8);
  put_integer(fixed, 96, payloadBase + 8440, 8);
  const auto body = golden_direct();
  std::vector<std::byte> rows(body.begin() + 128, body.begin() + 176);
  put_integer(rows, 32, payloadBase + 8440, 8);
  const std::array<std::byte, 5> padding{};
  const auto prefix = require_value(mapped::decode_publication_prefix(
      fixed, payloadBase + 184, record_offset, payloadBase + 384));
  EXPECT_EQ(prefix._layout._payloadOffset, 8432u);
  EXPECT_EQ(prefix._layout._receiptsOffset, payloadBase + 8440);
  EXPECT_EQ(prefix._payloadBytes, payloadBase + 3);
  const auto metadata = require_value(mapped::decode_publication_metadata(
      fixed, rows, padding, payloadBase + 184, record_offset, payloadBase + 384));
  ASSERT_EQ(metadata._destinations.size(), 1u);
  EXPECT_EQ(metadata._payloadBytes, payloadBase + 3);
  EXPECT_EQ(metadata._destinations.front()._receiptReference, payloadBase + 8440);
  expect_error(mapped::decode_publication_prefix(fixed, payloadBase + 184, record_offset,
                                                 payloadBase + 383), ErrorCode::IoError);
}

TEST(PublicationMetadata, BoundedMetadataRetainsPartialSnapshotChecks)
{
  const auto golden = golden_partial();
  const auto fixed = std::span{golden}.first(128);
  const auto rows = std::span{golden}.subspan(128, 96);
  const auto padding = std::span{golden}.subspan(227, 5);
  const auto metadata = require_value(mapped::decode_publication_metadata(
      fixed, rows, padding, 232, record_offset, 576));
  EXPECT_EQ(metadata._destinations, partial_publication()._destinations);
  EXPECT_EQ(metadata._payloadBytes, 3u);
  auto duplicate = std::vector<std::byte>{rows.begin(), rows.end()};
  put_integer(duplicate, 48, 65535, 2);
  expect_error(mapped::decode_publication_metadata(fixed, duplicate, padding, 232, record_offset, 576), ErrorCode::IoError);
}

TEST(PublicationMetadata, PrefixRejectsMalformedGeometryBeforeVariableMetadataReads)
{
  const auto golden = golden_direct();
  const auto fixed = std::span{golden}.first(128);
  for (std::size_t length = 0; length < fixed.size(); ++length)
  {
    expect_error(mapped::decode_publication_prefix(fixed.first(length), 184, record_offset, record_limit), ErrorCode::IoError);
  }
  expect_error(mapped::decode_publication_prefix(std::span{golden}.first(129), 184, record_offset, record_limit), ErrorCode::IoError);
  for (const auto immutableBytes : {std::uint64_t{0}, std::uint64_t{183}, std::uint64_t{185}, maximum})
  {
    expect_error(mapped::decode_publication_prefix(fixed, immutableBytes, record_offset, record_limit), ErrorCode::IoError);
  }
  for (const auto offset : {56u, 80u, 88u, 96u, 104u})
  {
    auto bad = std::vector<std::byte>{fixed.begin(), fixed.end()};
    put_integer(bad, offset, maximum, 8);
    expect_error(mapped::decode_publication_prefix(bad, 184, record_offset, record_limit), ErrorCode::IoError);
  }
  auto badCount = std::vector<std::byte>{fixed.begin(), fixed.end()};
  put_integer(badCount, 48, 65536, 4);
  expect_error(mapped::decode_publication_prefix(badCount, 184, record_offset, record_limit), ErrorCode::IoError);
  auto badIdentity = std::vector<std::byte>{fixed.begin(), fixed.end()};
  put_integer(badIdentity, 16, 0, 8);
  expect_error(mapped::decode_publication_prefix(badIdentity, 184, record_offset, record_limit), ErrorCode::IoError);
  auto badDirect = golden_partial();
  put_integer(badDirect, 2, 0, 2);
  expect_error(mapped::decode_publication_prefix(std::span{badDirect}.first(128), 232, record_offset, 576), ErrorCode::IoError);
}

TEST(PublicationMetadata, BoundedMetadataRequiresExactRowsAndZeroAlignmentPadding)
{
  const auto golden = golden_direct();
  const auto fixed = std::span{golden}.first(128);
  const auto rows = std::span{golden}.subspan(128, 48);
  const std::array<std::byte, 5> padding{};
  expect_error(mapped::decode_publication_metadata(fixed, rows.first(47), padding, 184, record_offset, record_limit), ErrorCode::IoError);
  expect_error(mapped::decode_publication_metadata(fixed, std::span{golden}.subspan(128, 49), padding,
                                                   184, record_offset, record_limit), ErrorCode::IoError);
  expect_error(mapped::decode_publication_metadata(fixed, rows, std::span{padding}.first(4),
                                                   184, record_offset, record_limit), ErrorCode::IoError);
  std::array<std::byte, 6> extraPadding{};
  expect_error(mapped::decode_publication_metadata(fixed, rows, extraPadding, 184, record_offset, record_limit), ErrorCode::IoError);
  for (std::size_t index = 0; index < padding.size(); ++index)
  {
    auto badPadding = padding;
    badPadding[index] = std::byte{1};
    expect_error(mapped::decode_publication_metadata(fixed, rows, badPadding, 184, record_offset, record_limit), ErrorCode::IoError);
  }
  auto wrongReceipt = std::vector<std::byte>{rows.begin(), rows.end()};
  put_integer(wrongReceipt, 32, 8448, 8);
  expect_error(mapped::decode_publication_metadata(fixed, wrongReceipt, padding, 184, record_offset, record_limit), ErrorCode::IoError);
  auto wrongTimestamp = std::vector<std::byte>{rows.begin(), rows.end()};
  put_integer(wrongTimestamp, 24, 0, 8);
  expect_error(mapped::decode_publication_metadata(fixed, wrongTimestamp, padding, 184, record_offset, record_limit), ErrorCode::IoError);
}

TEST(PublicationMetadata, EmptyPayloadMetadataStillRequiresZeroPayloadSlice)
{
  auto golden = golden_direct();
  golden.resize(176);
  put_integer(golden, 56, 0, 8);
  put_integer(golden, 64, 0, 8);
  put_integer(golden, 96, 8432, 8);
  put_integer(golden, 160, 8432, 8);
  const auto fixed = std::span{golden}.first(128);
  const auto rows = std::span{golden}.subspan(128, 48);
  const auto metadata = require_value(mapped::decode_publication_metadata(fixed, rows, {}, 176, record_offset, record_limit));
  EXPECT_EQ(metadata._payloadBytes, 0u);
  EXPECT_EQ(metadata._layout._payloadOffset, 0u);
  EXPECT_EQ(metadata._layout._receiptsOffset, 8432u);
  put_integer(golden, 56, 8432, 8);
  expect_error(mapped::decode_publication_prefix(fixed, 176, record_offset, record_limit), ErrorCode::IoError);
}
