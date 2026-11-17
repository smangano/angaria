#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>

namespace angaria
{
  inline constexpr std::uint64_t DEFAULT_SESSION_LOG_LIMIT = 1024 * 1024;

  enum class SendResult
  {
    Sent,
    QueueFull,
    LogFull
  };

  void create_session(const std::filesystem::path& session,
                      std::uint64_t maxLogBytes = DEFAULT_SESSION_LOG_LIMIT);
  SendResult send_reading(const std::filesystem::path& session, std::int32_t value);
  std::optional<std::int32_t> receive_reading(const std::filesystem::path& session);
  void inspect_session(const std::filesystem::path& logPath, std::ostream& output);
  void remove_queue(const std::filesystem::path& session);
} // namespace angaria
