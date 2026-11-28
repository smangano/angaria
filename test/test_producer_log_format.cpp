// Private format-3 byte fixtures, not runnable producer/consumer examples.
// These tests define representation and borrowing boundaries independently of
// the codecs. No encoded record or tail shape alone proves live acceptance.

#include "producer_log_format.h"
#include "result_assertions.h"
#include "wire_fixture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace recorded = angaria::detail::recorded;
  using angaria::test_support::expect_error;
  using angaria::test_support::put_integer;
  using angaria::test_support::require_value;

  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  constexpr std::uint32_t fixture_profile = 0x00010002;
  constexpr std::uint64_t fixture_offset = 8192;

  recorded::ProducerLogHeader fixture_header()
  {
    recorded::ProducerLogHeader header;
    header._session = {0x0807060504030201, 0x8877665544332211};
    header._producer = TaskId{65535};
    header._catalogRevision = 17;
    header._fileBytes = 16384;
    header._publicationCapacity = 64;
    header._liveAbiProfile = fixture_profile;
    return header;
  }

  std::array<std::byte, 256> golden_header()
  {
    // Immutable header only. The live committed-tail word at byte 256 is
    // deliberately absent; atomic snapshotting belongs to the later runtime.
    std::array<std::byte, 256> bytes{};
    const std::array<unsigned, 8> magic{0x41, 0x4e, 0x47, 0x41, 0x50, 0x4c, 0x4f, 0x47};
    for (std::size_t index = 0; index != magic.size(); ++index)
    {
      bytes[index] = static_cast<std::byte>(magic[index]);
    }
    put_integer(bytes, 8, 3, 2);
    put_integer(bytes, 12, 4096, 4);
    put_integer(bytes, 16, 0x0807060504030201);
    put_integer(bytes, 24, 0x8877665544332211);
    put_integer(bytes, 32, 65535, 2);
    put_integer(bytes, 40, 17);
    put_integer(bytes, 48, 16384);
    put_integer(bytes, 56, 64);
    put_integer(bytes, 64, 0x00010002, 4);
    put_integer(bytes, 68, 2, 2);
    put_integer(bytes, 70, 8, 2);
    put_integer(bytes, 72, 1);
    put_integer(bytes, 88, 4096);
    return bytes;
  }

  recorded::LogReference fixture_reference()
  {
    const auto header = fixture_header();
    return {header._session, header._producer, 8192, 8384};
  }

  std::array<std::byte, 48> golden_reference()
  {
    std::array<std::byte, 48> bytes{};
    put_integer(bytes, 0, 0x0807060504030201);
    put_integer(bytes, 8, 0x8877665544332211);
    put_integer(bytes, 16, 65535, 2);
    put_integer(bytes, 18, 1, 2);
    put_integer(bytes, 24, 8192);
    put_integer(bytes, 32, 8384);
    return bytes;
  }

  recorded::DestinationRecord accepted_destination()
  {
    recorded::DestinationRecord destination;
    destination._queue = QueueId{65535};
    destination._queueSequence = 42;
    destination._enqueuedAt = UtcTimestamp{std::chrono::nanoseconds{0}};
    return destination;
  }

  recorded::PublicationRecordInput fixture_publication(
      const recorded::ProducerLogHeader& log, std::span<const recorded::DestinationRecord> destinations,
      std::span<const std::byte> payload)
  {
    // The spans refer to each test's explicit storage. There is no hidden
    // application schema/Registry setup inside this local codec fixture.
    recorded::PublicationRecordInput publication;
    publication._header._schema = {MessageType{65534}, 0xffffffffu};
    publication._header._publication = {{log._session, log._producer, maximum}, maximum};
    publication._header._publishedAt = UtcTimestamp{std::chrono::nanoseconds{-7}};
    publication._header._payloadBytes = payload.size();
    publication._header._flags = 0xa55a33ccu;
    publication._recordOrdinal = 13;
    publication._schemaReference = 4096;
    publication._topologyRevision = 9;
    publication._destinations = destinations;
    publication._payload = payload;
    return publication;
  }

  std::vector<std::byte> golden_record()
  {
    // 128 fixed + 32 destination + 3 payload + 29 zero padding = 192.
    std::vector<std::byte> bytes(192);
    bytes[0] = std::byte{0x41};
    bytes[1] = std::byte{0x4e};
    bytes[2] = std::byte{0x50};
    bytes[3] = std::byte{0x52};
    put_integer(bytes, 4, 1, 2);
    put_integer(bytes, 6, 1, 2);
    put_integer(bytes, 8, 192);
    put_integer(bytes, 16, 8192);
    put_integer(bytes, 24, maximum);
    put_integer(bytes, 32, maximum);
    put_integer(bytes, 40, 0xfffffffffffffff9u);
    put_integer(bytes, 48, 17);
    put_integer(bytes, 56, 4096);
    put_integer(bytes, 64, 65534, 2);
    put_integer(bytes, 66, 1, 2);
    put_integer(bytes, 68, 0xffffffffu, 4);
    put_integer(bytes, 72, 0xa55a33ccu, 4);
    put_integer(bytes, 78, 1, 2);
    put_integer(bytes, 80, 9);
    put_integer(bytes, 88, 1, 4);
    put_integer(bytes, 96, 3);
    put_integer(bytes, 104, 13);
    put_integer(bytes, 112, 65535, 2);
    put_integer(bytes, 128, 65535, 2);
    put_integer(bytes, 130, 1, 2);
    put_integer(bytes, 136, 42);
    bytes[160] = std::byte{0xff};
    bytes[162] = std::byte{0x42};
    return bytes;
  }

  std::vector<std::byte> golden_partial_record()
  {
    auto bytes = golden_record();
    bytes.resize(256);
    std::fill(bytes.begin() + 160, bytes.end(), std::byte{});
    put_integer(bytes, 8, 256);
    put_integer(bytes, 76, 9, 2);
    put_integer(bytes, 78, 2, 2);
    put_integer(bytes, 88, 2, 4);
    put_integer(bytes, 160, 1, 2);
    put_integer(bytes, 162, 2, 2);
    put_integer(bytes, 164, 14, 2); // Explicit wire code for LogFull.
    bytes[192] = std::byte{0xff};
    bytes[194] = std::byte{0x42};
    return bytes;
  }
}

