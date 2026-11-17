#include "angaria/session.h"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace
{
  template <typename Integer>
  Integer parse_integer(std::string_view text, const char* error)
  {
    Integer value{};
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end)
    {
      throw std::invalid_argument(error);
    }
    return value;
  }

  int run_command(std::string_view command, const std::filesystem::path& path, const char* argument)
  {
    if (command == "create")
    {
      const auto maxLogBytes = argument == nullptr
                                   ? angaria::DEFAULT_SESSION_LOG_LIMIT
                                   : parse_integer<std::uint64_t>(argument, "log limit must be a byte count");
      angaria::create_session(path, maxLogBytes);
    }
    else if (command == "send")
    {
      const auto value = parse_integer<std::int32_t>(argument, "value must be a signed 32-bit integer");
      switch (angaria::send_reading(path, value))
      {
      case angaria::SendResult::Sent:
        return 0;
      case angaria::SendResult::QueueFull:
        std::cerr << "angaria_queue_demo: queue is full\n";
        return 3;
      case angaria::SendResult::LogFull:
        std::cerr << "angaria_queue_demo: session log is full\n";
        return 4;
      }
    }
    else if (command == "receive")
    {
      const auto value = angaria::receive_reading(path);
      if (!value)
      {
        throw std::runtime_error("queue is empty");
      }
      std::cout << "Reading value=" << *value << '\n';
    }
    else if (command == "inspect")
    {
      angaria::inspect_session(path, std::cout);
    }
    else
    {
      angaria::remove_queue(path);
    }
    return 0;
  }
} // namespace

int main(int argc, char** argv)
{
  const std::string_view command = argc > 1 ? argv[1] : "";
  const bool validCommand =
      (command == "create" && (argc == 3 || argc == 4)) || (command == "send" && argc == 4) ||
      ((command == "receive" || command == "remove" || command == "inspect") && argc == 3);
  if (!validCommand)
  {
    std::cerr << "Usage: angaria_queue_demo create SESSION [MAX_LOG_BYTES]\n"
                 "       angaria_queue_demo send SESSION VALUE\n"
                 "       angaria_queue_demo receive|remove SESSION\n"
                 "       angaria_queue_demo inspect LOG_FILE\n";
    return 2;
  }

  try
  {
    return run_command(command, argv[2], argc == 4 ? argv[3] : nullptr);
  }
  catch (const std::exception& error)
  {
    std::cerr << "angaria_queue_demo: " << error.what() << '\n';
    return 1;
  }
}
