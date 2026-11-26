// Portable byte-format examples for the private operation journal (see docs/concurrency.md).
// These tests teach the persisted representation and rejection rules; applications
// use Session/Task APIs rather than constructing journal commands themselves.

#include "wire_fixture.h"
#include "result_helpers.h"
#include "journal_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;

  using angaria::test_support::require_value;

  template <typename T>
  void expect_error(const Result<T>& result, ErrorCode code = ErrorCode::IoError)
  {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    const auto& error = std::get<Error>(result);
    EXPECT_EQ(error._code, code);
    EXPECT_FALSE(error._path.empty());
    EXPECT_FALSE(error._message.empty());
  }

  void put_word(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value)
  {
    angaria::test_support::put_integer(bytes, offset, value);
  }

  mapped::JournalHeader fixed_header()
  {
    mapped::JournalHeader header;
    header._control = mapped::JournalControl::Prepared;
    header._generation = 0x0102030405060708;
    header._baseTail = 4096;
    header._newTail = 4096;
    header._baseAccounted = 4096;
    header._newAccounted = 4096;
    header._inlineBytes = 24;
    header._actor = static_cast<TaskId>(1);
    header._actorIncarnation = 9;
    header._originalFileBytes = 4096;
    header._newFileBytes = 4096;
    header._baseRevision = 7;
    header._newRevision = 7;
    return header;
  }

  mapped::JournalHeaderBytes fixed_fixture()
  {
    // Independent sparse golden fixture for every nonzero byte of the header.
    mapped::JournalHeaderBytes bytes{};
    for (const auto [offset, value] : std::array<std::pair<std::size_t, unsigned>, 21>{
             {{0, 1}, {8, 8}, {9, 7}, {10, 6}, {11, 5}, {12, 4}, {13, 3},
              {14, 2}, {15, 1}, {16, 2}, {18, 1}, {25, 0x10}, {33, 0x10},
              {41, 0x10}, {49, 0x10}, {64, 24}, {72, 1}, {80, 9},
              {105, 0x10}, {113, 0x10}, {136, 7}}})
    {
      bytes[offset] = static_cast<std::byte>(value);
    }
    bytes[144] = std::byte{7};
    return bytes;
  }

  std::vector<std::byte> command_fixture()
  {
    return {std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
            std::byte{8}, std::byte{0}, std::byte{0}, std::byte{0},
            std::byte{0}, std::byte{0x10}, std::byte{0}, std::byte{0},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
            std::byte{0x88}, std::byte{0x77}, std::byte{0x66}, std::byte{0x55},
            std::byte{0x44}, std::byte{0x33}, std::byte{0x22}, std::byte{0x11}};
  }

  mapped::RedoCommand write_command()
  {
    const auto fixture = command_fixture();
    return {mapped::RedoOpcode::WriteRange, 4096, {fixture.begin() + 16, fixture.end()}};
  }

  mapped::Manifest manifest()
  {
    mapped::Manifest value;
    value._actor = static_cast<TaskId>(1);
    value._actorIncarnation = 3;
    value._baseRevision = 4;
    value._newRevision = 5;
    value._records = {4096, 4224};
    value._commands = {write_command()};
    return value;
  }

  std::vector<std::byte> manifest_fixture()
  {
    std::vector<std::byte> bytes(104);
    bytes[0] = std::byte{1};
    bytes[2] = std::byte{1};
    bytes[8] = std::byte{3};
    bytes[16] = std::byte{4};
    bytes[24] = std::byte{5};
    bytes[32] = std::byte{0x80};
    bytes[33] = std::byte{0x20};
    bytes[40] = std::byte{2};
    bytes[48] = std::byte{0x90};
    bytes[49] = std::byte{0x20};
    bytes[56] = std::byte{24};
    bytes[65] = std::byte{0x10};
    bytes[72] = std::byte{0x80};
    bytes[73] = std::byte{0x10};
    const auto command = command_fixture();
    std::copy(command.begin(), command.end(), bytes.begin() + 80);
    return bytes;
  }

  // An independently written byte fixture catches shared encoder/decoder mistakes.
  // The displaced input also demonstrates that decoding bytes needs no native struct alignment.
  TEST(JournalFormat, FixedHeaderMatchesIndependentGoldenBytesAndUnalignedDecode)
  {
    const auto expected = fixed_fixture();
    EXPECT_EQ(require_value(mapped::encode_journal_header(fixed_header())), expected);
    EXPECT_EQ(require_value(mapped::decode_journal_header(expected)), fixed_header());
    std::vector<std::byte> unaligned(257);
    std::copy(expected.begin(), expected.end(), unaligned.begin() + 1);
    EXPECT_EQ(require_value(mapped::decode_journal_header(std::span{unaligned}.subspan(1))), fixed_header());
  }

  // A damaged or newer unsupported header must fail before recovery interprets it.
  // Corrupt one field at a time so each reserved-byte and stable-code boundary stays visible.
  TEST(JournalFormat, HeaderRejectsTruncationFlagsReservedAndUnknownCodes)
  {
    const auto fixture = fixed_fixture();
    expect_error(mapped::decode_journal_header(std::span{fixture}.first(255)));
    std::vector<std::byte> oversized(257);
    expect_error(mapped::decode_journal_header(oversized));
    for (const auto offset : {20u, 68u, 74u, 88u, 132u, 160u, 255u})
    {
      auto invalid = fixture;
      invalid[offset] = std::byte{1};
      expect_error(mapped::decode_journal_header(invalid));
    }
    for (const auto [offset, value] : {std::pair{0u, 4u}, {16u, 3u}, {18u, 2u}})
    {
      auto invalid = fixture;
      invalid[offset] = static_cast<std::byte>(value);
      expect_error(mapped::decode_journal_header(invalid));
    }
  }

  // FixedState changes reserved words without allocating history or advancing Registry revision.
  // A header mixing in append effects would break that full-log progress guarantee.
  TEST(JournalFormat, FixedStateCannotDescribeAppendSideEffects)
  {
    for (const auto offset : {32u, 48u, 56u, 96u, 112u, 120u, 128u, 144u, 152u})
    {
      auto invalid = fixed_fixture();
      put_word(invalid, offset, 8192);
      expect_error(mapped::decode_journal_header(invalid));
    }
    auto invalid = fixed_header();
    invalid._generation = 0;
    expect_error(mapped::encode_journal_header(invalid), ErrorCode::InvalidArgument);
    invalid = fixed_header();
    invalid._actorIncarnation = 0;
    expect_error(mapped::encode_journal_header(invalid), ErrorCode::InvalidArgument);
    invalid = fixed_header();
    invalid._inlineBytes = 1800;
    expect_error(mapped::encode_journal_header(invalid), ErrorCode::InvalidArgument);
    invalid = fixed_header();
    invalid._baseTail = std::numeric_limits<std::uint64_t>::max();
    expect_error(mapped::encode_journal_header(invalid), ErrorCode::InvalidArgument);
  }

  // An Append identifies its complete new allocation and final manifest before commit.
  // The manifest must belong to that bounded batch; inline commands belong to FixedState instead.
  TEST(JournalFormat, AppendHeaderRequiresBoundedBatchAndManifest)
  {
    auto header = fixed_header();
    header._kind = mapped::JournalKind::Append;
    header._inlineBytes = 0;
    header._newTail = 4480;
    header._newAccounted = 4480;
    header._newFileBytes = 4480;
    header._manifest = 4352;
    header._recordCount = 2;
    header._firstRecord = 4096;
    header._newRevision = 8;
    EXPECT_EQ(require_value(mapped::decode_journal_header(require_value(mapped::encode_journal_header(header)))), header);
    auto invalid = header;
    invalid._manifest = invalid._newTail;
    expect_error(mapped::encode_journal_header(invalid), ErrorCode::InvalidArgument);
    invalid = header;
    invalid._firstRecord = 4088;
    expect_error(mapped::encode_journal_header(invalid), ErrorCode::InvalidArgument);
    invalid = header;
    invalid._recordCount = 0;
    expect_error(mapped::encode_journal_header(invalid), ErrorCode::InvalidArgument);
    invalid = header;
    invalid._inlineBytes = 8;
    expect_error(mapped::encode_journal_header(invalid), ErrorCode::InvalidArgument);
  }

  // WriteRange stores an absolute target and replacement bytes, allowing replay to repeat it.
  // The known bytes document its frame independently of a successful encode/decode round trip.
  TEST(JournalFormat, WriteCommandMatchesIndependentGoldenBytes)
  {
    const std::array commands{write_command()};
    EXPECT_EQ(require_value(mapped::encode_commands(commands)), command_fixture());
    EXPECT_EQ(require_value(mapped::decode_commands(command_fixture())),
              std::vector<mapped::RedoCommand>{write_command()});
    EXPECT_TRUE(require_value(mapped::encode_commands({})).empty());
    EXPECT_TRUE(require_value(mapped::decode_commands({})).empty());
  }

  // A command cannot authorize an unaligned, overflowing, or incomplete write.
  // Decoding persisted corruption reports IoError; rejecting a newly supplied command reports InvalidArgument.
  TEST(JournalFormat, CommandsRejectMalformedHeadersRangesAndLengths)
  {
    const auto fixture = command_fixture();
    expect_error(mapped::decode_commands(std::span{fixture}.first(15)));
    expect_error(mapped::decode_commands(std::span{fixture}.first(23)));
    for (const auto [offset, value] : {std::pair{0u, 9u}, {2u, 1u}, {4u, 0u}, {4u, 16u}, {8u, 1u}})
    {
      auto invalid = fixture;
      invalid[offset] = static_cast<std::byte>(value);
      expect_error(mapped::decode_commands(invalid));
    }
    auto invalid = fixture;
    put_word(invalid, 8, std::numeric_limits<std::uint64_t>::max() - 7);
    expect_error(mapped::decode_commands(invalid));
    auto command = write_command();
    command._payload.pop_back();
    expect_error(mapped::encode_commands(std::array{command}), ErrorCode::InvalidArgument);
    command = write_command();
    command._target = 0;
    expect_error(mapped::encode_commands(std::array{command}), ErrorCode::InvalidArgument);
  }

  // Compact commands encode domain intent in fixed-size payloads instead of one write per affected row.
  // This checks framing and signed timestamp bits only; recognizing an opcode does not implement its policy.
  TEST(JournalFormat, CompactCommandPayloadsRoundTripWithSignedTimes)
  {
    std::vector<mapped::RedoCommand> commands{write_command()};
    auto& reset = commands.emplace_back(mapped::RedoCommand{mapped::RedoOpcode::ResetBudgets, 8192,
                                                           std::vector<std::byte>(24)});
    reset._payload[0] = std::byte{2};
    put_word(reset._payload, 8, 12288);
    put_word(reset._payload, 16, 7);
    auto& loss = commands.emplace_back(mapped::RedoCommand{mapped::RedoOpcode::LoseRange, 16384,
                                                          std::vector<std::byte>(32)});
    put_word(loss._payload, 0, 1);
    put_word(loss._payload, 8, 5);
    put_word(loss._payload, 16, std::numeric_limits<std::uint64_t>::max());
    loss._payload[24] = std::byte{2};
    auto& release = commands.emplace_back(mapped::RedoCommand{mapped::RedoOpcode::ReleaseClaim, 20480,
                                                             std::vector<std::byte>(16)});
    put_word(release._payload, 0, 3);
    put_word(release._payload, 8, std::uint64_t{1} << 63);
    EXPECT_EQ(require_value(mapped::decode_commands(require_value(mapped::encode_commands(commands)))), commands);
    for (std::size_t index = 1; index < commands.size(); ++index)
    {
      auto invalid = commands[index];
      invalid._payload.pop_back();
      expect_error(mapped::encode_commands(std::array{invalid}), ErrorCode::InvalidArgument);
      invalid = commands[index];
      put_word(invalid._payload, 0, 0);
      expect_error(mapped::encode_commands(std::array{invalid}), ErrorCode::InvalidArgument);
    }
    auto invalid = commands[1];
    invalid._payload[2] = std::byte{1};
    expect_error(mapped::encode_commands(std::array{invalid}), ErrorCode::InvalidArgument);
    invalid = commands[2];
    invalid._payload[26] = std::byte{1};
    expect_error(mapped::encode_commands(std::array{invalid}), ErrorCode::InvalidArgument);
  }

  // The envelope locates a record body and records its generation, revision, and readiness.
  // Ready describes materialization; the enclosing journal commit still determines acceptance.
  TEST(JournalFormat, RecordEnvelopeMatchesIndependentGoldenBytes)
  {
    mapped::RecordEnvelope envelope;
    envelope._kind = 6;
    envelope._totalBytes = 192;
    envelope._immutableBytes = 128;
    envelope._generation = 0x0102030405060708;
    envelope._previous = 4096;
    envelope._registryRevision = 3;
    envelope._readiness = 1;
    mapped::RecordEnvelopeBytes expected{};
    expected[0] = std::byte{6};
    expected[2] = std::byte{1};
    expected[4] = std::byte{64};
    expected[8] = std::byte{192};
    expected[16] = std::byte{128};
    for (std::size_t index = 0; index < 8; ++index)
    {
      expected[24 + index] = static_cast<std::byte>(8 - index);
    }
    expected[33] = std::byte{0x10};
    expected[40] = std::byte{3};
    expected[48] = std::byte{1};
    EXPECT_EQ(require_value(mapped::encode_record_envelope(envelope)), expected);
    EXPECT_EQ(require_value(mapped::decode_record_envelope(expected)), envelope);
    for (const auto offset : {0u, 2u, 4u, 8u, 16u, 24u, 32u, 48u, 56u})
    {
      auto invalid = expected;
      if (offset == 24)
      {
        put_word(invalid, offset, 0);
      }
      else
      {
        invalid[offset] = std::byte{0xff};
      }
      expect_error(mapped::decode_record_envelope(invalid));
    }
    expect_error(mapped::decode_record_envelope(std::span{expected}.first(63)));
    envelope._totalBytes = std::numeric_limits<std::uint64_t>::max();
    expect_error(mapped::encode_record_envelope(envelope), ErrorCode::InvalidArgument);
  }

  // A manifest owns its record-reference list and redo bytes through absolute logical slices.
  // Only short zero padding is permitted beyond those bytes, keeping the canonical encoding unambiguous.
  TEST(JournalFormat, ManifestMatchesIndependentGoldenAbsoluteSlices)
  {
    const auto fixture = manifest_fixture();
    EXPECT_EQ(require_value(mapped::encode_manifest_body(manifest(), 8192)), fixture);
    EXPECT_EQ(require_value(mapped::decode_manifest_body(fixture, 8192)), manifest());
    auto padded = fixture;
    padded.resize(128);
    EXPECT_EQ(require_value(mapped::decode_manifest_body(padded, 8192)), manifest());
    padded.back() = std::byte{1};
    expect_error(mapped::decode_manifest_body(padded, 8192));
    padded = fixture;
    padded.resize(168);
    expect_error(mapped::decode_manifest_body(padded, 8192));
  }

  // Slices and record references are untrusted persisted values, not pointers to follow first.
  // Reject overflow, overlap, duplicate/out-of-order references, and self-reference before exposing a manifest.
  TEST(JournalFormat, ManifestRejectsOverflowOverlapAndReferencesOutsideItsBatch)
  {
    const auto fixture = manifest_fixture();
    for (const auto offset : {32u, 40u, 48u, 56u})
    {
      auto invalid = fixture;
      put_word(invalid, offset, std::numeric_limits<std::uint64_t>::max());
      expect_error(mapped::decode_manifest_body(invalid, 8192));
    }
    for (const auto reference : {std::uint64_t{0}, std::uint64_t{4096}, std::uint64_t{8192}})
    {
      auto invalid = fixture;
      put_word(invalid, 72, reference);
      expect_error(mapped::decode_manifest_body(invalid, 8192));
    }
    auto overlap = fixture;
    put_word(overlap, 48, 8320);
    expect_error(mapped::decode_manifest_body(overlap, 8192));
    expect_error(mapped::decode_manifest_body(std::span{fixture}.first(64), 8192));
    expect_error(mapped::decode_manifest_body(fixture, std::numeric_limits<std::uint64_t>::max() - 7));
  }

  // Registry appends advance the Registry revision once; publication/retry appends preserve it.
  // Keeping those rules in the wire format prevents a valid frame from describing an impossible transition.
  TEST(JournalFormat, ManifestRejectsUnknownCodesFlagsAndInvalidRevisionTransitions)
  {
    for (const auto [offset, value] : {std::pair{0u, 4u}, {4u, 1u}, {8u, 0u}, {24u, 6u}})
    {
      auto invalid = manifest_fixture();
      invalid[offset] = static_cast<std::byte>(value);
      expect_error(mapped::decode_manifest_body(invalid, 8192));
    }
    for (const auto subtype : {mapped::TransactionSubtype::Publication, mapped::TransactionSubtype::Retry})
    {
      auto value = manifest();
      value._subtype = subtype;
      expect_error(mapped::encode_manifest_body(value, 8192), ErrorCode::InvalidArgument);
      value._newRevision = value._baseRevision;
      EXPECT_EQ(require_value(mapped::decode_manifest_body(require_value(mapped::encode_manifest_body(value, 8192)), 8192)), value);
    }
  }

  // An empty list has a zero offset as well as a zero count, rather than a dangling logical address.
  // The codec can represent empty slices even though the execution layer imposes additional batch requirements.
  TEST(JournalFormat, EmptyManifestSlicesUseZeroOffsetsAndBoundedCounts)
  {
    auto value = manifest();
    value._records.clear();
    value._commands.clear();
    const auto encoded = require_value(mapped::encode_manifest_body(value, 8192));
    ASSERT_EQ(encoded.size(), 64u);
    EXPECT_TRUE(std::all_of(encoded.begin() + 32, encoded.end(),
                            [](std::byte byte)
                            {
                              return byte == std::byte{};
                            }));
    EXPECT_EQ(require_value(mapped::decode_manifest_body(encoded, 8192)), value);
    auto invalid = encoded;
    put_word(invalid, 32, 8320);
    expect_error(mapped::decode_manifest_body(invalid, 8192));
    invalid = encoded;
    put_word(invalid, 48, 8320);
    expect_error(mapped::decode_manifest_body(invalid, 8192));
    value._records = {8192};
    expect_error(mapped::encode_manifest_body(value, 8192), ErrorCode::InvalidArgument);
  }
} // namespace