TEST(ProducerLogFormat, HeaderUsesIndependentBytesAndExcludesLiveControl)
{
  const auto header = fixture_header();
  const auto expected = golden_header();
  EXPECT_EQ(require_value(recorded::encode_producer_header(header)), expected);
  EXPECT_EQ(require_value(recorded::decode_producer_header(expected)), header);
  EXPECT_EQ(recorded::producer_header_bytes, 4096u);
  EXPECT_EQ(recorded::producer_immutable_bytes, 256u);
  EXPECT_EQ(recorded::committed_tail_offset, 256u);
  EXPECT_EQ(recorded::control_alignment, 256u);

  expect_error(recorded::decode_producer_header(std::span{expected}.first(255)), ErrorCode::IoError);
  std::vector<std::byte> withControl(expected.begin(), expected.end());
  withControl.resize(264);
  expect_error(recorded::decode_producer_header(withControl), ErrorCode::IoError);
}

TEST(ProducerLogFormat, StructuralHeaderDecodingDoesNotAdmitForeignLiveProfiles)
{
  auto expected = golden_header();
  put_integer(expected, 64, 0x12345678, 4);
  const auto header = require_value(recorded::decode_producer_header(expected));
  EXPECT_EQ(header._liveAbiProfile, 0x12345678u);
  expect_error(recorded::live_profile_compatibility(header, fixture_profile), ErrorCode::UnsupportedPolicy);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(
      recorded::live_profile_compatibility(header, 0x12345678)));
  expect_error(recorded::live_profile_compatibility(header, 0), ErrorCode::InvalidArgument);
}

