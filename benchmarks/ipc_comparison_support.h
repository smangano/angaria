#pragma once

// Benchmark-only wire, control and clock helpers. Data-plane buffers are owned
// by the adapter; successful fill/validation/hash calls allocate no storage.
#include "angaria/detail/byte_operations.h"
#include "angaria/detail/checked_arithmetic.h"
#include "angaria/detail/text_escaping.h"
#include "transport_support.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <time.h>

namespace angaria::benchmarks::ipc
{
  inline constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000;
  inline constexpr std::uint64_t nanoseconds_per_millisecond = 1'000'000;
  inline constexpr std::size_t envelope_bytes = 32;

  enum class Profile
  {
    Angaria,
    Nanomsg
  };

  enum class Role
  {
    Setup,
    Producer,
    Consumer
  };

  inline std::string_view profile_name(Profile profile)
  {
    return profile == Profile::Angaria ? "angaria" : "nanomsg";
  }

  inline std::string_view role_name(Role role)
  {
    switch (role)
    {
      case Role::Setup: return "setup";
      case Role::Producer: return "producer";
      case Role::Consumer: return "consumer";
    }
    require(false, "Unknown IPC benchmark role");
    return {};
  }

  struct Options
  {
    Profile _profile = Profile::Angaria;
    Role _role = Role::Setup;
    std::filesystem::path _directory;
    std::uint64_t _messages = 16;
    std::uint64_t _history = 0;
    std::uint64_t _payloadBytes = 64;
    std::uint64_t _budgetMs = 10'000;
    std::uint64_t _socketBufferBytes = 131'072;
    std::uint64_t _intervalNs = 0;
  };

  inline bool valid_payload_size(std::size_t size)
  {
    return size == 64 || size == 256 || size == 1024;
  }

