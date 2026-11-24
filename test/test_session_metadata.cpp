#include "session_metadata.h"

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
#include <variant>
#include <vector>

// These private codecs persist the metadata returned by Session/Registry APIs.
// Fixtures describe record bodies without creating a Session or claiming Tasks;
// public lifecycle usage is demonstrated in test_runtime_session.cpp instead.
namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;

  constexpr std::uint64_t record_offset = 8192;
  constexpr std::uint64_t body_offset = 8256;

  void put_integer(std::vector<std::byte>& bytes, std::size_t offset,
                   std::uint64_t value, std::size_t width)
  {
    for (std::size_t index = 0; index != width; ++index)
    {
      bytes[offset + index] = static_cast<std::byte>((value >> (8 * index)) & 0xff);
    }
  }

  void append_text(std::vector<std::byte>& bytes, const std::string& text)
  {
    for (const auto character : text)
    {
      bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
  }

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
  void expect_error(const Result<T>& result, ErrorCode code)
  {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    const auto& error = std::get<Error>(result);
    EXPECT_EQ(error._code, code);
    EXPECT_FALSE(error._path.empty());
    EXPECT_FALSE(error._message.empty());
  }

  mapped::SuperblockConfiguration configuration()
  {
    mapped::SuperblockConfiguration result;
    result._session = SessionId{1, 2};
    result._maxLogBytes = 65536;
    result._limits = CapacityLimits::empty().tasks(3);
    return result;
  }

  mapped::SessionDefinition session_definition()
  {
    mapped::SessionDefinition definition;
    definition._metadata._session = configuration()._session;
    definition._metadata._createdBy = static_cast<TaskId>(1);
    definition._metadata._createdAt = UtcTimestamp{std::chrono::nanoseconds{-7}};
    definition._metadata._angariaVersion = "0.2.4";
    definition._metadata._applicationVersion = "sim";
    definition._metadata._creatorUser = "u";
    definition._options._name = "S";
    definition._options._applicationVersion = "sim";
    definition._options._directory = "/d";
    definition._options._maxLogBytes = configuration()._maxLogBytes;
    definition._options._limits = configuration()._limits;
    definition._options._defaults._ringCapacity = 8;
    definition._policyFeatures = 0x1122334455667788;
    return definition;
  }

  mapped::TaskDefinition task_definition()
  {
    mapped::TaskDefinition definition;
    definition._id = static_cast<TaskId>(3);
    definition._descriptor._name = "task";
    definition._descriptor._doc = "doc";
    definition._registration._createdBy = static_cast<TaskId>(1);
    definition._registration._createdAt = UtcTimestamp{std::chrono::nanoseconds{-9}};
    definition._registration._revision = 7;
    return definition;
  }

  std::vector<std::byte> golden_session()
  {
    // Independently authored from the format table: 128 fixed bytes followed
    // by name, framework version, application version, user and original path.
    std::vector<std::byte> bytes(128);
    for (const auto offset : {0, 2, 4, 6, 8, 10})
    {
      put_integer(bytes, offset, 1, 2);
    }
    put_integer(bytes, 16, 0xfffffffffffffff9, 8);
    put_integer(bytes, 24, 8, 8);
    put_integer(bytes, 40, 0x1122334455667788, 8);
    const std::array<std::uint64_t, 5> offsets{8384, 8385, 8390, 8393, 8394};
    const std::array<std::uint64_t, 5> lengths{1, 5, 3, 1, 2};
    for (std::size_t index = 0; index != offsets.size(); ++index)
    {
      put_integer(bytes, 48 + 16 * index, offsets[index], 8);
      put_integer(bytes, 56 + 16 * index, lengths[index], 8);
    }
    append_text(bytes, "S0.2.4simu/d");
    return bytes;
  }

  std::vector<std::byte> golden_task()
  {
    // The Task key is widened to u64; common definition fields precede the
    // explicit Process code at byte 64. No native structure is copied here.
    std::vector<std::byte> bytes(72);
    put_integer(bytes, 0, 3, 8);
    put_integer(bytes, 8, 1, 2);
    put_integer(bytes, 16, 0xfffffffffffffff7, 8);
    put_integer(bytes, 24, 7, 8);
    put_integer(bytes, 32, 8328, 8);
    put_integer(bytes, 40, 4, 8);
    put_integer(bytes, 48, 8332, 8);
    put_integer(bytes, 56, 3, 8);
    put_integer(bytes, 64, 1, 2);
    append_text(bytes, "taskdoc");
    return bytes;
  }

  // Independent bytes document the Session body and its absolute string slices.
  // Start decoding one byte into a buffer to verify that storage encoding does
  // not depend on native structure alignment.
  TEST(SessionMetadata, SessionEncoderAndUnalignedDecoderMatchIndependentGolden)
  {
    EXPECT_EQ(require_value(mapped::encode_session_definition(session_definition(), record_offset)),
              golden_session());
    auto bytes = golden_session();
    bytes.insert(bytes.begin(), std::byte{0x7f});
    const auto result = require_value(mapped::decode_session_definition(
        std::span{bytes}.subspan(1), record_offset, configuration()));
    EXPECT_EQ(result._metadata._session, (SessionId{1, 2}));
    EXPECT_EQ(result._metadata._createdBy, static_cast<TaskId>(1));
    EXPECT_EQ(result._metadata._createdAt.time_since_epoch().count(), -7);
    EXPECT_EQ(result._metadata._angariaVersion, "0.2.4");
    EXPECT_EQ(result._metadata._applicationVersion, "sim");
    EXPECT_EQ(result._metadata._creatorUser, "u");
    EXPECT_EQ(result._options._name, "S");
    EXPECT_EQ(result._options._directory.generic_string(), "/d");
    EXPECT_EQ(result._options._maxLogBytes, 65536u);
    EXPECT_EQ(result._options._limits._tasks, 3u);
    EXPECT_EQ(result._options._defaults._ringCapacity, 8u);
    EXPECT_EQ(result._policyFeatures, 0x1122334455667788u);
  }

  // A Task's immutable registration metadata is separate from its live claim.
  // Decode its ID, descriptor, creator, timestamp, and revision without copying
  // a native C++ structure from the unaligned fixture.
  TEST(SessionMetadata, TaskEncoderAndUnalignedDecoderMatchIndependentGolden)
  {
    EXPECT_EQ(require_value(mapped::encode_task_definition(task_definition(), record_offset)), golden_task());
    auto bytes = golden_task();
    bytes.insert(bytes.begin(), std::byte{0x7f});
    const auto result = require_value(mapped::decode_task_definition(std::span{bytes}.subspan(1), record_offset));
    EXPECT_EQ(result._id, static_cast<TaskId>(3));
    EXPECT_EQ(result._descriptor._name, "task");
    EXPECT_EQ(result._descriptor._doc, "doc");
    EXPECT_EQ(result._descriptor._kind, TaskKind::Process);
    EXPECT_FALSE(result._descriptor._process);
    EXPECT_EQ(result._registration._createdBy, static_cast<TaskId>(1));
    EXPECT_EQ(result._registration._createdAt.time_since_epoch().count(), -9);
    EXPECT_EQ(result._registration._revision, 7u);
  }

  // Empty application versions/docs use zero offset-and-length slices. Up to
  // 63 zero padding bytes account for record alignment; extra or nonzero trailing
  // bytes are not silently accepted as another metadata field.
  TEST(SessionMetadata, EmptyOptionalStringsUseZeroSlicesAndAllowShortZeroPadding)
  {
    auto session = session_definition();
    session._options._applicationVersion.clear();
    session._metadata._applicationVersion.clear();
    auto body = require_value(mapped::encode_session_definition(session, record_offset));
    EXPECT_TRUE(std::all_of(body.begin() + 80, body.begin() + 96,
                            [](std::byte value)
                            {
                              return value == std::byte{};
                            }));
    body.resize(body.size() + 63);
    EXPECT_TRUE(require_value(mapped::decode_session_definition(body, record_offset, configuration()))
                    ._metadata._applicationVersion.empty());
    body.push_back(std::byte{});
    expect_error(mapped::decode_session_definition(body, record_offset, configuration()), ErrorCode::IoError);

    auto task = task_definition();
    task._descriptor._doc.clear();
    body = require_value(mapped::encode_task_definition(task, record_offset));
    EXPECT_TRUE(std::all_of(body.begin() + 48, body.begin() + 64,
                            [](std::byte value)
                            {
                              return value == std::byte{};
                            }));
    body.push_back(std::byte{});
    EXPECT_TRUE(require_value(mapped::decode_task_definition(body, record_offset))._descriptor._doc.empty());
    body.back() = std::byte{1};
    expect_error(mapped::decode_task_definition(body, record_offset), ErrorCode::IoError);
  }

  // Change stored fields directly to exercise malformed-input handling. A
  // truncated body, unknown code, or reserved data must fail before its options
  // could be exposed by public Session attachment.
  TEST(SessionMetadata, SessionRejectsTruncationReservedAndUnknownPolicyCodes)
  {
    for (const auto offset : {0, 2, 4, 6, 8, 10, 12, 15, 24})
    {
      auto body = golden_session();
      put_integer(body, offset, offset >= 12 && offset <= 15 ? 1 : 0, offset >= 12 && offset <= 15 ? 1 : 2);
      expect_error(mapped::decode_session_definition(body, record_offset, configuration()), ErrorCode::IoError);
    }
    for (const auto length : {0u, 16u, 127u, 139u})
    {
      auto body = golden_session();
      body.resize(length);
      expect_error(mapped::decode_session_definition(body, record_offset, configuration()), ErrorCode::IoError);
    }
    auto body = golden_session();
    put_integer(body, 6, 4, 2);
    expect_error(mapped::decode_session_definition(body, record_offset, configuration()), ErrorCode::IoError);
    body = golden_session();
    put_integer(body, 0, 4, 2);
    expect_error(mapped::decode_session_definition(body, record_offset, configuration()), ErrorCode::IoError);
  }

  // The body must describe a nonzero Task and registration, with a parent only
  // for Thread Tasks. This local codec checks the relationship's shape; checking
  // that the named parent is a registered Process Task belongs to the runtime.
  TEST(SessionMetadata, TaskRejectsInvalidKeysRegistrationKindAndParent)
  {
    for (const auto offset : {0, 8, 24, 64})
    {
      auto body = golden_task();
      put_integer(body, offset, 0, offset == 8 || offset == 64 ? 2 : 8);
      expect_error(mapped::decode_task_definition(body, record_offset), ErrorCode::IoError);
    }
    for (const auto offset : {10, 11, 15, 68, 71})
    {
      auto body = golden_task();
      body[offset] = std::byte{1};
      expect_error(mapped::decode_task_definition(body, record_offset), ErrorCode::IoError);
    }
    for (const auto key : {std::uint64_t{65536}, std::numeric_limits<std::uint64_t>::max()})
    {
      auto body = golden_task();
      put_integer(body, 0, key, 8);
      expect_error(mapped::decode_task_definition(body, record_offset), ErrorCode::IoError);
    }
    for (const auto values : {std::array{1, 1}, std::array{2, 0}, std::array{2, 3}, std::array{3, 0}})
    {
      auto body = golden_task();
      put_integer(body, 64, values[0], 2);
      put_integer(body, 66, values[1], 2);
      expect_error(mapped::decode_task_definition(body, record_offset), ErrorCode::IoError);
    }
    auto body = golden_task();
    body.resize(71);
    expect_error(mapped::decode_task_definition(body, record_offset), ErrorCode::IoError);
  }

  // String slices use logical file offsets but must stay inside their own
  // definition's canonical string area. Reject overlaps, empty nonzero slices,
  // and references into another record rather than reading unrelated bytes.
  TEST(SessionMetadata, SlicesMustBeCanonicalOwnedAndBounded)
  {
    for (const auto offset : {body_offset, std::uint64_t{8327}, std::uint64_t{8329},
                              std::numeric_limits<std::uint64_t>::max()})
    {
      auto body = golden_task();
      put_integer(body, 32, offset, 8);
      expect_error(mapped::decode_task_definition(body, record_offset), ErrorCode::IoError);
    }
    auto body = golden_task();
    put_integer(body, 40, std::numeric_limits<std::uint64_t>::max(), 8);
    expect_error(mapped::decode_task_definition(body, record_offset), ErrorCode::IoError);
    body = golden_task();
    put_integer(body, 48, 8328, 8); // Documentation overlaps the name.
    expect_error(mapped::decode_task_definition(body, record_offset), ErrorCode::IoError);
    body = golden_task();
    put_integer(body, 56, 0, 8); // An empty slice still has a nonzero offset.
    expect_error(mapped::decode_task_definition(body, record_offset), ErrorCode::IoError);
    body = golden_session();
    put_integer(body, 112, 4096, 8); // Directory points into another record.
    expect_error(mapped::decode_session_definition(body, record_offset, configuration()), ErrorCode::IoError);
  }

  // Names are nonempty UTF-8 without embedded NULs. Documentation is length
  // delimited and may contain NUL bytes; preserving it must not weaken name
  // validation or accept malformed UTF-8 in stored metadata.
  TEST(SessionMetadata, NamesAndStringsValidateUtf8WithoutLosingOptionalBytes)
  {
    const std::array<std::string, 8> invalid{
        std::string{}, std::string{"a\0b", 3}, std::string{"\xc0\x80", 2},
        std::string{"\xed\xa0\x80", 3}, std::string{"\xf4\x90\x80\x80", 4},
        std::string{"\xf0\x80\x80\x80", 4}, std::string{"\x80", 1}, std::string{"\xe2\x82", 2}};
    for (const auto& name : invalid)
    {
      auto task = task_definition();
      task._descriptor._name = name;
      expect_error(mapped::encode_task_definition(task, record_offset), ErrorCode::InvalidArgument);
      auto session = session_definition();
      session._options._name = name;
      expect_error(mapped::encode_session_definition(session, record_offset), ErrorCode::InvalidArgument);
    }
    auto body = golden_task();
    body[72] = std::byte{0xff};
    expect_error(mapped::decode_task_definition(body, record_offset), ErrorCode::IoError);
    body = golden_session();
    body[138] = std::byte{0xff};
    expect_error(mapped::decode_session_definition(body, record_offset, configuration()), ErrorCode::IoError);
    auto task = task_definition();
    task._descriptor._name = "\xf4\x8f\xbf\xbf";
    task._descriptor._doc = std::string{"a\0b", 3};
    const auto result = require_value(mapped::decode_task_definition(
        require_value(mapped::encode_task_definition(task, record_offset)), record_offset));
    EXPECT_EQ(result._descriptor._name, task._descriptor._name);
    EXPECT_EQ(result._descriptor._doc, task._descriptor._doc);
  }

  // Round-trip the full wire ranges, including UTC times before the epoch.
  // These boundary descriptors test serialization only, not a live parent claim
  // or a registration history containing those extreme values.
  TEST(SessionMetadata, ThreadSupportsMaximumIdsAndSignedNanosecondRange)
  {
    for (const auto timestamp : {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()})
    {
      auto task = task_definition();
      task._id = static_cast<TaskId>(65535);
      task._descriptor._kind = TaskKind::Thread;
      task._descriptor._process = static_cast<TaskId>(65534);
      task._registration._createdBy = static_cast<TaskId>(65535);
      task._registration._createdAt = UtcTimestamp{std::chrono::nanoseconds{timestamp}};
      task._registration._revision = std::numeric_limits<std::uint64_t>::max();
      const auto result = require_value(mapped::decode_task_definition(
          require_value(mapped::encode_task_definition(task, record_offset)), record_offset));
      EXPECT_EQ(result._id, task._id);
      EXPECT_EQ(result._descriptor._process, task._descriptor._process);
      EXPECT_EQ(result._registration._createdBy, task._registration._createdBy);
      EXPECT_EQ(result._registration._createdAt.time_since_epoch().count(), timestamp);
      EXPECT_EQ(result._registration._revision, task._registration._revision);
    }
  }

  // The caller supplies the owning record offset so absolute slices can be
  // checked before construction. Invalid placement is an argument error for
  // encoding and malformed storage for decoding.
  TEST(SessionMetadata, RecordOffsetsRejectMisalignmentAndSignedOffsetOverflow)
  {
    for (const auto offset : {std::uint64_t{0}, std::uint64_t{4095}, std::uint64_t{8193},
                              std::uint64_t{0x7fffffffffffffc0}, std::numeric_limits<std::uint64_t>::max()})
    {
      expect_error(mapped::encode_session_definition(session_definition(), offset), ErrorCode::InvalidArgument);
      expect_error(mapped::encode_task_definition(task_definition(), offset), ErrorCode::InvalidArgument);
      expect_error(mapped::decode_session_definition(golden_session(), offset, configuration()), ErrorCode::IoError);
      expect_error(mapped::decode_task_definition(golden_task(), offset), ErrorCode::IoError);
    }
  }

  // Portable inspection recognizes policy/storage codes beyond today's native
  // implementation. Storage mapping and segment size must agree with the header;
  // decoding policy codes does not enable them for live creation/attachment.
  TEST(SessionMetadata, PortablePolicyCodesAndSegmentedMetadataMatchHeader)
  {
    auto header = configuration();
    header._storage = StorageMapping::Segmented;
    header._segmentBytes = 8192;
    auto body = golden_session();
    put_integer(body, 2, 2, 2);
    put_integer(body, 4, 2, 2);
    put_integer(body, 6, 3, 2);
    put_integer(body, 8, 2, 2);
    put_integer(body, 10, 2, 2);
    put_integer(body, 32, 8192, 8);
    const auto result = require_value(mapped::decode_session_definition(body, record_offset, header));
    EXPECT_EQ(result._options._storage, StorageMapping::Segmented);
    EXPECT_EQ(result._options._segmentBytes, 8192u);
    EXPECT_EQ(result._options._defaults._delivery, DeliveryPolicy::AllowLoss);
    EXPECT_EQ(result._options._defaults._selection, SelectionPolicy::WeightedRandom);
    EXPECT_EQ(result._options._defaults._wait, WaitPolicy::EventFd);
    EXPECT_EQ(result._options._defaults._acceptance, AcceptancePolicy::Partial);
    EXPECT_EQ(result._policyFeatures, 0x1122334455667788u);
    EXPECT_EQ(require_value(mapped::encode_session_definition(result, record_offset)), body);
    expect_error(mapped::decode_session_definition(body, record_offset, configuration()), ErrorCode::IoError);
    header._segmentBytes = 16384;
    expect_error(mapped::decode_session_definition(body, record_offset, header), ErrorCode::IoError);
    header = configuration();
    header._segmentBytes = 8192;
    body = golden_session();
    put_integer(body, 32, 8192, 8);
    expect_error(mapped::decode_session_definition(body, record_offset, header), ErrorCode::IoError);
  }

  // Reject contradictory owning values before generating bytes, including an
  // application version that differs between options and metadata. Native Session
  // creation additionally checks which recognized policies are implemented.
  TEST(SessionMetadata, EncoderRejectsInconsistentNativeValuesAndUnknownEnums)
  {
    auto session = session_definition();
    session._metadata._applicationVersion = "different";
    expect_error(mapped::encode_session_definition(session, record_offset), ErrorCode::InvalidArgument);
    session = session_definition();
    session._metadata._session = {};
    expect_error(mapped::encode_session_definition(session, record_offset), ErrorCode::InvalidArgument);
    session = session_definition();
    session._options._defaults._ringCapacity = std::numeric_limits<std::uint64_t>::max();
    expect_error(mapped::encode_session_definition(session, record_offset), ErrorCode::InvalidArgument);
    session = session_definition();
    session._options._segmentBytes = 0;
    expect_error(mapped::encode_session_definition(session, record_offset), ErrorCode::InvalidArgument);
    session = session_definition();
    session._options._defaults._selection = static_cast<SelectionPolicy>(99);
    expect_error(mapped::encode_session_definition(session, record_offset), ErrorCode::InvalidArgument);
    session = session_definition();
    session._metadata._angariaVersion.clear();
    expect_error(mapped::encode_session_definition(session, record_offset), ErrorCode::InvalidArgument);
    auto task = task_definition();
    task._descriptor._kind = static_cast<TaskKind>(99);
    expect_error(mapped::encode_task_definition(task, record_offset), ErrorCode::InvalidArgument);
    task = task_definition();
    task._descriptor._process = TaskId::Invalid;
    expect_error(mapped::encode_task_definition(task, record_offset), ErrorCode::InvalidArgument);
  }
}