TEST(ProducerLogFormat, UnsupportedHeaderVersionsAndFeaturesFailBeforeUse)
{
  struct Mutation
  {
    std::size_t _offset;
    std::uint64_t _value;
    std::size_t _width;
  };
  const std::array changes{Mutation{8, 2, 2}, Mutation{10, 1, 2}, Mutation{68, 1, 2},
                           Mutation{72, 3, 8}, Mutation{80, 1, 8}};
  for (const auto change : changes)
  {
    SCOPED_TRACE(change._offset);
    auto bytes = golden_header();
    put_integer(bytes, change._offset, change._value, change._width);
    expect_error(recorded::decode_producer_header(bytes), ErrorCode::UnsupportedPolicy);
  }
}

TEST(ProducerLogFormat, RejectsMalformedHeaderShapeAndInvalidInputCapacities)
{
  for (const auto offset : {std::size_t{0}, std::size_t{34}, std::size_t{96}, std::size_t{255}})
  {
    SCOPED_TRACE(offset);
    auto bytes = golden_header();
    bytes[offset] = std::byte{0xfe};
    expect_error(recorded::decode_producer_header(bytes), ErrorCode::IoError);
  }
  std::array<recorded::ProducerLogHeader, 8> invalid;
  invalid.fill(fixture_header());
  invalid[0]._session = {};
  invalid[1]._producer = TaskId::Invalid;
  invalid[2]._catalogRevision = 0;
  invalid[3]._fileBytes = 4096;
  invalid[4]._fileBytes = 16383;
  invalid[5]._publicationCapacity = 0;
  invalid[6]._publicationCapacity = 65; // Only 64 minimum records fit.
  invalid[7]._liveAbiProfile = 0;
  for (std::size_t index = 0; index != invalid.size(); ++index)
  {
    SCOPED_TRACE(index);
    expect_error(recorded::encode_producer_header(invalid[index]), ErrorCode::InvalidArgument);
  }

  auto oneHalf = fixture_header();
  oneHalf._session._high = 0;
  EXPECT_EQ(require_value(recorded::decode_producer_header(
                require_value(recorded::encode_producer_header(oneHalf)))), oneHalf);
}

TEST(ProducerLogFormat, CommittedTailValidationChecksShapeWithoutClaimingAcceptance)
{
  const auto header = fixture_header();
  // These values establish no committed history; only a future runtime can
  // acquire the atomic tail and prove each referenced record is accepted.
  for (const auto tail : {std::uint64_t{4096}, std::uint64_t{8192}, std::uint64_t{16384}})
  {
    EXPECT_TRUE(std::holds_alternative<std::monostate>(recorded::validate_committed_tail(header, tail)));
  }
  for (const auto tail : {std::uint64_t{0}, std::uint64_t{4095}, std::uint64_t{4097},
                           std::uint64_t{16385}, maximum})
  {
    SCOPED_TRACE(tail);
    expect_error(recorded::validate_committed_tail(header, tail), ErrorCode::IoError);
  }
}

TEST(ProducerLogFormat, CrossLogReferenceUsesIndependentBytes)
{
  const auto header = fixture_header();
  const auto expected = golden_reference();
  EXPECT_EQ(require_value(recorded::encode_log_reference(fixture_reference(), header)), expected);
  EXPECT_EQ(require_value(recorded::decode_log_reference(expected, header)), fixture_reference());
  expect_error(recorded::decode_log_reference(std::span{expected}.first(47), header), ErrorCode::IoError);
  std::vector<std::byte> extended(expected.begin(), expected.end());
  extended.push_back(std::byte{});
  expect_error(recorded::decode_log_reference(extended, header), ErrorCode::IoError);
}

