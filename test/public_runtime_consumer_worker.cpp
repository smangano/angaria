// Private process-test entry point using only the public messaging API. The
// parent terminates this process after receive returns, before any ACK/close.
#include "angaria/runtime.h"
#include "result_helpers.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <variant>

#include <unistd.h>

namespace
{
  std::uint64_t number(std::string_view text)
  {
    std::uint64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
      throw std::runtime_error("Invalid private process-test argument");
    }
    return value;
  }
}

int main(int argc, char** argv)
{
  using namespace angaria;
  using angaria::test_support::require_value;
  try
  {
    if (argc != 5)
    {
      throw std::runtime_error("Expected Session directory, Task ID, Source ID, and ready FD");
    }
    const auto task = number(argv[2]);
    const auto sourceId = number(argv[3]);
    const auto channel = number(argv[4]);
    if (task == 0 || task > 65535 || sourceId == 0 || sourceId > 65535 || channel > 2147483647)
    {
      throw std::runtime_error("Private process-test argument exceeds its native type");
    }
    auto session = require_value(Session::attach(argv[1]));
    auto consumer = require_value(session.attach_task(TaskId{static_cast<std::uint16_t>(task)}));
    auto source = require_value(consumer.open_source_set(SourceQueueSetId{static_cast<std::uint16_t>(sourceId)}));
    const auto received = source.receive();
    if (const auto* error = std::get_if<Error>(&received))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    if (!std::holds_alternative<Delivery>(received))
    {
      throw std::runtime_error("Public process-test consumer expected an acknowledged Delivery");
    }
    const auto& token = std::get<Delivery>(received)._token;
    const std::array<std::uint64_t, 8> words{
        token._consumer._session._high, token._consumer._session._low,
        static_cast<std::uint16_t>(token._consumer._task), token._consumer._incarnation,
        static_cast<std::uint16_t>(token._sourceSet), static_cast<std::uint16_t>(token._queue),
        token._sequence, token._issuance};
    if (::write(static_cast<int>(channel), words.data(), sizeof(words)) != static_cast<ssize_t>(sizeof(words)))
    {
      throw std::runtime_error("Public process-test consumer could not report its pin");
    }
    for (;;)
    {
      ::pause();
    }
  }
  catch (const std::exception& error)
  {
    std::cerr << "Public consumer worker: " << error.what() << '\n';
    return 1;
  }
}