  inline Options parse_options(int argc, char** argv)
  {
    Options options;
    bool haveProfile{};
    bool haveRole{};
    bool haveDirectory{};
    for (int index = 1; index < argc; ++index)
    {
      const std::string_view flag{argv[index]};
      require(index + 1 < argc, "IPC benchmark option is missing its value");
      const std::string_view argument{argv[++index]};
      if (flag == "--profile")
      {
        require(!haveProfile && (argument == "angaria" || argument == "nanomsg"),
                "Expected one --profile angaria|nanomsg");
        options._profile = argument == "angaria" ? Profile::Angaria : Profile::Nanomsg;
        haveProfile = true;
      }
      else if (flag == "--role")
      {
        require(!haveRole && (argument == "setup" || argument == "producer" || argument == "consumer"),
                "Expected one --role setup|producer|consumer");
        options._role = argument == "setup" ? Role::Setup :
                        (argument == "producer" ? Role::Producer : Role::Consumer);
        haveRole = true;
      }
      else if (flag == "--directory")
      {
        require(!haveDirectory && !argument.empty(), "Expected one nonempty --directory");
        options._directory = std::filesystem::path{argument};
        haveDirectory = true;
      }
      else if (flag == "--messages")
      {
        options._messages = parse_count(argument, flag);
      }
      else if (flag == "--history")
      {
        options._history = parse_count(argument, flag);
      }
      else if (flag == "--payload-bytes")
      {
        options._payloadBytes = parse_count(argument, flag);
      }
      else if (flag == "--budget-ms")
      {
        options._budgetMs = parse_count(argument, flag);
      }
      else if (flag == "--socket-buffer-bytes")
      {
        options._socketBufferBytes = parse_count(argument, flag);
      }
      else if (flag == "--interval-ns")
      {
        options._intervalNs = parse_count(argument, flag);
      }
      else
      {
        require(false, "Unknown IPC benchmark option");
      }
    }
    require(haveProfile && haveRole && haveDirectory, "IPC benchmark requires profile, role and directory");
    require(options._messages >= 1 && options._messages <= 128, "Messages must be in 1..128");
    require(options._history <= 128, "History must be in 0..128");
    require(options._payloadBytes == 64 || options._payloadBytes == 256 || options._payloadBytes == 1024,
            "Payload bytes must be 64, 256 or 1024");
    require(options._budgetMs >= 1 && options._budgetMs <= 600'000, "Budget ms must be in 1..600000");
    require(options._socketBufferBytes >= 1024 &&
                options._socketBufferBytes <= static_cast<std::uint64_t>(std::numeric_limits<int>::max()),
            "Socket buffer bytes must be in 1024..INT_MAX");
    require(options._intervalNs <= nanoseconds_per_second, "Interval ns must be in 0..1000000000");
    const auto lastScheduledOffset = detail::checked_multiply(options._messages - 1, options._intervalNs);
    const auto budgetNs = detail::checked_multiply(options._budgetMs, nanoseconds_per_millisecond);
    require(lastScheduledOffset.has_value() && budgetNs.has_value() && *lastScheduledOffset < *budgetNs,
            "The last scheduled message must precede the phase deadline");
    return options;
  }

  inline std::uint64_t checked_add(std::uint64_t first, std::uint64_t second)
  {
    const auto result = detail::checked_add(first, second);
    require(result.has_value(), "IPC benchmark timestamp addition overflow");
    return *result;
  }

  inline std::uint64_t checked_multiply(std::uint64_t first, std::uint64_t second)
  {
    const auto result = detail::checked_multiply(first, second);
    require(result.has_value(), "IPC benchmark timestamp multiplication overflow");
    return *result;
  }

  inline std::uint64_t timespec_ns(const timespec& value)
  {
    require(value.tv_sec >= 0 && value.tv_nsec >= 0 && value.tv_nsec < 1'000'000'000,
            "IPC benchmark received an invalid clock value");
    return checked_add(checked_multiply(static_cast<std::uint64_t>(value.tv_sec), nanoseconds_per_second),
                       static_cast<std::uint64_t>(value.tv_nsec));
  }

  inline std::uint64_t monotonic_ns()
  {
    timespec value{};
    require(::clock_gettime(CLOCK_MONOTONIC, &value) == 0, "CLOCK_MONOTONIC read failed");
    return timespec_ns(value);
  }

  inline std::uint64_t realtime_ns()
  {
    timespec value{};
    require(::clock_gettime(CLOCK_REALTIME, &value) == 0, "CLOCK_REALTIME read failed");
    return timespec_ns(value);
  }

  inline std::uint64_t clock_resolution_ns()
  {
    timespec value{};
    require(::clock_getres(CLOCK_MONOTONIC, &value) == 0, "CLOCK_MONOTONIC resolution read failed");
    return timespec_ns(value);
  }

  inline std::uint64_t timeval_ns(const timeval& value)
  {
    require(value.tv_sec >= 0 && value.tv_usec >= 0 && value.tv_usec < 1'000'000,
            "IPC benchmark received an invalid CPU time");
    return checked_add(checked_multiply(static_cast<std::uint64_t>(value.tv_sec), nanoseconds_per_second),
                       checked_multiply(static_cast<std::uint64_t>(value.tv_usec), 1000));
  }

  // RUSAGE_SELF includes all threads in this process, including library workers.
  inline std::uint64_t process_cpu_ns()
  {
    rusage usage{};
    require(::getrusage(RUSAGE_SELF, &usage) == 0, "Process CPU read failed");
    return checked_add(timeval_ns(usage.ru_utime), timeval_ns(usage.ru_stime));
  }

  inline std::uint64_t peak_rss_bytes()
  {
    rusage usage{};
    require(::getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss >= 0, "Peak RSS read failed");
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return checked_multiply(static_cast<std::uint64_t>(usage.ru_maxrss), 1024);
#endif
  }

  // Metadata only, outside the timed loop. Platforms without this source report
  // null rather than implying that a library has no background threads.
  inline std::optional<std::uint64_t> thread_count()
  {
#if defined(__linux__)
    std::ifstream status{"/proc/self/status"};
    std::string line;
    while (std::getline(status, line))
    {
      if (line.starts_with("Threads:"))
      {
        const auto first = line.find_first_not_of(" \t", 8);
        if (first != std::string::npos)
        {
          return parse_count(std::string_view{line}.substr(first), "Threads");
        }
      }
    }
#endif
    return std::nullopt;
  }

  inline std::uint64_t deadline_ns(std::uint64_t startNs, std::uint64_t budgetMs)
  {
    return checked_add(startNs, checked_multiply(budgetMs, nanoseconds_per_millisecond));
  }

  // A work budget is an expected incomplete outcome. Transport, payload and
  // other failures retain their own cause even if they happen to finish late.
  class DeadlineExceeded final : public std::exception
  {
  public:
    const char* what() const noexcept override
    {
      return "IPC benchmark deadline exceeded";
    }
  };

  inline void check_deadline(std::uint64_t deadlineNs)
  {
    if (monotonic_ns() >= deadlineNs)
    {
      throw DeadlineExceeded{};
    }
  }

  inline int remaining_ms(std::uint64_t deadlineNs)
  {
    const auto now = monotonic_ns();
    if (now >= deadlineNs)
    {
      throw DeadlineExceeded{};
    }
    const auto remaining = deadlineNs - now;
    const auto milliseconds = remaining / nanoseconds_per_millisecond +
                              (remaining % nanoseconds_per_millisecond != 0 ? 1 : 0);
    return static_cast<int>(std::min(milliseconds,
        static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
  }

  inline std::uint64_t wait_until(std::uint64_t targetNs, std::uint64_t deadlineNs)
  {
    require(targetNs < deadlineNs, "IPC benchmark target is outside its deadline");
    while (true)
    {
      const auto now = monotonic_ns();
      if (now >= deadlineNs)
      {
        throw DeadlineExceeded{};
      }
      if (now >= targetNs)
      {
        return now;
      }
      const auto remaining = targetNs - now;
      timespec pause{};
      pause.tv_sec = static_cast<time_t>(remaining / nanoseconds_per_second);
      pause.tv_nsec = static_cast<long>(remaining % nanoseconds_per_second);
      require(::nanosleep(&pause, nullptr) == 0 || errno == EINTR, "IPC benchmark wait failed");
    }
  }

  struct Envelope
  {
    std::uint64_t _sequence{};
    std::uint64_t _scheduledNs{};
    std::uint64_t _logicalStartNs{};
  };

  inline void fill_payload(std::span<std::byte> bytes, std::uint64_t sequence,
                           std::uint64_t scheduledNs, std::uint64_t logicalStartNs)
  {
    require(valid_payload_size(bytes.size()) && sequence != 0, "Invalid IPC benchmark payload geometry");
    detail::write_unsigned_le<std::uint16_t>(bytes, 0, 1);
    detail::write_unsigned_le<std::uint16_t>(bytes, 2, 1);
    detail::write_unsigned_le<std::uint32_t>(bytes, 4, 0);
    detail::write_unsigned_le(bytes, 8, sequence);
    detail::write_unsigned_le(bytes, 16, scheduledNs);
    detail::write_unsigned_le(bytes, 24, logicalStartNs);
    for (std::size_t index = envelope_bytes; index < bytes.size(); ++index)
    {
      bytes[index] = static_cast<std::byte>((sequence + index) & 0xff);
    }
  }

  inline Envelope validate_payload(std::span<const std::byte> bytes, std::uint64_t expectedSequence)
  {
    require(valid_payload_size(bytes.size()), "Unexpected IPC benchmark payload length");
    require(detail::read_unsigned_le<std::uint16_t>(bytes, 0) == 1 &&
                detail::read_unsigned_le<std::uint16_t>(bytes, 2) == 1 &&
                detail::read_unsigned_le<std::uint32_t>(bytes, 4) == 0,
            "Invalid IPC benchmark envelope version, producer or flags");
    Envelope envelope{detail::read_unsigned_le<std::uint64_t>(bytes, 8),
                      detail::read_unsigned_le<std::uint64_t>(bytes, 16),
                      detail::read_unsigned_le<std::uint64_t>(bytes, 24)};
    require(expectedSequence != 0 && envelope._sequence == expectedSequence,
            "IPC benchmark sequence is missing, duplicated or out of order");
    for (std::size_t index = envelope_bytes; index < bytes.size(); ++index)
    {
      require(bytes[index] == static_cast<std::byte>((expectedSequence + index) & 0xff),
              "IPC benchmark payload body mismatch");
    }
    return envelope;
  }

  inline std::uint64_t payload_hash(std::span<const std::byte> bytes)
  {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (const auto byte : bytes)
    {
      hash ^= std::to_integer<unsigned char>(byte);
      hash *= 1'099'511'628'211ULL;
    }
    return hash;
  }

  inline std::string hash_hex(std::uint64_t hash)
  {
    std::string result(16, '0');
    for (std::size_t index = 0; index < result.size(); ++index)
    {
      result[result.size() - index - 1] = detail::hex_digit(static_cast<unsigned>(hash & 0xf));
      hash >>= 4;
    }
    return result;
  }

  struct Command
  {
    std::string _cmd;
    std::uint64_t _nonce{};
    std::uint64_t _t0Ns{};
  };

  inline Command read_command()
  {
    std::array<char, 257> lineBuffer{};
    std::cin.getline(lineBuffer.data(), static_cast<std::streamsize>(lineBuffer.size()));
    require(std::cin.good(), "IPC benchmark control input ended early or exceeded 256 bytes");
    // Successful getline consumed its newline; retain any embedded NUL so that
    // command matching rejects it rather than accepting a truncated command.
    std::string line{lineBuffer.data(), static_cast<std::size_t>(std::cin.gcount() - 1)};
    if (!line.empty() && line.back() == '\r')
    {
      line.pop_back();
    }
    const auto separator = line.find(' ');
    const std::string_view name{line.data(), separator == std::string::npos ? line.size() : separator};
    Command command{std::string{name}, 0, 0};
    if (name == "CLOCK" || name == "START")
    {
      require(separator != std::string::npos, "IPC benchmark control command needs an integer");
      const auto argument = std::string_view{line}.substr(separator + 1);
      const auto number = parse_count(argument, name);
      if (name == "CLOCK")
      {
        command._nonce = number;
      }
      else
      {
        command._t0Ns = number;
      }
    }
    else
    {
      require(separator == std::string::npos &&
                  (name == "PRELOAD" || name == "READY" || name == "FINISH"),
              "Unknown or malformed IPC benchmark control command");
    }
    return command;
  }

  inline std::string json_string(std::string_view value)
  {
    std::string result{"\""};
    for (const auto valueByte : value)
    {
      const auto character = static_cast<unsigned char>(valueByte);
      switch (character)
      {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
          if (character < 0x20)
          {
            result += "\\u00";
            result += detail::hex_digit(character >> 4);
            result += detail::hex_digit(character);
          }
          else
          {
            result += static_cast<char>(character);
          }
      }
    }
    result += '"';
    return result;
  }

  inline void write_record(std::string_view json)
  {
    std::cout << json << '\n' << std::flush;
    require(std::cout.good(), "IPC benchmark control output failed");
  }

  // extraFields is adapter-owned JSON member text, without a leading comma.
  // Records are emitted only before or after the measured data phase.
  inline void emit_control(std::string_view event, Role role, std::string_view extraFields = {})
  {
    std::string json = "{\"event\":" + json_string(event) + ",\"role\":" + json_string(role_name(role));
    if (!extraFields.empty())
    {
      json += ',';
      json += extraFields;
    }
    json += '}';
    write_record(json);
  }
} // namespace angaria::benchmarks::ipc