TEST(ProducerLogFormat, RejectsWrongLogReservedBytesAndInvalidReferenceRanges)
{
  const auto header = fixture_header();
  for (const auto offset : {std::size_t{0}, std::size_t{16}, std::size_t{20}, std::size_t{47}})
  {
    SCOPED_TRACE(offset);
    auto bytes = golden_reference();
    bytes[offset] = std::byte{0x5a};
    expect_error(recorded::decode_log_reference(bytes, header), ErrorCode::IoError);
  }
  auto encoding = golden_reference();
  put_integer(encoding, 18, 2, 2);
  expect_error(recorded::decode_log_reference(encoding, header), ErrorCode::UnsupportedPolicy);

  std::array<recorded::LogReference, 7> invalid;
  invalid.fill(fixture_reference());
  invalid[0]._session = {};
  invalid[1]._producer = TaskId{1};
  invalid[2]._offset = 4032;
  invalid[3]._offset = 8193;
  invalid[4]._end = 8192;
  invalid[5]._end = 16385;
  invalid[6]._offset = maximum - 63;
  for (std::size_t index = 0; index != invalid.size(); ++index)
  {
    SCOPED_TRACE(index);
    expect_error(recorded::encode_log_reference(invalid[index], header), ErrorCode::InvalidArgument);
  }
}

TEST(ProducerLogFormat, PublicationEncodingMatchesGoldenRecordAndOnlyTouchesPlannedExtent)
{
  const auto header = fixture_header();
  const std::array destinations{accepted_destination()};
  const std::array payload{std::byte{0xff}, std::byte{}, std::byte{0x42}};
  const auto publication = fixture_publication(header, destinations, payload);
  const auto layout = require_value(recorded::plan_publication_record(publication, header, fixture_offset));
  EXPECT_EQ(layout._bytes, 192u);
  EXPECT_EQ(layout._end, 8384u);
  EXPECT_EQ(layout._payloadOffset, 160u);

  std::array<std::byte, 256> output;
  output.fill(std::byte{0xa5});
  EXPECT_EQ(require_value(recorded::encode_publication_record(publication, header, fixture_offset, output)),
            fixture_reference());
  const auto golden = golden_record();
  EXPECT_TRUE(std::equal(golden.begin(), golden.end(), output.begin()));
  EXPECT_TRUE(std::all_of(output.begin() + 192, output.end(),
                          [](std::byte byte)
                          {
                            return byte == std::byte{0xa5};
                          }));
}

TEST(ProducerLogFormat, DecodedViewPreservesHeaderAndBorrowsPayloadWithoutOwningIt)
{
  const auto header = fixture_header();
  const std::array destinations{accepted_destination()};
  const std::array payload{std::byte{0xff}, std::byte{}, std::byte{0x42}};
  const auto publication = fixture_publication(header, destinations, payload);
  auto bytes = golden_record();
  const auto view = require_value(recorded::decode_publication_record(bytes, header, fixture_reference()));
  EXPECT_EQ(view._header, publication._header);
  EXPECT_EQ(view._reference, fixture_reference());
  EXPECT_EQ(view._recordOrdinal, 13u);
  EXPECT_EQ(view._schemaReference, 4096u);
  EXPECT_EQ(view._topologyRevision, 9u);
  EXPECT_EQ(view._publishingSet, PublishingQueueSetId::Invalid);
  EXPECT_EQ(view._acceptance, AcceptancePolicy::AllDestinations);
  ASSERT_EQ(view.destination_count(), 1u);
  EXPECT_EQ(require_value(view.destination_at(0)), destinations[0]);
  expect_error(view.destination_at(1), ErrorCode::InvalidArgument);
  EXPECT_EQ(view._payload.data(), bytes.data() + 160);
  EXPECT_EQ(view._payload.size(), 3u);

  // Deliberately mutate this local fixture, never live committed storage, to
  // demonstrate the view's borrow while its header remains an owning value.
  bytes[160] = std::byte{0x7a};
  put_integer(bytes, 72, 0, 4);
  EXPECT_EQ(view._payload[0], std::byte{0x7a});
  EXPECT_EQ(view._header._flags, 0xa55a33ccu);
}

