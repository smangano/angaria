#include "angaria/session.h"
#include "session_log.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <ostream>
#include <stdexcept>
#include <system_error>

#include <sys/stat.h>

namespace angaria
{
  namespace
  {
    namespace fs = std::filesystem;

    constexpr char PENDING_DIRECTORY[] = "pending";
    constexpr std::size_t QUEUE_CAPACITY = 2;

    struct PendingQueue
    {
      std::array<std::uint64_t, QUEUE_CAPACITY> _offsets;
      std::size_t _size;
    };

    PendingQueue read_queue(const fs::path& path)
    {
      PendingQueue queue{};
      for (const auto& entry : fs::directory_iterator(path))
      {
        const auto name = entry.path().filename().string();
        const auto end = name.data() + name.size();
        std::uint64_t offset{};
        const auto result = std::from_chars(name.data(), end, offset);
        if (queue._size == QUEUE_CAPACITY || result.ec != std::errc{} || result.ptr != end ||
            name != std::to_string(offset) || !fs::is_directory(entry.symlink_status()))
        {
          throw std::runtime_error("invalid pending queue entry");
        }
        queue._offsets[queue._size++] = offset;
      }
      return queue;
    }
  } // namespace

  void create_session(const fs::path& session, std::uint64_t maxLogBytes)
  {
    if (::mkdir(session.c_str(), 0700) == -1)
    {
      throw std::system_error(errno, std::generic_category(), "create session directory");
    }

    const auto logPath = session / detail::SESSION_LOG_NAME;
    try
    {
      detail::SessionLog log(logPath, detail::LogMode::Create);
      log.write_header(session.filename().string(), maxLogBytes);
      fs::create_directory(session / PENDING_DIRECTORY);
    }
    catch (...)
    {
      std::error_code ignored;
      fs::remove(logPath, ignored);
      fs::remove(session, ignored);
      throw;
    }
  }

  SendResult send_reading(const fs::path& session, std::int32_t value)
  {
    detail::SessionLog log(session / detail::SESSION_LOG_NAME, detail::LogMode::Update);
    const auto header = log.read_header();
    const auto path = session / PENDING_DIRECTORY;
    if (read_queue(path)._size == QUEUE_CAPACITY)
    {
      return SendResult::QueueFull;
    }

    const auto offset = log.append_reading(header, value);
    if (!offset)
    {
      return SendResult::LogFull;
    }

    // Directory creation publishes the offset in one filesystem operation.
    if (!fs::create_directory(path / std::to_string(*offset)))
    {
      throw std::runtime_error("message offset already published");
    }
    return SendResult::Sent;
  }

  std::optional<std::int32_t> receive_reading(const fs::path& session)
  {
    detail::SessionLog log(session / detail::SESSION_LOG_NAME, detail::LogMode::Update);
    const auto header = log.read_header();
    const auto path = session / PENDING_DIRECTORY;
    const auto queue = read_queue(path);
    if (queue._size == 0)
    {
      return std::nullopt;
    }

    const auto end = queue._offsets.begin() + queue._size;
    const auto offset = *std::min_element(queue._offsets.begin(), end);
    const auto record = log.read_record(header, offset);
    if (!fs::remove(path / std::to_string(offset)))
    {
      throw std::runtime_error("pending queue entry disappeared");
    }
    return record._value;
  }

  void inspect_session(const fs::path& logPath, std::ostream& output)
  {
    detail::SessionLog log(logPath, detail::LogMode::Inspect);
    const auto header = log.read_header();
    const auto end = log.records_end(header);
    for (auto offset = header._size; offset < end; offset += detail::SESSION_RECORD_SIZE)
    {
      const auto record = log.read_record(header, offset);
      output << "queue=" << header._queueName << " sequence=" << record._sequence
             << " timestamp_ns=" << record._timestampNs << " type=" << header._messageName << ' '
             << header._fieldName << '=' << record._value << '\n';
    }
  }

  void remove_queue(const fs::path& session)
  {
    detail::SessionLog log(session / detail::SESSION_LOG_NAME, detail::LogMode::Update);
    log.read_header();
    const auto path = session / PENDING_DIRECTORY;
    if (fs::is_directory(fs::symlink_status(path)))
    {
      const auto queue = read_queue(path);
      for (std::size_t index = 0; index < queue._size; ++index)
      {
        fs::remove(path / std::to_string(queue._offsets[index]));
      }
      fs::remove(path);
    }
  }
} // namespace angaria
