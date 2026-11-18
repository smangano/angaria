#include "angaria/session.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
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

  int send_value(angaria::Producer& producer, std::int32_t value)
  {
    switch (producer.send_reading(value))
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
    throw std::logic_error("unrecognized send result");
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
      angaria::Producer producer(path);
      return send_value(producer, value);
    }
    else if (command == "produce")
    {
      angaria::Producer producer(path);
      std::string line;
      while (std::getline(std::cin, line))
      {
        const auto value = parse_integer<std::int32_t>(line, "value must be a signed 32-bit integer");
        const int status = send_value(producer, value);
        if (status != 0)
        {
          return status;
        }
        std::cout << "Sent" << std::endl;
      }
    }
    else if (command == "receive")
    {
      const auto timeout = argument == nullptr
                               ? 0
                               : parse_integer<std::int64_t>(argument, "timeout must be a millisecond count");
      angaria::Consumer consumer(path);
      const auto delivery = consumer.receive_reading(std::chrono::milliseconds{timeout});
      if (!delivery)
      {
        throw std::runtime_error("queue is empty");
      }
      std::cout << "Reading sequence=" << delivery->_sequence << " value=" << delivery->_value << std::endl;
      std::string response;
      if (std::getline(std::cin, response) && response == "ack")
      {
        consumer.acknowledge(delivery->_sequence);
      }
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
      ((command == "receive") && (argc == 3 || argc == 4)) ||
      ((command == "produce" || command == "remove" || command == "inspect") && argc == 3);
  if (!validCommand)
  {
    std::cerr << "Usage: angaria_queue_demo create SESSION [MAX_LOG_BYTES]\n"
                 "       angaria_queue_demo send SESSION VALUE\n"
                 "       angaria_queue_demo produce SESSION\n"
                 "       angaria_queue_demo receive SESSION [WAIT_MS]\n"
                 "       angaria_queue_demo remove SESSION\n"
                 "       angaria_queue_demo inspect LOG_FILE\n";
    return 2;
  }

  try
  {
    return run_command(command, argv[2], argc == 4 ? argv[3] : nullptr);
  }
  catch (const angaria::RoleBusy& error)
  {
    std::cerr << "angaria_queue_demo: " << error.what() << '\n';
    return 5;
  }
  catch (const std::exception& error)
  {
    std::cerr << "angaria_queue_demo: " << error.what() << '\n';
    return 1;
  }
}