TEST(ProducerLogFormat, LocalRecordCopiesNeedNoNativePointerAlignment)
{
  const auto header = fixture_header();
  const std::array destinations{accepted_destination()};
  const std::array payload{std::byte{0xff}, std::byte{}, std::byte{0x42}};
  const auto publication = fixture_publication(header, destinations, payload);
  std::array<std::byte, 194> storage;
  storage.fill(std::byte{0xa5});
  const auto output = std::span{storage}.subspan(1, 192);

  // The logical log offset is aligned, but a portable local copy may begin
  // at any byte address. These codecs do not bind native atomic words.
  const auto reference = require_value(recorded::encode_publication_record(
      publication, header, fixture_offset, output));
  const auto golden = golden_record();
  EXPECT_TRUE(std::equal(golden.begin(), golden.end(), output.begin()));
  EXPECT_EQ(storage.front(), std::byte{0xa5});
  EXPECT_EQ(storage.back(), std::byte{0xa5});
  const auto decoded = require_value(recorded::decode_publication_record(output, header, reference));
  EXPECT_EQ(decoded._header, publication._header);
  EXPECT_EQ(decoded._payload.data(), storage.data() + 161);
  EXPECT_EQ(require_value(decoded.destination_at(0)), destinations[0]);
}

TEST(ProducerLogFormat, AllPayloadFormatsAndSignedTimestampValuesRoundTrip)
{
  const auto header = fixture_header();
  std::array destinations{accepted_destination()};
  const std::array payload{std::byte{0xff}, std::byte{}, std::byte{0x42}};
  for (const auto format : {PayloadFormat::Binary, PayloadFormat::Json, PayloadFormat::Xml,
                             PayloadFormat::Csv, PayloadFormat::Text})
  {
    for (const auto time : {std::int64_t{0}, std::int64_t{-1}, std::numeric_limits<std::int64_t>::min(),
                             std::numeric_limits<std::int64_t>::max()})
    {
      auto publication = fixture_publication(header, destinations, payload);
      publication._header._format = format;
      publication._header._publishedAt = UtcTimestamp{std::chrono::nanoseconds{time}};
      destinations[0]._enqueuedAt = UtcTimestamp{std::chrono::nanoseconds{time}};
      std::array<std::byte, 192> output{};
      const auto reference = require_value(recorded::encode_publication_record(
          publication, header, fixture_offset, output));
      const auto decoded = require_value(recorded::decode_publication_record(output, header, reference));
      EXPECT_EQ(decoded._header, publication._header);
      EXPECT_EQ(require_value(decoded.destination_at(0)), destinations[0]);
    }
  }
}

TEST(ProducerLogFormat, EmptyPayloadHasCanonicalPositionAndNoMutableReceiptSuffix)
{
  const auto header = fixture_header();
  const std::array destinations{accepted_destination()};
  const auto publication = fixture_publication(header, destinations, {});
  const auto layout = require_value(recorded::plan_publication_record(publication, header, fixture_offset));
  EXPECT_EQ(layout._bytes, 192u);
  EXPECT_EQ(layout._payloadOffset, 160u);
  std::array<std::byte, 192> output{};
  const auto reference = require_value(recorded::encode_publication_record(
      publication, header, fixture_offset, output));
  const auto decoded = require_value(recorded::decode_publication_record(output, header, reference));
  EXPECT_TRUE(decoded._payload.empty());
  EXPECT_EQ(decoded._header._payloadBytes, 0u);
  EXPECT_EQ(require_value(decoded.destination_at(0)), destinations[0]);
  EXPECT_TRUE(std::all_of(output.begin() + 160, output.end(),
                          [](std::byte byte)
                          {
                            return byte == std::byte{};
                          }));
}

