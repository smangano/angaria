// Canonical record tests use public value snapshots and hand-authored bytes.
// A framed record preserves immutable message metadata; it neither authenticates
// its producer nor establishes Queue acceptance, delivery, or acknowledgment.
#include "angaria/message_record.h"
#include "angaria/runtime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace fs = std::filesystem;

  constexpr SessionId sample_session{0x0807060504030201, 0x8877665544332211};
  constexpr SchemaId sample_schema{static_cast<MessageType>(0xabcd), 0x89abcdef};

  template <typename T>
  T require_value(Result<T> result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    return std::get<T>(std::move(result));
  }

  template <typename T>
  void expect_error(const Result<T>& result, ErrorCode code, std::string_view path = {})
  {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    const auto& error = std::get<Error>(result);
    EXPECT_EQ(error._code, code) << error._path << ": " << error._message;
    EXPECT_FALSE(error._message.empty());
    EXPECT_FALSE(error._path.empty());
    if (!path.empty())
    {
      EXPECT_NE(error._path.find(path), std::string::npos) << error._path;
    }
  }

  std::vector<std::byte> octets(std::initializer_list<unsigned> values)
  {
    std::vector<std::byte> result;
    for (const auto value : values)
    {
      if (value > 255)
      {
        throw std::runtime_error("Invalid hand-authored golden byte");
      }
      result.push_back(static_cast<std::byte>(value));
    }
    return result;
  }

  MessageHeader sample_header(std::uint64_t payloadBytes = 3)
  {
    MessageHeader header;
    header._schema = sample_schema;
    header._publication = {{sample_session, static_cast<TaskId>(0x1234), 0x1817161514131211},
                           0xf8f7f6f5f4f3f2f1};
    header._publicationTime = UtcTimestamp{std::chrono::nanoseconds{-2}};
    header._flags = 0xdeadbeef;
    header._payloadBytes = payloadBytes;
    return header;
  }

  std::vector<std::byte> golden_header()
  {
    // This literal follows the published 80-byte wire table, independently of
    // production constants, integer writers, native padding, or the encoder.
    return octets({
        0x41, 0x4e, 0x47, 0x41, 0x4d, 0x53, 0x47, 0x00,
        0x01, 0x00, 0x50, 0x00, 0x01, 0x00, 0x34, 0x12,
        0xcd, 0xab, 0x00, 0x00, 0xef, 0xcd, 0xab, 0x89,
        0xef, 0xbe, 0xad, 0xde, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
        0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  }

  std::vector<std::byte> golden_payload()
  {
    // Structural framing deliberately does not interpret these application
    // bytes. Binary semantic checks are exercised separately below.
    return octets({0x00, 0xff, 0x5a});
  }

  std::vector<std::byte> golden_record()
  {
    auto bytes = golden_header();
    const auto payload = golden_payload();
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
  }

  void put_integer(std::span<std::byte> bytes, std::size_t offset,
                     std::uint64_t value, std::size_t width)
  {
    // This helper only creates deliberately malformed inputs; expected valid
    // header bytes above never come from an integer encoder.
    for (std::size_t index = 0; index < width; ++index)
    {
      bytes[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
    }
  }

  RegistrySnapshot scalar_definitions(EFieldBase base = EFieldBase::BASE_INT16)
  {
    RegistrySnapshot result;
    result._revision = 1;
    result._currentRevision = 1;
    FieldDescriptor field;
    field._id = static_cast<FieldId>(0xee000001);
    field._name = "Value";
    field._base = base;
    field._size = 2;
    const RegistrationMetadata registration{TaskId{1}, UtcTimestamp{}, 1};
    result._fields.push_back({field, registration});
    MessageDescriptor message;
    message._schema = sample_schema;
    message._name = "Sample";
    message._fields = {field._id};
    result._messages.push_back({message, registration});
    return result;
  }

  BinaryPayloadCodec scalar_codec()
  {
    return require_value(BinaryPayloadCodec::from_snapshot(scalar_definitions(), sample_schema, 2));
  }

  template <typename T>
  concept HasTemporaryView = requires(T&& value)
  {
    std::move(value).view();
  };

  static_assert(!HasTemporaryView<MessageRecord>);

  TEST(MessageRecordTest, EncodesAndDecodesIndependentGoldenHeaderAndRecordBytes)
  {
    const auto header = sample_header();
    const auto expected_header = golden_header();
    ASSERT_EQ(expected_header.size(), 80u);
    const auto encoded_header = require_value(MessageRecordCodec::encode_header(header, 83));
    EXPECT_EQ((std::vector<std::byte>{encoded_header.begin(), encoded_header.end()}), expected_header);
    EXPECT_EQ(require_value(MessageRecordCodec::decode_header(expected_header, 83)), header);
    EXPECT_EQ(require_value(MessageRecordCodec::encode(header, golden_payload(), 83)), golden_record());

    // Decode the independent fixture, rather than the production encoder's
    // output. Every identity component, timestamp, flag, and payload is retained.
    const auto decoded = require_value(MessageRecordCodec::decode(golden_record(), 83));
    EXPECT_EQ(decoded._header, header);
    EXPECT_EQ(decoded._payload, golden_payload());

    // Records can begin inside a larger byte buffer. Neither header nor
    // payload decoding may assume a naturally aligned native integer address.
    const auto expected_record = golden_record();
    std::vector<std::byte> unaligned(1 + expected_record.size(), std::byte{0xff});
    std::copy(expected_record.begin(), expected_record.end(), unaligned.begin() + 1);
    const auto borrowed = require_value(MessageRecordCodec::decode_view(std::span{unaligned}.subspan(1), 83));
    EXPECT_EQ(borrowed._header, header);
    EXPECT_TRUE(std::equal(borrowed._payload.begin(), borrowed._payload.end(), decoded._payload.begin(), decoded._payload.end()));
  }

  TEST(MessageRecordTest, EveryStablePayloadFormatPreservesOpaqueBytes)
  {
    const std::array formats{PayloadFormat::Binary, PayloadFormat::Json, PayloadFormat::Xml,
                             PayloadFormat::Csv, PayloadFormat::Text};
    for (std::size_t index = 0; index < formats.size(); ++index)
    {
      SCOPED_TRACE(index);
      auto header = sample_header();
      header._format = formats[index];
      auto expected = golden_record();
      expected[12] = static_cast<std::byte>(index + 1);
      EXPECT_EQ(require_value(MessageRecordCodec::encode(header, golden_payload(), 83)), expected);
      const auto decoded = require_value(MessageRecordCodec::decode(expected, 83));
      EXPECT_EQ(decoded._header._format, formats[index]);
      EXPECT_EQ(decoded._payload, golden_payload());
    }
    // Recognizing JSON/XML/etc. is framing support, not parsing or validating
    // those formats. The opaque payload even contains NUL and a non-text byte.
  }

  TEST(MessageRecordTest, PreservesFullTimestampAndApplicationFlagRanges)
  {
    struct TimestampCase
    {
      std::int64_t _time;
      std::array<std::byte, 8> _bytes;
    };
    const std::array<TimestampCase, 4> cases{{
        {0, {}},
        {-1, {std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
              std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}}},
        {std::numeric_limits<std::int64_t>::min(), {std::byte{}, std::byte{}, std::byte{}, std::byte{},
                                                   std::byte{}, std::byte{}, std::byte{}, std::byte{0x80}}},
        {std::numeric_limits<std::int64_t>::max(), {std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
                                                   std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x7f}}}}};
    for (const auto& test : cases)
    {
      SCOPED_TRACE(test._time);
      for (const auto flags : {std::uint32_t{0}, std::numeric_limits<std::uint32_t>::max()})
      {
        auto header = sample_header();
        header._publicationTime = UtcTimestamp{std::chrono::nanoseconds{test._time}};
        header._flags = flags;
        auto expected = golden_header();
        std::copy(test._bytes.begin(), test._bytes.end(), expected.begin() + 64);
        std::fill(expected.begin() + 24, expected.begin() + 28, flags == 0 ? std::byte{} : std::byte{0xff});
        const auto encoded = require_value(MessageRecordCodec::encode_header(header, 83));
        EXPECT_EQ((std::vector<std::byte>{encoded.begin(), encoded.end()}), expected);
        EXPECT_EQ(require_value(MessageRecordCodec::decode_header(expected, 83)), header);
      }
    }
  }

  TEST(MessageRecordTest, IdentityShapeRequiresNonzeroComponentsButAllowsEitherSessionHalfToBeZero)
  {
    std::vector<MessageHeader> invalid;
    auto header = sample_header();
    header._publication._producer._session = {};
    invalid.push_back(header);
    header = sample_header();
    header._publication._producer._task = TaskId::Invalid;
    invalid.push_back(header);
    header = sample_header();
    header._publication._producer._incarnation = 0;
    invalid.push_back(header);
    header = sample_header();
    header._publication._sequence = 0;
    invalid.push_back(header);
    header = sample_header();
    header._schema._type = MessageType::Invalid;
    invalid.push_back(header);
    header = sample_header();
    header._schema._version = 0;
    invalid.push_back(header);
    header = sample_header();
    header._format = static_cast<PayloadFormat>(0xffff);
    invalid.push_back(header);
    for (const auto& test : invalid)
    {
      expect_error(MessageRecordCodec::encode_header(test, 83), ErrorCode::InvalidArgument);
      expect_error(MessageRecordCodec::encode(test, golden_payload(), 83), ErrorCode::InvalidArgument);
    }
    for (const auto session : {SessionId{0, 1}, SessionId{1, 0}})
    {
      header = sample_header();
      header._publication._producer._session = session;
      header._publication._producer._task = static_cast<TaskId>(0xffff);
      header._publication._producer._incarnation = std::numeric_limits<std::uint64_t>::max();
      header._publication._sequence = std::numeric_limits<std::uint64_t>::max();
      header._schema = {static_cast<MessageType>(0xffff), std::numeric_limits<std::uint32_t>::max()};
      const auto encoded = require_value(MessageRecordCodec::encode_header(header, 83));
      EXPECT_EQ(require_value(MessageRecordCodec::decode_header(encoded, 83)), header);
    }
  }

  TEST(MessageRecordTest, RecordBudgetsIncludeTheHeaderAndDoNotReserveTheMaximum)
  {
    const auto header = sample_header();
    const auto bytes = golden_record();
    expect_error(MessageRecordCodec::encode_header(header, 82), ErrorCode::CapacityExceeded);
    expect_error(MessageRecordCodec::decode_header(golden_header(), 82), ErrorCode::CapacityExceeded);
    expect_error(MessageRecordCodec::encode(header, golden_payload(), 82), ErrorCode::CapacityExceeded);
    expect_error(MessageRecordCodec::decode_view(bytes, 82), ErrorCode::CapacityExceeded);
    expect_error(MessageRecordCodec::decode(bytes, 82), ErrorCode::CapacityExceeded);
    EXPECT_EQ(require_value(MessageRecordCodec::encode(header, golden_payload(), 83)), bytes);
    const auto generous = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(require_value(MessageRecordCodec::encode(header, golden_payload(), generous)).size(), 83u);
    EXPECT_EQ(require_value(MessageRecordCodec::decode(bytes, generous))._payload.size(), 3u);

    auto empty = sample_header(0);
    const auto control = require_value(MessageRecordCodec::encode(empty, {}, 80));
    EXPECT_EQ(control.size(), 80u);
    EXPECT_TRUE(require_value(MessageRecordCodec::decode(control, 80))._payload.empty());
    expect_error(MessageRecordCodec::encode(empty, {}, 79), ErrorCode::CapacityExceeded);
    expect_error(MessageRecordCodec::decode(control, 79), ErrorCode::CapacityExceeded);
  }

  TEST(MessageRecordTest, HeaderOnlyFramingPreflightsAdvertisedLengthsWithoutAllocatingPayloads)
  {
    const auto header = sample_header();
    const auto prefix = golden_header();
    EXPECT_EQ(require_value(MessageRecordCodec::decode_header(prefix, 83)), header);
    // A header advertises the complete record bound even before a streaming
    // caller obtains its payload. The codec itself never allocates that payload.
    auto oversized = sample_header(std::numeric_limits<std::uint64_t>::max());
    expect_error(MessageRecordCodec::encode_header(oversized, std::numeric_limits<std::uint64_t>::max()),
                 ErrorCode::InvalidArgument);
    auto corrupt = prefix;
    put_integer(corrupt, 72, std::numeric_limits<std::uint64_t>::max(), 8);
    expect_error(MessageRecordCodec::decode_header(corrupt, std::numeric_limits<std::uint64_t>::max()),
                 ErrorCode::InvalidArgument);
    const auto impossible = static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max());
    oversized._payloadBytes = impossible;
    expect_error(MessageRecordCodec::encode_header(oversized, std::numeric_limits<std::uint64_t>::max()),
                 ErrorCode::CapacityExceeded);
    put_integer(corrupt, 72, impossible, 8);
    expect_error(MessageRecordCodec::decode_header(corrupt, std::numeric_limits<std::uint64_t>::max()),
                 ErrorCode::CapacityExceeded);
  }

  TEST(MessageRecordTest, RejectsEveryTruncatedHeaderAndRequiresItsExactLength)
  {
    const auto bytes = golden_record();
    for (std::size_t length = 0; length < 80; ++length)
    {
      SCOPED_TRACE(length);
      expect_error(MessageRecordCodec::decode_header(std::span{bytes}.first(length), 83), ErrorCode::InvalidArgument);
    }
    for (const auto length : {81u, 82u, 83u})
    {
      expect_error(MessageRecordCodec::decode_header(std::span{bytes}.first(length), 83), ErrorCode::InvalidArgument);
    }
    EXPECT_EQ(require_value(MessageRecordCodec::decode_header(std::span{bytes}.first(80), 83)), sample_header());
  }

  TEST(MessageRecordTest, RejectsMalformedMagicVersionsHeaderSizesFormatsAndReservedBytes)
  {
    auto header = golden_header();
    header[0] = std::byte{};
    expect_error(MessageRecordCodec::decode_header(header, 83), ErrorCode::InvalidArgument);
    for (const auto version : {0u, 2u, 0xffffu})
    {
      auto changed = golden_header();
      put_integer(changed, 8, version, 2);
      expect_error(MessageRecordCodec::decode_header(changed, 83), ErrorCode::UnsupportedPolicy);
    }
    for (const auto size : {0u, 79u, 81u, 0xffffu})
    {
      auto changed = golden_header();
      put_integer(changed, 10, size, 2);
      expect_error(MessageRecordCodec::decode_header(changed, 83), ErrorCode::InvalidArgument);
    }
    for (const auto format : {0u, 6u, 0xffffu})
    {
      auto changed = golden_header();
      put_integer(changed, 12, format, 2);
      expect_error(MessageRecordCodec::decode_header(changed, 83), ErrorCode::InvalidArgument);
    }
    for (const auto offset : {18u, 19u, 28u, 29u, 30u, 31u})
    {
      auto changed = golden_header();
      changed[offset] = std::byte{1};
      expect_error(MessageRecordCodec::decode_header(changed, 83), ErrorCode::InvalidArgument);
    }
  }

  TEST(MessageRecordTest, InvalidWireIdentityComponentsAreNotUsableHeaders)
  {
    for (const auto& mutation : {std::pair{14u, 2u}, std::pair{16u, 2u}, std::pair{20u, 4u},
                                 std::pair{48u, 8u}, std::pair{56u, 8u}})
    {
      auto changed = golden_header();
      put_integer(changed, mutation.first, 0, mutation.second);
      expect_error(MessageRecordCodec::decode_header(changed, 83), ErrorCode::InvalidArgument);
    }
    auto changed = golden_header();
    std::fill(changed.begin() + 32, changed.begin() + 48, std::byte{});
    expect_error(MessageRecordCodec::decode_header(changed, 83), ErrorCode::InvalidArgument);
  }

  TEST(MessageRecordTest, PayloadLengthsAreExactAndConcatenatedRecordsNeedTheirOwnFraming)
  {
    const auto header = sample_header();
    expect_error(MessageRecordCodec::encode(header, octets({0, 0xff}), 83), ErrorCode::InvalidArgument);
    expect_error(MessageRecordCodec::encode(header, octets({0, 0xff, 0x5a, 0}), 84), ErrorCode::InvalidArgument);
    const auto bytes = golden_record();
    for (std::size_t length = 0; length < bytes.size(); ++length)
    {
      SCOPED_TRACE(length);
      expect_error(MessageRecordCodec::decode_view(std::span{bytes}.first(length), 83), ErrorCode::InvalidArgument);
      expect_error(MessageRecordCodec::decode(std::span{bytes}.first(length), 83), ErrorCode::InvalidArgument);
    }
    auto extra = bytes;
    extra.push_back(std::byte{});
    expect_error(MessageRecordCodec::decode(extra, 84), ErrorCode::InvalidArgument);
    extra = bytes;
    extra.insert(extra.end(), bytes.begin(), bytes.end());
    expect_error(MessageRecordCodec::decode_view(extra, 166), ErrorCode::InvalidArgument);
    for (const auto size : {0u, 2u, 4u})
    {
      auto changed = bytes;
      put_integer(changed, 72, size, 8);
      expect_error(MessageRecordCodec::decode(changed, 84), ErrorCode::InvalidArgument);
    }
    auto overflow = bytes;
    put_integer(overflow, 72, std::numeric_limits<std::uint64_t>::max(), 8);
    expect_error(MessageRecordCodec::decode(overflow, std::numeric_limits<std::uint64_t>::max()),
                 ErrorCode::InvalidArgument);
  }

  TEST(MessageRecordTest, CallerOutputChangesOnlyAfterCompleteValidation)
  {
    auto header = sample_header();
    const auto payload = golden_payload();
    std::vector<std::byte> output(83, std::byte{0xa5});
    const auto before = output;
    header._schema._version = 0;
    expect_error(MessageRecordCodec::encode_into(header, payload, output, 83), ErrorCode::InvalidArgument);
    EXPECT_EQ(output, before);
    header = sample_header();
    expect_error(MessageRecordCodec::encode_into(header, payload, output, 82), ErrorCode::CapacityExceeded);
    EXPECT_EQ(output, before);
    expect_error(MessageRecordCodec::encode_into(header, std::span{payload}.first(2), output, 83), ErrorCode::InvalidArgument);
    EXPECT_EQ(output, before);
    expect_error(MessageRecordCodec::encode_into(header, payload, std::span{output}.first(82), 83), ErrorCode::InvalidArgument);
    EXPECT_EQ(output, before);
    std::vector<std::byte> larger(84, std::byte{0xa5});
    const auto larger_before = larger;
    expect_error(MessageRecordCodec::encode_into(header, payload, larger, 84), ErrorCode::InvalidArgument);
    EXPECT_EQ(larger, larger_before);
    require_value(MessageRecordCodec::encode_into(header, payload, output, 83));
    EXPECT_EQ(output, golden_record());
  }

  TEST(MessageRecordTest, CallerOutputCanOverlapTheBorrowedInputPayload)
  {
    const auto header = sample_header();
    const auto payload = golden_payload();
    for (const auto offset : {0u, 79u, 80u})
    {
      SCOPED_TRACE(offset);
      std::vector<std::byte> output(83, std::byte{0xa5});
      std::copy(payload.begin(), payload.end(), output.begin() + offset);
      const auto borrowed = std::span<const std::byte>{output}.subspan(offset, 3);
      require_value(MessageRecordCodec::encode_into(header, borrowed, output, 83));
      EXPECT_EQ(output, golden_record());
    }
  }

  TEST(MessageRecordTest, BorrowedViewsKeepHeaderValuesButBorrowOnlyThePayload)
  {
    auto bytes = golden_record();
    const auto record = require_value(MessageRecordCodec::decode_view(bytes, 83));
    EXPECT_EQ(record._header, sample_header());
    EXPECT_EQ(record._payload.data(), bytes.data() + 80);
    EXPECT_EQ(record._payload.size(), 3u);
    const auto message = record.message();
    EXPECT_EQ(message._schema, sample_schema);
    EXPECT_EQ(message._format, PayloadFormat::Binary);
    EXPECT_EQ(message._flags, 0xdeadbeefu);
    EXPECT_EQ(message._payload.data(), record._payload.data());

    // The view's header is an ordinary value. Its payload still belongs to the
    // source vector, so source byte changes remain visible through that span.
    bytes[24] = std::byte{};
    bytes[80] = std::byte{0x7f};
    EXPECT_EQ(record._header._flags, 0xdeadbeefu);
    EXPECT_EQ(record._payload[0], std::byte{0x7f});
  }

  TEST(MessageRecordTest, OwningDecodeAndCopiesOutliveTheirEncodedInput)
  {
    auto owned = []
    {
      auto bytes = golden_record();
      auto result = require_value(MessageRecordCodec::decode(bytes, 83));
      std::fill(bytes.begin(), bytes.end(), std::byte{});
      return result;
    }();
    EXPECT_EQ(owned._header, sample_header());
    EXPECT_EQ(owned._payload, golden_payload());
    const auto borrowed = owned.view();
    EXPECT_EQ(borrowed._payload.data(), owned._payload.data());
    EXPECT_EQ(borrowed.message()._schema, sample_schema);
    auto copy = owned;
    EXPECT_NE(copy._payload.data(), owned._payload.data());
    copy._payload[0] = std::byte{0x7f};
    EXPECT_EQ(owned._payload[0], std::byte{});
    auto moved = std::move(owned);
    EXPECT_EQ(moved._header, sample_header());
    EXPECT_EQ(moved._payload, golden_payload());
  }

  TEST(MessageRecordTest, BinaryDecodingRequiresTheExactSchemaEvenWhenWidthsMatch)
  {
    auto definitions = scalar_definitions();
    auto newer = definitions._messages.front();
    newer._descriptor._schema._version += 1;
    definitions._messages.push_back(newer);
    const auto first = require_value(BinaryPayloadCodec::from_snapshot(definitions, sample_schema, 2));
    const auto second = require_value(BinaryPayloadCodec::from_snapshot(definitions, newer._descriptor._schema, 2));
    const auto payload = octets({0xff, 0xff});
    const MessageRecordView record{sample_header(2), payload};
    EXPECT_EQ(require_value(MessageRecordCodec::decode_binary(record, first, sample_session)),
              (std::vector<PayloadValue>{std::int16_t{-1}}));
    EXPECT_EQ(require_value(second.decode(payload)), (std::vector<PayloadValue>{std::int16_t{-1}}));
    expect_error(MessageRecordCodec::decode_binary(record, second, sample_session), ErrorCode::SchemaConflict);
  }

  TEST(MessageRecordTest, BinarySchemaContextIsAnIndependentSessionIdentity)
  {
    // Distinct Sessions can reuse every local Field and schema ID. This second
    // public snapshot intentionally gives the same two bytes another meaning.
    constexpr SessionId other_session{9, 10};
    const auto signed_codec = scalar_codec();
    const auto unsigned_codec = require_value(BinaryPayloadCodec::from_snapshot(
        scalar_definitions(EFieldBase::BASE_UINT16), sample_schema, 2));
    const auto payload = octets({0xff, 0xff});
    const MessageRecordView record{sample_header(2), payload};
    EXPECT_EQ(require_value(unsigned_codec.decode(payload)),
              (std::vector<PayloadValue>{std::numeric_limits<std::uint16_t>::max()}));
    expect_error(MessageRecordCodec::decode_binary(record, unsigned_codec, other_session), ErrorCode::SchemaConflict);
    expect_error(MessageRecordCodec::decode_binary(record, signed_codec, {}), ErrorCode::InvalidArgument,
                 "message_record.schema_session");
    EXPECT_EQ(require_value(MessageRecordCodec::decode_binary(record, signed_codec, sample_session)),
              (std::vector<PayloadValue>{std::int16_t{-1}}));
  }

  TEST(MessageRecordTest, BinaryDecodingChecksManualViewsAndMovedCodecs)
  {
    auto codec = scalar_codec();
    const auto payload = octets({0xff, 0xff});
    const MessageRecordView valid{sample_header(2), payload};
    for (const auto format : {PayloadFormat::Json, PayloadFormat::Xml, PayloadFormat::Csv, PayloadFormat::Text})
    {
      auto record = valid;
      record._header._format = format;
      expect_error(MessageRecordCodec::decode_binary(record, codec, sample_session), ErrorCode::UnsupportedPolicy);
    }
    auto malformed = valid;
    malformed._header._payloadBytes = 1;
    expect_error(MessageRecordCodec::decode_binary(malformed, codec, sample_session), ErrorCode::InvalidArgument);
    malformed = valid;
    malformed._header._publication._producer._task = TaskId::Invalid;
    expect_error(MessageRecordCodec::decode_binary(malformed, codec, sample_session), ErrorCode::InvalidArgument);
    malformed = valid;
    malformed._header._format = static_cast<PayloadFormat>(0xffff);
    expect_error(MessageRecordCodec::decode_binary(malformed, codec, sample_session), ErrorCode::InvalidArgument);

    auto moved = std::move(codec);
    expect_error(MessageRecordCodec::decode_binary(valid, codec, sample_session), ErrorCode::InvalidState);
    EXPECT_EQ(require_value(MessageRecordCodec::decode_binary(valid, moved, sample_session)),
              (std::vector<PayloadValue>{std::int16_t{-1}}));
  }

  TEST(MessageRecordTest, StructuralBinaryFramingDoesNotBypassPayloadValidation)
  {
    auto definitions = scalar_definitions();
    auto& field = definitions._fields.front()._descriptor;
    field._base = EFieldBase::BASE_BYTE;
    field._size = 1;
    field._logical._kind = FieldLogicalKind::Boolean;
    const auto codec = require_value(BinaryPayloadCodec::from_snapshot(definitions, sample_schema, 1));
    auto header = sample_header(1);
    const auto bytes = require_value(MessageRecordCodec::encode(header, octets({2}), 81));
    const auto framed = require_value(MessageRecordCodec::decode(bytes, 81));
    EXPECT_EQ(framed._payload, octets({2}));
    expect_error(MessageRecordCodec::decode_binary(framed.view(), codec, sample_session), ErrorCode::InvalidArgument,
                 "payload");
    // Keep a manual view's backing bytes alive for the complete operation.
    const auto two_bytes = octets({0, 0});
    const MessageRecordView sized{sample_header(2), two_bytes};
    expect_error(MessageRecordCodec::decode_binary(sized, codec, sample_session), ErrorCode::InvalidArgument,
                 "payload.size");
  }

  class MessageRecordRegistryTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-message-record-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    CreatedSession create() const
    {
      SessionOptions options;
      options._name = "Canonical records";
      options._directory = _root / "session";
      options._maxLogBytes = 1048576;
      options._limits = CapacityLimits::empty().tasks(2).fields(4).message_schemas(4);
      TaskDescriptor setup;
      setup._name = "Setup";
      return require_value(Session::create(options, setup));
    }

    fs::path _root;
  };

  // Here metadata comes from a real persistent Registry, unlike the fabricated
  // public snapshots above. Historical schema and Session context remain exact
  // after all original handles close and a replacement actor takes the claim.
  TEST_F(MessageRecordRegistryTest, HistoricalRecordMeaningSurvivesSchemaAndClaimReplacement)
  {
    RegistrySnapshot historical;
    RegistrySnapshot latest;
    SessionId session;
    PublicationId publication;
    TaskId setup;
    const SchemaId first{static_cast<MessageType>(42), 1};
    const SchemaId second{first._type, 2};
    {
      auto created = create();
      session = created._session.identity();
      setup = created._creator.claim()._task;
      auto registry = created._creator.registry();
      FieldDescriptor side;
      side._id = static_cast<FieldId>(0xee000001);
      side._name = "Side";
      side._base = EFieldBase::BASE_BYTE;
      side._size = 1;
      side._logical._kind = FieldLogicalKind::Enumeration;
      side._logical._enumerators = {{"Buy", std::uint64_t{1}}, {"Sell", std::uint64_t{2}}};
      require_value(registry.register_field(side));
      MessageDescriptor order;
      order._schema = first;
      order._name = "Order";
      order._fields = {side._id};
      require_value(registry.register_message(order));
      historical = require_value(created._session.registry().snapshot());
      side._id = static_cast<FieldId>(0xee000002);
      side._name = "SideV2";
      side._logical._enumerators = {{"Buy", std::uint64_t{1}}, {"Auction", std::uint64_t{2}},
                                    {"Sell", std::uint64_t{3}}};
      require_value(registry.register_field(side));
      order._schema = second;
      order._fields = {side._id};
      require_value(registry.register_message(order));
      latest = require_value(created._session.registry().snapshot());
      // Issuance reserves an identity, not a delivery or accepted publication.
      publication = require_value(created._creator.next_publication_id());
    }
    auto old_codec = require_value(BinaryPayloadCodec::from_snapshot(historical, first, 1));
    auto new_codec = require_value(BinaryPayloadCodec::from_snapshot(latest, second, 1));
    expect_error(BinaryPayloadCodec::from_snapshot(historical, second, 1), ErrorCode::NotFound);
    {
      auto attached = require_value(Session::attach(_root / "session"));
      auto replacement = require_value(attached.attach_task(setup));
      EXPECT_GT(replacement.claim()._incarnation, publication._producer._incarnation);
    }
    auto header = sample_header(1);
    header._schema = first;
    header._publication = publication;
    const auto payload = require_value(old_codec.encode({EnumValue{std::uint8_t{2}}}));
    const auto bytes = require_value(MessageRecordCodec::encode(header, payload, 81));
    const auto record = require_value(MessageRecordCodec::decode(bytes, 81));
    EXPECT_EQ(require_value(MessageRecordCodec::decode_binary(record.view(), old_codec, session)),
              (std::vector<PayloadValue>{EnumValue{std::uint8_t{2}}}));
    EXPECT_EQ(old_codec.fields()[0]._logical._enumerators[1]._name, "Sell");
    EXPECT_EQ(new_codec.fields()[0]._logical._enumerators[1]._name, "Auction");
    EXPECT_EQ(old_codec.payload_size(), new_codec.payload_size());
    expect_error(MessageRecordCodec::decode_binary(record.view(), new_codec, session), ErrorCode::SchemaConflict);
  }
} // namespace
