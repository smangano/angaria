#include "angaria/capture.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <ios>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace angaria
{
  namespace
  {
    struct CaptureFailure
    {
      Error _error;
    };

    [[noreturn]] void fail(ErrorCode code, const char* path, const char* message)
    {
      throw CaptureFailure{Error{code, message, path}};
    }

    template <typename T>
    T take(Result<T> result)
    {
      if (auto* error = std::get_if<Error>(&result))
      {
        throw CaptureFailure{std::move(*error)};
      }
      return std::get<T>(std::move(result));
    }

    Error capture_error(std::uint64_t record, std::uint64_t offset)
    {
      Error error;
      try
      {
        throw;
      }
      catch (const CaptureFailure& failure)
      {
        error = failure._error;
      }
      catch (const std::bad_alloc&)
      {
        error = Error{ErrorCode::CapacityExceeded, "Insufficient memory for capture data", "memory"};
      }
      catch (const std::length_error&)
      {
        error = Error{ErrorCode::CapacityExceeded, "Capture data exceeds a native container limit", "memory"};
      }
      catch (...)
      {
        error = Error{ErrorCode::InvalidState, "Unexpected capture helper failure", "internal"};
      }
      const auto position = "capture.records[" + std::to_string(record) + "]@" + std::to_string(offset);
      error._path = position + "." + error._path;
      error._message = "Capture record " + std::to_string(record) + " at byte " + std::to_string(offset) +
          ": " + error._message;
      return error;
    }

    void check_limits(std::uint64_t recordLimit)
    {
      if (recordLimit < MessageRecordCodec::header_bytes)
      {
        fail(ErrorCode::InvalidArgument, "record_limit", "Record limit must include the complete 80-byte header");
      }
    }

    std::streamsize stream_count(std::uint64_t count)
    {
      if (count > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
      {
        fail(ErrorCode::CapacityExceeded, "capacity", "Record byte count exceeds the stream operation range");
      }
      return static_cast<std::streamsize>(count);
    }

    void check_capture_capacity(std::uint64_t count, std::uint64_t bytes,
                                  std::uint64_t records, std::uint64_t limit)
    {
      if (bytes > limit || count > limit - bytes)
      {
        fail(ErrorCode::CapacityExceeded, "capacity", "Record exceeds the remaining capture byte budget");
      }
      if (records == std::numeric_limits<std::uint64_t>::max())
      {
        fail(ErrorCode::CapacityExceeded, "records", "Capture record counter is exhausted");
      }
      static_cast<void>(stream_count(count));
    }

    bool has_next_byte(std::istream& input)
    {
      if (input.bad() || (input.fail() && !input.eof()))
      {
        fail(ErrorCode::IoError, "header", "Capture input stream is not readable");
      }
      if (input.eof())
      {
        return false;
      }
      std::istream::int_type next{};
      bool threw{};
      try
      {
        next = input.peek();
      }
      catch (...)
      {
        threw = true;
      }
      // EOF may throw when the caller enables eofbit/failbit exceptions. It
      // still means a clean boundary when no byte of another header was read.
      if (input.eof() && !input.bad())
      {
        return false;
      }
      if (threw || input.fail() || input.bad() ||
          std::istream::traits_type::eq_int_type(next, std::istream::traits_type::eof()))
      {
        fail(ErrorCode::IoError, "header", "Capture input stream failed while checking the next record");
      }
      return true;
    }

    void read_exact(std::istream& input, std::span<std::byte> bytes, const char* component)
    {
      if (bytes.empty())
      {
        return;
      }
      const auto count = stream_count(bytes.size());
      bool threw{};
      try
      {
        input.read(reinterpret_cast<char*>(bytes.data()), count);
      }
      catch (...)
      {
        threw = true;
      }
      if (input.bad() || (input.fail() && !input.eof()) || (threw && !input.eof()))
      {
        fail(ErrorCode::IoError, component, "Capture input stream failed while reading a record");
      }
      if (input.gcount() != count)
      {
        if (input.eof())
        {
          fail(ErrorCode::InvalidArgument, component, "Truncated capture record");
        }
        fail(ErrorCode::IoError, component, "Capture input stream returned an incomplete read without EOF");
      }
      if (threw || input.fail())
      {
        fail(ErrorCode::IoError, component, "Capture input stream reported a read failure");
      }
    }
  } // namespace

  MessageCaptureWriter::MessageCaptureWriter(std::ostream& output, std::uint64_t recordLimit,
                                               std::uint64_t captureLimit)
      : _output(&output), _recordLimit(recordLimit), _captureLimit(captureLimit)
  {
  }

  Result<MessageCaptureWriter> MessageCaptureWriter::create(std::ostream& output,
                                                              std::uint64_t maxRecordBytes,
                                                              std::uint64_t maxCaptureBytes)
  {
    try
    {
      check_limits(maxRecordBytes);
      if (!output.good())
      {
        fail(ErrorCode::IoError, "output", "Capture output stream is not writable");
      }
      return MessageCaptureWriter{output, maxRecordBytes, maxCaptureBytes};
    }
    catch (...)
    {
      return capture_error(0, 0);
    }
  }

  MessageCaptureWriter::MessageCaptureWriter(MessageCaptureWriter&& other) noexcept
      : _output(std::exchange(other._output, nullptr)),
        _recordLimit(std::exchange(other._recordLimit, 0)),
        _captureLimit(std::exchange(other._captureLimit, 0)),
        _bytes(std::exchange(other._bytes, 0)), _records(std::exchange(other._records, 0)),
        _failed(std::exchange(other._failed, false))
  {
  }

  MessageCaptureWriter& MessageCaptureWriter::operator=(MessageCaptureWriter&& other) noexcept
  {
    if (this != &other)
    {
      _output = std::exchange(other._output, nullptr);
      _recordLimit = std::exchange(other._recordLimit, 0);
      _captureLimit = std::exchange(other._captureLimit, 0);
      _bytes = std::exchange(other._bytes, 0);
      _records = std::exchange(other._records, 0);
      _failed = std::exchange(other._failed, false);
    }
    return *this;
  }

  OperationResult MessageCaptureWriter::append(const MessageHeader& header,
                                                 std::span<const std::byte> payload)
  {
    try
    {
      if (!_output || _failed)
      {
        fail(ErrorCode::InvalidState, "state", "Capture writer is moved from or terminal after I/O failure");
      }
      // Header encoding allocates no payload bytes. Establish every byte
      // budget before the owning record encoder copies the caller's payload.
      static_cast<void>(take(MessageRecordCodec::encode_header(header, _recordLimit)));
      if (header._payloadBytes != payload.size())
      {
        fail(ErrorCode::InvalidArgument, "payload_size", "Declared payload length differs from the supplied bytes");
      }
      const auto total = std::uint64_t{MessageRecordCodec::header_bytes} + header._payloadBytes;
      check_capture_capacity(total, _bytes, _records, _captureLimit);
      const auto bytes = take(MessageRecordCodec::encode(header, payload, _recordLimit));
      return append(bytes);
    }
    catch (...)
    {
      return capture_error(_records, _bytes);
    }
  }

  OperationResult MessageCaptureWriter::append(std::span<const std::byte> record)
  {
    try
    {
      if (!_output || _failed)
      {
        fail(ErrorCode::InvalidState, "state", "Capture writer is moved from or terminal after I/O failure");
      }
      static_cast<void>(take(MessageRecordCodec::decode_view(record, _recordLimit)));
      check_capture_capacity(record.size(), _bytes, _records, _captureLimit);
      bool threw{};
      try
      {
        _output->write(reinterpret_cast<const char*>(record.data()), stream_count(record.size()));
      }
      catch (...)
      {
        threw = true;
      }
      if (threw || !_output->good())
      {
        _failed = true;
        fail(ErrorCode::IoError, "output", "Capture output failed; a partial record may remain in the stream");
      }
      _bytes += static_cast<std::uint64_t>(record.size());
      ++_records;
      return std::monostate{};
    }
    catch (...)
    {
      return capture_error(_records, _bytes);
    }
  }

  std::uint64_t MessageCaptureWriter::bytes_written() const
  {
    return _bytes;
  }

  std::uint64_t MessageCaptureWriter::records_written() const
  {
    return _records;
  }

  MessageCaptureReader::MessageCaptureReader(std::istream& input, std::uint64_t recordLimit,
                                               std::uint64_t captureLimit)
      : _input(&input), _recordLimit(recordLimit), _captureLimit(captureLimit)
  {
  }

  Result<MessageCaptureReader> MessageCaptureReader::create(std::istream& input,
                                                              std::uint64_t maxRecordBytes,
                                                              std::uint64_t maxCaptureBytes)
  {
    try
    {
      check_limits(maxRecordBytes);
      if (input.bad() || (input.fail() && !input.eof()))
      {
        fail(ErrorCode::IoError, "input", "Capture input stream is not readable");
      }
      return MessageCaptureReader{input, maxRecordBytes, maxCaptureBytes};
    }
    catch (...)
    {
      return capture_error(0, 0);
    }
  }

  MessageCaptureReader::MessageCaptureReader(MessageCaptureReader&& other) noexcept
      : _input(std::exchange(other._input, nullptr)),
        _recordLimit(std::exchange(other._recordLimit, 0)),
        _captureLimit(std::exchange(other._captureLimit, 0)),
        _bytes(std::exchange(other._bytes, 0)), _records(std::exchange(other._records, 0)),
        _failed(std::exchange(other._failed, false)), _ended(std::exchange(other._ended, false))
  {
  }

  MessageCaptureReader& MessageCaptureReader::operator=(MessageCaptureReader&& other) noexcept
  {
    if (this != &other)
    {
      _input = std::exchange(other._input, nullptr);
      _recordLimit = std::exchange(other._recordLimit, 0);
      _captureLimit = std::exchange(other._captureLimit, 0);
      _bytes = std::exchange(other._bytes, 0);
      _records = std::exchange(other._records, 0);
      _failed = std::exchange(other._failed, false);
      _ended = std::exchange(other._ended, false);
    }
    return *this;
  }

  Result<std::optional<MessageRecord>> MessageCaptureReader::read_next()
  {
    try
    {
      if (!_input || _failed)
      {
        fail(ErrorCode::InvalidState, "state", "Capture reader is moved from or terminal after a read error");
      }
      if (_ended || !has_next_byte(*_input))
      {
        _ended = true;
        return std::optional<MessageRecord>{};
      }
      check_capture_capacity(MessageRecordCodec::header_bytes, _bytes, _records, _captureLimit);
      MessageRecordCodec::HeaderBytes bytes{};
      read_exact(*_input, bytes, "header");
      auto header = take(MessageRecordCodec::decode_header(bytes, _recordLimit));
      const auto total = std::uint64_t{MessageRecordCodec::header_bytes} + header._payloadBytes;
      check_capture_capacity(total, _bytes, _records, _captureLimit);
      // decode_header already established native allocation bounds. The full
      // capture bound is also established before any payload read/allocation.
      MessageRecord record{header, std::vector<std::byte>(static_cast<std::size_t>(header._payloadBytes))};
      read_exact(*_input, record._payload, "payload");
      _bytes += total;
      ++_records;
      return std::optional<MessageRecord>{std::move(record)};
    }
    catch (...)
    {
      _failed = true;
      return capture_error(_records, _bytes);
    }
  }

  std::uint64_t MessageCaptureReader::bytes_read() const
  {
    return _bytes;
  }

  std::uint64_t MessageCaptureReader::records_read() const
  {
    return _records;
  }
} // namespace angaria