TEST(ProducerLogFormat, PartialFanoutPreservesEveryAcceptedAndRejectedDestination)
{
  const auto header = fixture_header();
  recorded::DestinationRecord rejected;
  rejected._queue = QueueId{1};
  rejected._status = DestinationStatus::Rejected;
  rejected._reason = ErrorCode::LogFull;
  const std::array destinations{accepted_destination(), rejected};
  const std::array payload{std::byte{0xff}, std::byte{}, std::byte{0x42}};
  auto publication = fixture_publication(header, destinations, payload);
  publication._publishingSet = PublishingQueueSetId{9};
  publication._acceptance = AcceptancePolicy::Partial;
  std::array<std::byte, 256> output{};
  const auto reference = require_value(recorded::encode_publication_record(
      publication, header, fixture_offset, output));
  const auto golden = golden_partial_record();
  EXPECT_TRUE(std::equal(golden.begin(), golden.end(), output.begin()));
  EXPECT_EQ(reference._end, 8448u);
  const auto view = require_value(recorded::decode_publication_record(golden, header, reference));
  EXPECT_EQ(view._header, publication._header);
  EXPECT_EQ(view._publishingSet, PublishingQueueSetId{9});
  EXPECT_EQ(view._acceptance, AcceptancePolicy::Partial);
  ASSERT_EQ(view.destination_count(), 2u);
  EXPECT_EQ(require_value(view.destination_at(0)), destinations[0]);
  EXPECT_EQ(require_value(view.destination_at(1)), rejected);
  EXPECT_EQ(view._payload.data(), golden.data() + 192);
}

TEST(ProducerLogFormat, AllDestinationFanoutRequiresEveryRowToBeAccepted)
{
  const auto header = fixture_header();
  std::array destinations{accepted_destination(), accepted_destination()};
  destinations[1]._queue = QueueId{1};
  destinations[1]._queueSequence = 1;
  auto publication = fixture_publication(header, destinations, {});
  publication._publishingSet = PublishingQueueSetId{9};
  std::array<std::byte, 192> output{};
  const auto reference = require_value(recorded::encode_publication_record(
      publication, header, fixture_offset, output));
  EXPECT_EQ(require_value(recorded::decode_publication_record(output, header, reference)).destination_count(), 2u);

  destinations[1]._status = DestinationStatus::Rejected;
  destinations[1]._reason = ErrorCode::LogFull;
  destinations[1]._queueSequence = 0;
  expect_error(recorded::plan_publication_record(publication, header, fixture_offset), ErrorCode::InvalidArgument);
}

TEST(ProducerLogFormat, InvalidRequestAndSmallOutputLeaveTheDestinationUnchanged)
{
  const auto header = fixture_header();
  std::array destinations{accepted_destination()};
  const std::array payload{std::byte{0xff}, std::byte{}, std::byte{0x42}};
  auto publication = fixture_publication(header, destinations, payload);
  std::array<std::byte, 256> output;
  output.fill(std::byte{0x5a});
  const auto before = output;
  expect_error(recorded::encode_publication_record(publication, header, fixture_offset,
                                                    std::span{output}.first(191)),
               ErrorCode::OutputBufferTooSmall);
  EXPECT_EQ(output, before);
  publication._header._payloadBytes = 4;
  expect_error(recorded::encode_publication_record(publication, header, fixture_offset, output),
               ErrorCode::InvalidArgument);
  EXPECT_EQ(output, before);
  publication._header._payloadBytes = 3;
  destinations[0]._reason = ErrorCode::LogFull;
  expect_error(recorded::encode_publication_record(publication, header, fixture_offset, output),
               ErrorCode::InvalidArgument);
  EXPECT_EQ(output, before);
}

