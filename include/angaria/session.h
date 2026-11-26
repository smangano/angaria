#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <stdexcept>

namespace angaria
{
  namespace detail
  {
    class RoleLock;
  }

  inline constexpr std::uint64_t DEFAULT_SESSION_LOG_LIMIT = 1024 * 1024;

  enum class SendResult
  {
    Sent,
    QueueFull,
    LogFull
  };

  class RoleBusy : public std::runtime_error
  {
  public:
    explicit RoleBusy(const char* message);
  };

  struct RoleIdentity
  {
    std::uint16_t _taskId;
    std::uint64_t _incarnation;
    std::uint64_t _pid;
  };

  // The legacy v0 integer-reading demo predates the native Message/Delivery
  // model. Keep its value type distinct when both APIs are included together.
  struct LegacyDelivery
  {
    std::uint64_t _sequence;
    std::int64_t _timestampNs;
    std::int32_t _value;
  };

  // Each endpoint belongs to its attaching process and is used by one thread.
  class Producer
  {
  public:
    explicit Producer(const std::filesystem::path& session);
    ~Producer();
    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;

    RoleIdentity identity() const;
    SendResult send_reading(std::int32_t value);

  private:
    std::filesystem::path _session;
    std::unique_ptr<detail::RoleLock> _role;
  };

  class Consumer
  {
  public:
    explicit Consumer(const std::filesystem::path& session);
    ~Consumer();
    Consumer(const Consumer&) = delete;
    Consumer& operator=(const Consumer&) = delete;

    RoleIdentity identity() const;
    std::optional<LegacyDelivery> receive_reading(std::chrono::milliseconds timeout = std::chrono::milliseconds{0});
    void acknowledge(std::uint64_t sequence);

  private:
    std::filesystem::path _session;
    std::unique_ptr<detail::RoleLock> _role;
    std::optional<LegacyDelivery> _inFlight;
    std::uint64_t _inFlightOffset{};
  };

  void create_session(const std::filesystem::path& session,
                      std::uint64_t maxLogBytes = DEFAULT_SESSION_LOG_LIMIT);
  SendResult send_reading(const std::filesystem::path& session, std::int32_t value);
  void inspect_session(const std::filesystem::path& logPath, std::ostream& output);
  void remove_queue(const std::filesystem::path& session);
} // namespace angaria
