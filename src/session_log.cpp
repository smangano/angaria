#include "session_log.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace angaria::detail
{
  namespace
  {
    constexpr std::string_view MAGIC{"ANGARIA\0", 8};
    constexpr std::uint16_t VERSION = 1;
    constexpr std::uint16_t READING_SCHEMA_ID = 1;
    constexpr std::uint8_t INT32_KIND = 1;
    constexpr std::uint32_t COMMIT_MARKER = 0x54494d43; // CMIT
    constexpr std::size_t HEADER_PREFIX_SIZE = 26;
    constexpr std::size_t MAX_NAME_SIZE = 63;
    constexpr std::size_t MAX_TEXT_SIZE = 3 * MAX_NAME_SIZE;
    constexpr std::uint64_t MAX_FILE_SIZE = std::numeric_limits<off_t>::max();

    [[noreturn]] void throw_os_error(const char* operation)
    {
      throw std::system_error(errno, std::generic_category(), operation);
    }

    bool valid_name(std::string_view name)
    {
      constexpr std::string_view allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";
      return !name.empty() && name.size() <= MAX_NAME_SIZE && name.find_first_not_of(allowed) == name.npos;
    }

    template <typename UInt>
    void put_le(std::span<std::uint8_t> bytes, std::size_t offset, UInt value)
    {
      for (std::size_t index = 0; index < sizeof(value); ++index)
      {
        bytes[offset + index] = static_cast<std::uint8_t>(value & 0xff);
        value >>= 8;
      }
    }

    template <typename UInt>
    UInt get_le(std::span<const std::uint8_t> bytes, std::size_t offset)
    {
      UInt value{};
      for (std::size_t index = sizeof(value); index > 0; --index)
      {
        value = static_cast<UInt>((value << 8) | bytes[offset + index - 1]);
      }
      return value;
    }

    int open_log(const std::filesystem::path& path, LogMode mode)
    {
      int flags = mode == LogMode::Inspect ? O_RDONLY : O_RDWR;
      if (mode == LogMode::Create)
      {
        flags |= O_CREAT | O_EXCL;
      }
      const int fd = ::open(path.c_str(), flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
      if (fd == -1)
      {
        throw_os_error("open session log");
      }

      const int lock = mode == LogMode::Inspect ? LOCK_SH : LOCK_EX;
      int result;
      do
      {
        result = ::flock(fd, lock);
      } while (result == -1 && errno == EINTR);

      if (result == -1)
      {
        const int error = errno;
        ::close(fd);
        throw std::system_error(error, std::generic_category(), "lock session log");
      }
      return fd;
    }
  } // namespace

  SessionLog::SessionLog(const std::filesystem::path& path, LogMode mode) : _fd(open_log(path, mode))
  {
  }

  SessionLog::~SessionLog()
  {
    ::close(_fd);
  }

  std::uint64_t SessionLog::file_size() const
  {
    struct stat info{};
    if (::fstat(_fd, &info) == -1)
    {
      throw_os_error("stat session log");
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0)
    {
      throw std::runtime_error("invalid session log file");
    }
    return static_cast<std::uint64_t>(info.st_size);
  }

  void SessionLog::read_bytes(std::uint64_t offset, std::span<std::uint8_t> bytes) const
  {
    while (!bytes.empty())
    {
      const auto count = ::pread(_fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
      if (count > 0)
      {
        offset += count;
        bytes = bytes.subspan(count);
      }
      else if (count == 0)
      {
        throw std::runtime_error("short read from session log");
      }
      else if (errno != EINTR)
      {
        throw_os_error("read session log");
      }
    }
  }

  void SessionLog::write_bytes(std::uint64_t offset, std::span<const std::uint8_t> bytes) const
  {
    while (!bytes.empty())
    {
      const auto count = ::pwrite(_fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
      if (count > 0)
      {
        offset += count;
        bytes = bytes.subspan(count);
      }
      else if (count == 0)
      {
        throw std::runtime_error("short write to session log");
      }
      else if (errno != EINTR)
      {
        throw_os_error("write session log");
      }
    }
  }

  void SessionLog::write_header(const std::string& queueName, std::uint64_t maxLogBytes) const
  {
    if (!valid_name(queueName))
    {
      throw std::invalid_argument(
          "session name must contain 1 to 63 letters, digits, underscores, or dashes");
    }
    constexpr std::string_view messageName = "Reading";
    constexpr std::string_view fieldName = "value";
    const auto size = HEADER_PREFIX_SIZE + queueName.size() + messageName.size() + fieldName.size();
    if (maxLogBytes < size || maxLogBytes > MAX_FILE_SIZE)
    {
      throw std::invalid_argument(
          "log limit cannot accommodate the session header or exceeds the file size limit");
    }
    std::array<std::uint8_t, HEADER_PREFIX_SIZE + MAX_TEXT_SIZE> bytes{};
    std::ranges::copy(MAGIC, bytes.begin());
    put_le(bytes, 8, VERSION);
    put_le(bytes, 10, static_cast<std::uint16_t>(size));
    put_le(bytes, 12, maxLogBytes);
    put_le(bytes, 20, READING_SCHEMA_ID);
    bytes[22] = INT32_KIND;
    bytes[23] = static_cast<std::uint8_t>(queueName.size());
    bytes[24] = static_cast<std::uint8_t>(messageName.size());
    bytes[25] = static_cast<std::uint8_t>(fieldName.size());
    auto next = std::ranges::copy(queueName, bytes.begin() + HEADER_PREFIX_SIZE).out;
    next = std::ranges::copy(messageName, next).out;
    std::ranges::copy(fieldName, next);
    write_bytes(0, std::span(bytes).first(size));
  }

  LogHeader SessionLog::read_header() const
  {
    const auto fileSize = file_size();
    std::array<std::uint8_t, HEADER_PREFIX_SIZE> prefix{};
    read_bytes(0, prefix);
    if (!std::ranges::equal(MAGIC, std::span(prefix).first(MAGIC.size())))
    {
      throw std::runtime_error("invalid session log magic");
    }
    if (get_le<std::uint16_t>(prefix, 8) != VERSION || prefix[22] != INT32_KIND)
    {
      throw std::runtime_error("unsupported session log format");
    }
    const auto size = get_le<std::uint16_t>(prefix, 10);
    const auto maxLogBytes = get_le<std::uint64_t>(prefix, 12);
    const auto schemaId = get_le<std::uint16_t>(prefix, 20);
    const std::size_t textSize = prefix[23] + prefix[24] + prefix[25];
    if (size != HEADER_PREFIX_SIZE + textSize || textSize > MAX_TEXT_SIZE || size > fileSize ||
        fileSize > maxLogBytes || maxLogBytes > MAX_FILE_SIZE || schemaId == 0)
    {
      throw std::runtime_error("invalid session log header");
    }

    std::string text(textSize, '\0');
    read_bytes(HEADER_PREFIX_SIZE, std::span(reinterpret_cast<std::uint8_t*>(text.data()), text.size()));
    auto queueName = text.substr(0, prefix[23]);
    auto messageName = text.substr(prefix[23], prefix[24]);
    auto fieldName = text.substr(prefix[23] + prefix[24]);
    if (!valid_name(queueName) || !valid_name(messageName) || !valid_name(fieldName))
    {
      throw std::runtime_error("invalid session log name");
    }
    return {size, maxLogBytes, schemaId, std::move(queueName), std::move(messageName), std::move(fieldName)};
  }

  std::uint64_t SessionLog::records_end(const LogHeader& header) const
  {
    const auto recordBytes = file_size() - header._size;
    return header._size + (recordBytes / SESSION_RECORD_SIZE) * SESSION_RECORD_SIZE;
  }

  LogRecord SessionLog::read_record(const LogHeader& header, std::uint64_t offset) const
  {
    if (offset < header._size || (offset - header._size) % SESSION_RECORD_SIZE != 0 ||
        offset >= records_end(header))
    {
      throw std::runtime_error("invalid message offset");
    }
    std::array<std::uint8_t, SESSION_RECORD_SIZE> bytes{};
    read_bytes(offset, bytes);
    const auto sequence = get_le<std::uint64_t>(bytes, 4);
    if (get_le<std::uint32_t>(bytes, 0) != SESSION_RECORD_SIZE ||
        sequence != 1 + (offset - header._size) / SESSION_RECORD_SIZE ||
        get_le<std::uint16_t>(bytes, 20) != header._schemaId ||
        get_le<std::uint32_t>(bytes, 26) != COMMIT_MARKER)
    {
      throw std::runtime_error("invalid session log record");
    }

    return {
        sequence,
        std::bit_cast<std::int64_t>(get_le<std::uint64_t>(bytes, 12)),
        std::bit_cast<std::int32_t>(get_le<std::uint32_t>(bytes, 22)),
    };
  }

  std::optional<std::uint64_t> SessionLog::append_reading(const LogHeader& header, std::int32_t value) const
  {
    const auto offset = records_end(header);
    if (header._maxLogBytes - offset < SESSION_RECORD_SIZE)
    {
      return std::nullopt;
    }
    if (offset != header._size)
    {
      read_record(header, offset - SESSION_RECORD_SIZE);
    }

    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    std::array<std::uint8_t, SESSION_RECORD_SIZE> bytes{};
    put_le(bytes, 0, static_cast<std::uint32_t>(SESSION_RECORD_SIZE));
    put_le(bytes, 4, 1 + (offset - header._size) / SESSION_RECORD_SIZE);
    put_le(bytes, 12, std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(timestamp)));
    put_le(bytes, 20, header._schemaId);
    put_le(bytes, 22, std::bit_cast<std::uint32_t>(value));
    put_le(bytes, 26, COMMIT_MARKER);
    // A previous partial write occupies fewer bytes than this record. Rewrite it
    // from its start and write the commit marker only after the body.
    const auto record = std::span(bytes);
    write_bytes(offset, record.first(26));
    write_bytes(offset + 26, record.last(4));
    return offset;
  }
} // namespace angaria::detail
