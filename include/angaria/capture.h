#pragma once

#include "angaria/message_record.h"

#include <cstdint>
#include <istream>
#include <optional>
#include <ostream>

namespace angaria
{
  // Borrowed ordinary streams. These capture helpers do not journal appends,
  // follow a live writer, own files, or establish Queue delivery acceptance.
  class MessageCaptureWriter
  {
  public:
    static Result<MessageCaptureWriter> create(std::ostream& output, std::uint64_t maxRecordBytes,
                                                std::uint64_t maxCaptureBytes);
    MessageCaptureWriter(MessageCaptureWriter&& other) noexcept;
    MessageCaptureWriter& operator=(MessageCaptureWriter&& other) noexcept;
    MessageCaptureWriter(const MessageCaptureWriter&) = delete;
    MessageCaptureWriter& operator=(const MessageCaptureWriter&) = delete;

    OperationResult append(const MessageHeader& header, std::span<const std::byte> payload);
    OperationResult append(std::span<const std::byte> record);
    // Count complete successful records relative to this wrapper's starting
    // stream position. A failed I/O append may leave uncounted partial bytes.
    std::uint64_t bytes_written() const;
    std::uint64_t records_written() const;

  private:
    MessageCaptureWriter(std::ostream& output, std::uint64_t recordLimit, std::uint64_t captureLimit);
    std::ostream* _output{};
    std::uint64_t _recordLimit{};
    std::uint64_t _captureLimit{};
    std::uint64_t _bytes{};
    std::uint64_t _records{};
    bool _failed{};
  };

  class MessageCaptureReader
  {
  public:
    static Result<MessageCaptureReader> create(std::istream& input, std::uint64_t maxRecordBytes,
                                                std::uint64_t maxCaptureBytes);
    MessageCaptureReader(MessageCaptureReader&& other) noexcept;
    MessageCaptureReader& operator=(MessageCaptureReader&& other) noexcept;
    MessageCaptureReader(const MessageCaptureReader&) = delete;
    MessageCaptureReader& operator=(const MessageCaptureReader&) = delete;

    // Empty optional means clean EOF between records. Any read error is
    // terminal; counters remain at the start of that incomplete/invalid record.
    Result<std::optional<MessageRecord>> read_next();
    std::uint64_t bytes_read() const;
    std::uint64_t records_read() const;

  private:
    MessageCaptureReader(std::istream& input, std::uint64_t recordLimit, std::uint64_t captureLimit);
    std::istream* _input{};
    std::uint64_t _recordLimit{};
    std::uint64_t _captureLimit{};
    std::uint64_t _bytes{};
    std::uint64_t _records{};
    bool _failed{};
    bool _ended{};
  };
} // namespace angaria
