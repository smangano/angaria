#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace angaria::detail
{
  inline constexpr char SESSION_LOG_NAME[] = "session.ang";
  inline constexpr std::uint64_t SESSION_RECORD_SIZE = 30;

  enum class LogMode
  {
    Create,
    Update,
    Inspect
  };

  struct LogHeader
  {
    std::uint64_t _size;
    std::uint64_t _maxLogBytes;
    std::uint16_t _schemaId;
    std::string _queueName;
    std::string _messageName;
    std::string _fieldName;
  };

  struct LogRecord
  {
    std::uint64_t _sequence;
    std::int64_t _timestampNs;
    std::int32_t _value;
  };

  class SessionLog
  {
  public:
    SessionLog(const std::filesystem::path& path, LogMode mode);
    ~SessionLog();
    SessionLog(const SessionLog&) = delete;
    SessionLog& operator=(const SessionLog&) = delete;

    void write_header(const std::string& queueName, std::uint64_t maxLogBytes) const;
    LogHeader read_header() const;
    std::uint64_t records_end(const LogHeader& header) const;
    LogRecord read_record(const LogHeader& header, std::uint64_t offset) const;
    std::optional<std::uint64_t> append_reading(const LogHeader& header, std::int32_t value) const;

  private:
    std::uint64_t file_size() const;
    void read_bytes(std::uint64_t offset, std::span<std::uint8_t> bytes) const;
    void write_bytes(std::uint64_t offset, std::span<const std::uint8_t> bytes) const;

    int _fd;
  };
} // namespace angaria::detail
