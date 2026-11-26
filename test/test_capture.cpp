// These examples use ordinary memory streams and independent canonical bytes.
// A capture records application bytes; it is not a native journal, publication,
// delivery acknowledgment, or promise of an atomic/durable file append.
#include "result_helpers.h"
#include "angaria/capture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;

  static_assert(!std::is_copy_constructible_v<MessageCaptureWriter>);
  static_assert(!std::is_copy_constructible_v<MessageCaptureReader>);
  static_assert(std::is_nothrow_move_constructible_v<MessageCaptureWriter>);
  static_assert(std::is_nothrow_move_constructible_v<MessageCaptureReader>);

  using angaria::test_support::require_value;

  template <typename T>
  void expect_error(const Result<T>& result, ErrorCode code, std::string_view path = {})
  {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    const auto& error = std::get<Error>(result);
    EXPECT_EQ(error._code, code) << error._path << ": " << error._message;
    EXPECT_FALSE(error._message.empty());
    if (!path.empty())
    {
      EXPECT_NE(error._path.find(path), std::string::npos) << error._path;
    }
  }

  std::vector<std::byte> golden_header()
  {
    // Hand-authored from the portable record table. No production encoder
    // supplies expected framing or values in this fixture.
    constexpr std::array<unsigned char, 80> literal{
        0x41, 0x4e, 0x47, 0x41, 0x4d, 0x53, 0x47, 0x00,
        0x01, 0x00, 0x50, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    std::vector<std::byte> bytes;
    for (const auto byte : literal)
    {
      bytes.push_back(static_cast<std::byte>(byte));
    }
    return bytes;
  }

  std::vector<std::byte> golden_record()
  {
    auto bytes = golden_header();
    bytes.insert(bytes.end(), {std::byte{0}, std::byte{0xff}, std::byte{0x5a}});
    return bytes;
  }

  std::string stream_bytes(std::span<const std::byte> bytes)
  {
    if (bytes.empty())
    {
      return {};
    }
    return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
  }

  MessageHeader header(std::uint64_t payloadBytes = 3)
  {
    MessageHeader result;
    result._schema = {static_cast<MessageType>(1), 1};
    result._publication = {{{1, 2}, TaskId{1}, 1}, 1};
    result._payloadBytes = payloadBytes;
    return result;
  }

  class LimitedWriteBuffer : public std::streambuf
  {
  public:
    explicit LimitedWriteBuffer(std::size_t capacity) : _capacity(capacity)
    {
    }

    const std::string& bytes() const
    {
      return _bytes;
    }

  protected:
    std::streamsize xsputn(const char* source, std::streamsize count) override
    {
      const auto available = _capacity - _bytes.size();
      const auto written = std::min(static_cast<std::size_t>(count), available);
      _bytes.append(source, written);
      return static_cast<std::streamsize>(written);
    }

  private:
    std::size_t _capacity;
    std::string _bytes;
  };

  // No get area bypasses these overrides. Tests can observe requested read
  // sizes and inject a genuine non-EOF stream failure after partial transfer.
  class ObservedReadBuffer : public std::streambuf
  {
  public:
    explicit ObservedReadBuffer(std::string bytes,
                                 std::size_t failAfter = std::numeric_limits<std::size_t>::max())
        : _bytes(std::move(bytes)), _failAfter(failAfter)
    {
    }

    std::size_t cursor() const
    {
      return _cursor;
    }

    const std::vector<std::streamsize>& requests() const
    {
      return _requests;
    }

  protected:
    int_type underflow() override
    {
      if (_cursor == _bytes.size())
      {
        return traits_type::eof();
      }
      if (_cursor >= _failAfter)
      {
        throw std::runtime_error("Injected capture input failure");
      }
      return traits_type::to_int_type(_bytes[_cursor]);
    }

    int_type uflow() override
    {
      const auto value = underflow();
      if (!traits_type::eq_int_type(value, traits_type::eof()))
      {
        ++_cursor;
      }
      return value;
    }

    std::streamsize xsgetn(char* destination, std::streamsize count) override
    {
      _requests.push_back(count);
      const auto available = _bytes.size() - _cursor;
      const auto beforeFailure = _cursor < _failAfter ? _failAfter - _cursor : 0;
      const auto copied = std::min({static_cast<std::size_t>(count), available, beforeFailure});
      std::copy_n(_bytes.data() + _cursor, copied, destination);
      _cursor += copied;
      if (copied < static_cast<std::size_t>(count) && _cursor < _bytes.size() && _cursor >= _failAfter)
      {
        throw std::runtime_error("Injected partial capture input failure");
      }
      return static_cast<std::streamsize>(copied);
    }

  private:
    std::string _bytes;
    std::size_t _failAfter;
    std::size_t _cursor{};
    std::vector<std::streamsize> _requests;
  };

  TEST(CaptureTest, WritesIndependentCanonicalBytesThroughEitherAppendForm)
  {
    std::ostringstream output;
    auto writer = require_value(MessageCaptureWriter::create(output, 83, 166));
    const auto record = golden_record();
    require_value(writer.append(record));
    require_value(writer.append(header(), std::span{record}.subspan(80)));
    const auto expected = stream_bytes(record);
    EXPECT_EQ(output.str(), expected + expected);
    EXPECT_EQ(writer.bytes_written(), 166u);
    EXPECT_EQ(writer.records_written(), 2u);
    // Capture order preserves duplicate records; it is not admission/dedup state.
  }

  TEST(CaptureTest, ReadsConcatenatedRecordsAndReturnsIndependentOwningPayloads)
  {
    const auto bytes = stream_bytes(golden_record());
    std::istringstream input{bytes + bytes};
    auto reader = require_value(MessageCaptureReader::create(input, 83, 166));
    auto first = require_value(reader.read_next());
    ASSERT_TRUE(first);
    auto second = require_value(reader.read_next());
    ASSERT_TRUE(second);
    EXPECT_EQ(first->_header, header());
    EXPECT_EQ(first->_payload, (std::vector<std::byte>{std::byte{0}, std::byte{0xff}, std::byte{0x5a}}));
    EXPECT_EQ(second->_header, first->_header);
    EXPECT_EQ(reader.bytes_read(), 166u);
    EXPECT_EQ(reader.records_read(), 2u);
    EXPECT_FALSE(require_value(reader.read_next()));
    input.str("replaced input storage");
    input.clear();
    EXPECT_EQ(first->_payload[1], std::byte{0xff});
    EXPECT_FALSE(require_value(reader.read_next()));
  }

  TEST(CaptureTest, EmptyCaptureIsValidAtZeroAggregateBudgetAndEofIsSticky)
  {
    for (const auto mask : {std::ios::goodbit, std::ios::eofbit | std::ios::failbit | std::ios::badbit})
    {
      std::istringstream input;
      input.exceptions(mask);
      auto reader = require_value(MessageCaptureReader::create(input, 80, 0));
      EXPECT_FALSE(require_value(reader.read_next()));
      EXPECT_FALSE(require_value(reader.read_next()));
      EXPECT_EQ(reader.bytes_read(), 0u);
      EXPECT_EQ(reader.records_read(), 0u);
      EXPECT_EQ(input.exceptions(), mask);
      input.clear();
      input.str(stream_bytes(golden_record()));
      EXPECT_FALSE(require_value(reader.read_next()));
    }
    std::ostringstream output;
    auto writer = require_value(MessageCaptureWriter::create(output, 83, 0));
    expect_error(writer.append(golden_record()), ErrorCode::CapacityExceeded, "capture.records[0]@0");
    EXPECT_TRUE(output.str().empty());
    EXPECT_EQ(writer.records_written(), 0u);
  }

  TEST(CaptureTest, RecordLimitsNeedAHeaderButAggregateLimitCanBeSmaller)
  {
    std::istringstream input;
    std::ostringstream output;
    for (const auto cap : {0u, 1u, 79u})
    {
      expect_error(MessageCaptureWriter::create(output, cap, 1000), ErrorCode::InvalidArgument, "record_limit");
      expect_error(MessageCaptureReader::create(input, cap, 1000), ErrorCode::InvalidArgument, "record_limit");
    }
    auto writer = require_value(MessageCaptureWriter::create(output, 83, 80));
    expect_error(writer.append(header(), std::vector<std::byte>(3)), ErrorCode::CapacityExceeded);
    EXPECT_TRUE(output.str().empty());
    require_value(writer.append(header(0), {}));
    EXPECT_EQ(output.str().size(), 80u);
    auto expected = golden_header();
    expected[72] = std::byte{};
    EXPECT_EQ(output.str(), stream_bytes(expected));
    std::istringstream saved{output.str()};
    auto reader = require_value(MessageCaptureReader::create(saved, 1000, 80));
    auto record = require_value(reader.read_next());
    ASSERT_TRUE(record);
    EXPECT_TRUE(record->_payload.empty());
    EXPECT_FALSE(require_value(reader.read_next()));
  }

  TEST(CaptureTest, MalformedAppendAndCapacityFailureAreRetryableWithoutOutputChanges)
  {
    std::ostringstream output;
    auto writer = require_value(MessageCaptureWriter::create(output, 83, 166));
    const auto valid = golden_record();
    require_value(writer.append(valid));
    const auto before = output.str();
    auto invalid = valid;
    invalid[0] = std::byte{};
    expect_error(writer.append(invalid), ErrorCode::InvalidArgument, "capture.records[1]@83.message_record.magic");
    auto inconsistent = header(2);
    expect_error(writer.append(inconsistent, std::span{valid}.subspan(80)), ErrorCode::InvalidArgument, "capture.records[1]@83");
    auto unknownVersion = valid;
    unknownVersion[8] = std::byte{2};
    expect_error(writer.append(unknownVersion), ErrorCode::UnsupportedPolicy, "capture.records[1]@83");
    EXPECT_EQ(output.str(), before);
    EXPECT_EQ(writer.records_written(), 1u);
    require_value(writer.append(valid));
    EXPECT_EQ(writer.records_written(), 2u);
    const auto complete = output.str();
    expect_error(writer.append(valid), ErrorCode::CapacityExceeded, "capture.records[2]@166");
    EXPECT_EQ(output.str(), complete);
    EXPECT_EQ(writer.bytes_written(), 166u);
  }

  TEST(CaptureTest, EveryPartialHeaderAndPayloadIsTruncationInsteadOfCleanEof)
  {
    const auto record = golden_record();
    for (std::size_t length = 1; length < record.size(); ++length)
    {
      SCOPED_TRACE(length);
      std::istringstream input{stream_bytes(std::span{record}.first(length))};
      auto reader = require_value(MessageCaptureReader::create(input, 83, 83));
      expect_error(reader.read_next(), ErrorCode::InvalidArgument,
                   length < 80 ? "capture.records[0]@0.header" : "capture.records[0]@0.payload");
      EXPECT_EQ(reader.bytes_read(), 0u);
      EXPECT_EQ(reader.records_read(), 0u);
      input.clear();
      expect_error(reader.read_next(), ErrorCode::InvalidState);
    }
  }

  TEST(CaptureTest, TruncatedSecondRecordReportsItsOwnIndexAndStartByte)
  {
    const auto record = stream_bytes(golden_record());
    for (const auto length : {1u, 79u, 80u, 82u})
    {
      std::istringstream input{record + record.substr(0, length)};
      auto reader = require_value(MessageCaptureReader::create(input, 83, 1000));
      ASSERT_TRUE(require_value(reader.read_next()));
      expect_error(reader.read_next(), ErrorCode::InvalidArgument, "capture.records[1]@83");
      EXPECT_EQ(reader.bytes_read(), 83u);
      EXPECT_EQ(reader.records_read(), 1u);
    }
  }

  TEST(CaptureTest, DeclaredBoundsFailAfterHeaderAndBeforeAnyPayloadRead)
  {
    for (const auto limits : {std::pair{82u, 1000u}, std::pair{83u, 82u}})
    {
      ObservedReadBuffer buffer{stream_bytes(golden_record())};
      std::istream input{&buffer};
      auto reader = require_value(MessageCaptureReader::create(input, limits.first, limits.second));
      expect_error(reader.read_next(), ErrorCode::CapacityExceeded, "capture.records[0]@0");
      EXPECT_EQ(buffer.cursor(), 80u);
      EXPECT_EQ(buffer.requests(), (std::vector<std::streamsize>{80}));
      EXPECT_EQ(reader.bytes_read(), 0u);
      expect_error(reader.read_next(), ErrorCode::InvalidState);
    }
    ObservedReadBuffer buffer{stream_bytes(golden_record())};
    std::istream input{&buffer};
    auto reader = require_value(MessageCaptureReader::create(input, 83, 0));
    expect_error(reader.read_next(), ErrorCode::CapacityExceeded);
    EXPECT_EQ(buffer.cursor(), 0u);
    EXPECT_TRUE(buffer.requests().empty());
  }

  TEST(CaptureTest, UnknownAndMalformedHeadersAreTerminalWithoutReadingPayload)
  {
    struct Mutation
    {
      std::size_t _offset;
      std::byte _value;
      ErrorCode _code;
    };
    for (const auto mutation : {Mutation{0, std::byte{}, ErrorCode::InvalidArgument},
                                Mutation{8, std::byte{2}, ErrorCode::UnsupportedPolicy},
                                Mutation{12, std::byte{6}, ErrorCode::InvalidArgument},
                                Mutation{18, std::byte{1}, ErrorCode::InvalidArgument}})
    {
      auto bytes = golden_record();
      bytes[mutation._offset] = mutation._value;
      ObservedReadBuffer buffer{stream_bytes(bytes)};
      std::istream input{&buffer};
      auto reader = require_value(MessageCaptureReader::create(input, 83, 83));
      expect_error(reader.read_next(), mutation._code, "capture.records[0]@0.message_record");
      EXPECT_EQ(buffer.requests(), (std::vector<std::streamsize>{80}));
      expect_error(reader.read_next(), ErrorCode::InvalidState);
    }
    auto bytes = golden_header();
    std::fill(bytes.begin() + 72, bytes.end(), std::byte{0xff});
    ObservedReadBuffer buffer{stream_bytes(bytes)};
    std::istream input{&buffer};
    auto reader = require_value(MessageCaptureReader::create(input,
        std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max()));
    expect_error(reader.read_next(), ErrorCode::InvalidArgument, "capture.records[0]@0.message_record.size");
    EXPECT_EQ(buffer.requests(), (std::vector<std::streamsize>{80}));
  }

  TEST(CaptureTest, KnownNonBinaryFormatsRemainOpaqueCaptureBytes)
  {
    auto metadata = header();
    metadata._format = PayloadFormat::Json;
    const std::array payload{std::byte{}, std::byte{0xff}, std::byte{0x5a}};
    std::ostringstream output;
    auto writer = require_value(MessageCaptureWriter::create(output, 83, 83));
    require_value(writer.append(metadata, payload));
    std::istringstream input{output.str()};
    auto reader = require_value(MessageCaptureReader::create(input, 83, 83));
    auto record = require_value(reader.read_next());
    ASSERT_TRUE(record);
    EXPECT_EQ(record->_header._format, PayloadFormat::Json);
    EXPECT_TRUE(std::equal(record->_payload.begin(), record->_payload.end(), payload.begin(), payload.end()));
  }

  TEST(CaptureTest, EofExceptionMasksPreserveCleanBoundaryAndTruncationMeaning)
  {
    const auto complete = stream_bytes(golden_record());
    for (const auto length : {0u, 1u, 79u, 80u, 82u, 83u})
    {
      SCOPED_TRACE(length);
      std::istringstream input{complete.substr(0, length)};
      const auto mask = std::ios::badbit | std::ios::failbit | std::ios::eofbit;
      input.exceptions(mask);
      auto reader = require_value(MessageCaptureReader::create(input, 83, 83));
      if (length == 0)
      {
        EXPECT_FALSE(require_value(reader.read_next()));
      }
      else if (length == 83)
      {
        ASSERT_TRUE(require_value(reader.read_next()));
        EXPECT_FALSE(require_value(reader.read_next()));
        EXPECT_FALSE(require_value(reader.read_next()));
      }
      else
      {
        expect_error(reader.read_next(), ErrorCode::InvalidArgument);
        expect_error(reader.read_next(), ErrorCode::InvalidState);
      }
      EXPECT_EQ(input.exceptions(), mask);
    }
  }

  TEST(CaptureTest, PartialOutputFailuresAreIoErrorsAndMakeTheWriterTerminal)
  {
    for (const auto mask : {std::ios::goodbit, std::ios::badbit | std::ios::failbit})
    {
      LimitedWriteBuffer buffer{17};
      std::ostream output{&buffer};
      output.exceptions(mask);
      auto writer = require_value(MessageCaptureWriter::create(output, 83, 1000));
      expect_error(writer.append(golden_record()), ErrorCode::IoError, "capture.records[0]@0.output");
      EXPECT_EQ(buffer.bytes().size(), 17u);
      EXPECT_EQ(writer.bytes_written(), 0u);
      EXPECT_EQ(writer.records_written(), 0u);
      output.clear();
      expect_error(writer.append(golden_record()), ErrorCode::InvalidState);
      EXPECT_EQ(buffer.bytes().size(), 17u);
      EXPECT_EQ(output.exceptions(), mask);
    }
  }

  TEST(CaptureTest, ThrowingInputFailuresAreIoErrorsEvenAfterPartialTransfer)
  {
    for (const auto failAfter : {0u, 17u, 81u})
    {
      for (const auto mask : {std::ios::goodbit, std::ios::badbit | std::ios::failbit})
      {
        SCOPED_TRACE(failAfter);
        ObservedReadBuffer buffer{stream_bytes(golden_record()), failAfter};
        std::istream input{&buffer};
        input.exceptions(mask);
        auto reader = require_value(MessageCaptureReader::create(input, 83, 1000));
        expect_error(reader.read_next(), ErrorCode::IoError, "capture.records[0]@0");
        EXPECT_EQ(reader.bytes_read(), 0u);
        EXPECT_EQ(reader.records_read(), 0u);
        input.clear();
        expect_error(reader.read_next(), ErrorCode::InvalidState);
        EXPECT_EQ(input.exceptions(), mask);
      }
    }
  }

  TEST(CaptureTest, ExistingNonEofStreamFailuresAreNotMisreportedAsCleanEof)
  {
    std::istringstream input{stream_bytes(golden_record())};
    auto reader = require_value(MessageCaptureReader::create(input, 83, 1000));
    input.setstate(std::ios::failbit);
    expect_error(reader.read_next(), ErrorCode::IoError);
    input.clear();
    expect_error(reader.read_next(), ErrorCode::InvalidState);
    std::istringstream initiallyBad;
    initiallyBad.setstate(std::ios::badbit);
    expect_error(MessageCaptureReader::create(initiallyBad, 80, 0), ErrorCode::IoError);
    std::ostringstream output;
    output.setstate(std::ios::failbit);
    expect_error(MessageCaptureWriter::create(output, 80, 0), ErrorCode::IoError);
  }

  TEST(CaptureTest, WriterMovesTransferProgressWithoutOwningOrClosingStreams)
  {
    std::ostringstream output;
    auto first = require_value(MessageCaptureWriter::create(output, 83, 249));
    require_value(first.append(golden_record()));
    auto second = std::move(first);
    EXPECT_EQ(first.bytes_written(), 0u);
    EXPECT_EQ(first.records_written(), 0u);
    expect_error(first.append(golden_record()), ErrorCode::InvalidState);
    require_value(second.append(golden_record()));
    std::ostringstream unused;
    auto third = require_value(MessageCaptureWriter::create(unused, 80, 0));
    third = std::move(second);
    expect_error(second.append(golden_record()), ErrorCode::InvalidState);
    require_value(third.append(golden_record()));
    EXPECT_EQ(third.bytes_written(), 249u);
    EXPECT_EQ(third.records_written(), 3u);
    EXPECT_TRUE(unused.str().empty());
    EXPECT_EQ(output.str().size(), 249u);
  }

  TEST(CaptureTest, ReaderMovesTransferProgressAndMovedFromReadersCannotConsume)
  {
    const auto bytes = stream_bytes(golden_record());
    std::istringstream input{bytes + bytes};
    auto first = require_value(MessageCaptureReader::create(input, 83, 166));
    ASSERT_TRUE(require_value(first.read_next()));
    auto second = std::move(first);
    expect_error(first.read_next(), ErrorCode::InvalidState);
    EXPECT_EQ(first.bytes_read(), 0u);
    std::istringstream unused;
    auto third = require_value(MessageCaptureReader::create(unused, 80, 0));
    third = std::move(second);
    expect_error(second.read_next(), ErrorCode::InvalidState);
    ASSERT_TRUE(require_value(third.read_next()));
    EXPECT_FALSE(require_value(third.read_next()));
    EXPECT_EQ(third.bytes_read(), 166u);
    EXPECT_EQ(third.records_read(), 2u);
    EXPECT_EQ(unused.peek(), std::istream::traits_type::eof());
  }
} // namespace
