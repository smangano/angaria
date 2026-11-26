// Independent consumer for public Source lifetime qualification. Its protocol
// over stdout is test coordination, not Angaria message encoding. All Session,
// receive, acknowledgment, release, and close operations below are public APIs.
#include "angaria/runtime.h"
#include "process_support.h"
#include "result_helpers.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include <unistd.h>

int main(int argc, char** argv)
{
  namespace support = angaria::test_support;
  using namespace angaria;
  try
  {
    if (argc != 4)
    {
      throw std::runtime_error("usage: native_source_worker directory source receive|acknowledge|release|close");
    }
    ::alarm(8);
    auto session = support::require_value(Session::attach(argv[1]));
    auto consumer = support::require_value(session.attach_task("Consumer"));
    auto source = support::require_value(consumer.open_source_set(
        SourceQueueSetId{static_cast<std::uint16_t>(std::stoul(argv[2]))}));
    auto result = source.receive();
    if (!std::holds_alternative<Delivery>(result))
    {
      throw std::runtime_error("Consumer worker did not receive the queued message");
    }
    const auto token = std::get<Delivery>(result)._token;
    const std::string_view operation{argv[3]};
    if (operation == "acknowledge")
    {
      support::require_value(source.acknowledge(token));
    }
    else if (operation == "release")
    {
      support::require_value(source.release(token));
    }
    else if (operation == "close")
    {
      support::require_value(source.close());
    }
    else if (operation != "receive")
    {
      throw std::runtime_error("Unknown Source worker operation");
    }
    support::report('A');
    support::report(token._consumer._incarnation);
    support::report(token._sequence);
    support::report(token._issuance);
    support::stop_for_kill();
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