TEST(ProducerLogFormat, DeliveryMetadataCannotBeSilentlyDiscardedFromPublicationHeaders)
{
  const auto header = fixture_header();
  const std::array destinations{accepted_destination()};
  const auto original = fixture_publication(header, destinations, {});
  std::array<recorded::PublicationRecordInput, 5> invalid;
  invalid.fill(original);
  invalid[0]._header._queue = QueueId{1};
  invalid[1]._header._queueSequence = 1;
  invalid[2]._header._consumer = TaskClaim{header._session, TaskId{2}, 1};
  invalid[3]._header._enqueuedAt = UtcTimestamp{};
  invalid[4]._header._dequeuedAt = UtcTimestamp{};
  for (std::size_t index = 0; index != invalid.size(); ++index)
  {
    SCOPED_TRACE(index);
    expect_error(recorded::plan_publication_record(invalid[index], header, fixture_offset),
                 ErrorCode::InvalidArgument);
  }
}

TEST(ProducerLogFormat, RejectsDuplicateQueuesAndPublicationsWithNoAcceptance)
{
  const auto header = fixture_header();
  std::array destinations{accepted_destination(), accepted_destination()};
  auto publication = fixture_publication(header, destinations, {});
  publication._publishingSet = PublishingQueueSetId{9};
  publication._acceptance = AcceptancePolicy::Partial;
  expect_error(recorded::plan_publication_record(publication, header, fixture_offset), ErrorCode::InvalidArgument);

  destinations[1]._queue = QueueId{1};
  for (auto& destination : destinations)
  {
    destination._status = DestinationStatus::Rejected;
    destination._reason = ErrorCode::LogFull;
    destination._queueSequence = 0;
  }
  expect_error(recorded::plan_publication_record(publication, header, fixture_offset), ErrorCode::InvalidArgument);
  destinations[0] = accepted_destination();
  destinations[1]._reason = ErrorCode::OutcomeUnknown;
  expect_error(recorded::plan_publication_record(publication, header, fixture_offset), ErrorCode::InvalidArgument);
  destinations[1]._reason = ErrorCode::None;
  expect_error(recorded::plan_publication_record(publication, header, fixture_offset), ErrorCode::InvalidArgument);
  destinations[1]._reason = ErrorCode::LogFull;
  destinations[1]._enqueuedAt = UtcTimestamp{std::chrono::nanoseconds{-1}};
  expect_error(recorded::plan_publication_record(publication, header, fixture_offset), ErrorCode::InvalidArgument);
}

TEST(ProducerLogFormat, PublicationCountAndByteBoundsRejectWithoutOverflow)
{
  const auto header = fixture_header();
  const std::array destinations{accepted_destination()};
  auto publication = fixture_publication(header, destinations, {});
  publication._recordOrdinal = 65;
  expect_error(recorded::plan_publication_record(publication, header, fixture_offset), ErrorCode::LogFull);
  publication._recordOrdinal = 13;
  expect_error(recorded::plan_publication_record(publication, header, 16320), ErrorCode::LogFull);
  EXPECT_TRUE(std::holds_alternative<Error>(recorded::plan_publication_record(publication, header, maximum - 63)));
  expect_error(recorded::plan_publication_record(publication, header, 8193), ErrorCode::InvalidArgument);
}

TEST(ProducerLogFormat, SchemaReferenceNamesTheCatalogRatherThanTheProducerFile)
{
  const auto header = fixture_header();
  const std::array destinations{accepted_destination()};
  auto publication = fixture_publication(header, destinations, {});
  publication._schemaReference = 32768; // Beyond this producer file, in session.ang.
  std::array<std::byte, 192> bytes{};
  const auto reference = require_value(recorded::encode_publication_record(
      publication, header, fixture_offset, bytes));
  EXPECT_EQ(require_value(recorded::decode_publication_record(bytes, header, reference))._schemaReference, 32768u);
  publication._schemaReference = 4097;
  expect_error(recorded::plan_publication_record(publication, header, fixture_offset), ErrorCode::InvalidArgument);
}

TEST(ProducerLogFormat, MalformedRecordLengthsReferencesAndPaddingAreRejected)
{
  const auto header = fixture_header();
  const auto original = golden_record();
  const auto reference = fixture_reference();
  for (const auto count : {std::size_t{0}, std::size_t{127}, std::size_t{159}, std::size_t{191}})
  {
    SCOPED_TRACE(count);
    expect_error(recorded::decode_publication_record(std::span{original}.first(count), header, reference),
                 ErrorCode::IoError);
  }
  auto extended = original;
  extended.push_back(std::byte{});
  expect_error(recorded::decode_publication_record(extended, header, reference), ErrorCode::IoError);

  for (const auto offset : {std::size_t{0}, std::size_t{92}, std::size_t{114}, std::size_t{127},
                             std::size_t{134}, std::size_t{152}, std::size_t{191}})
  {
    SCOPED_TRACE(offset);
    auto bytes = original;
    bytes[offset] = std::byte{0x5a};
    expect_error(recorded::decode_publication_record(bytes, header, reference), ErrorCode::IoError);
  }

  struct Mutation
  {
    std::size_t _offset;
    std::uint64_t _value;
    std::size_t _width;
  };
  const std::array changes{Mutation{8, 256, 8}, Mutation{16, 8256, 8}, Mutation{24, 0, 8},
                           Mutation{32, 0, 8}, Mutation{48, 18, 8}, Mutation{56, 4097, 8},
                           Mutation{64, 0, 2}, Mutation{66, 65535, 2}, Mutation{68, 0, 4},
                           Mutation{80, 0, 8}, Mutation{88, 0, 4}, Mutation{96, maximum, 8},
                           Mutation{104, 0, 8}, Mutation{112, 1, 2}, Mutation{128, 0, 2},
                           Mutation{130, 3, 2}, Mutation{132, 14, 2}, Mutation{136, 0, 8}};
  for (const auto change : changes)
  {
    SCOPED_TRACE(change._offset);
    auto bytes = original;
    put_integer(bytes, change._offset, change._value, change._width);
    expect_error(recorded::decode_publication_record(bytes, header, reference), ErrorCode::IoError);
  }

  auto wrongLog = reference;
  wrongLog._session._low ^= 1;
  expect_error(recorded::decode_publication_record(original, header, wrongLog), ErrorCode::IoError);
  wrongLog = reference;
  wrongLog._producer = TaskId{1};
  expect_error(recorded::decode_publication_record(original, header, wrongLog), ErrorCode::IoError);
}

TEST(ProducerLogFormat, UnsupportedPublicationKindAndEncodingCannotBeAdmitted)
{
  const auto header = fixture_header();
  const auto reference = fixture_reference();
  // A valid producer header cannot admit an unknown record type or encoding.
  // They require a separately reviewed format capability, not shape guessing.
  for (const auto offset : {std::size_t{4}, std::size_t{6}})
  {
    SCOPED_TRACE(offset);
    auto bytes = golden_record();
    put_integer(bytes, offset, 2, 2);
    expect_error(recorded::decode_publication_record(bytes, header, reference), ErrorCode::UnsupportedPolicy);
  }
}

TEST(ProducerLogFormat, DecodingAlsoChecksFanoutUniquenessAndAcceptedSubset)
{
  const auto header = fixture_header();
  auto reference = fixture_reference();
  reference._end = 8448;
  auto duplicate = golden_partial_record();
  put_integer(duplicate, 160, 65535, 2);
  expect_error(recorded::decode_publication_record(duplicate, header, reference), ErrorCode::IoError);
  auto noAcceptance = golden_partial_record();
  put_integer(noAcceptance, 130, 2, 2);
  put_integer(noAcceptance, 132, 14, 2);
  put_integer(noAcceptance, 136, 0);
  expect_error(recorded::decode_publication_record(noAcceptance, header, reference), ErrorCode::IoError);
  auto allDestinations = golden_partial_record();
  put_integer(allDestinations, 78, 1, 2);
  expect_error(recorded::decode_publication_record(allDestinations, header, reference), ErrorCode::IoError);
}
