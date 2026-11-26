// Private journal checkpoints wrap a real public allowed-loss receive. Recovery
// mode deliberately stops replay again; neither mode is an application API.
#include "angaria/runtime.h"
#include "native_fixture.h"
#include "process_support.h"
#include "source_runtime_state.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include <unistd.h>

namespace
{
  namespace mapped = angaria::detail::mapped;
  namespace support = angaria::test_support;

  class StopAt final : public mapped::JournalFaultInjector
  {
  public:
    StopAt(mapped::JournalCheckpoint point, std::uint64_t index) : _point(point), _index(index)
    {
    }

    angaria::OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t index) override
    {
      if (point == _point && index == _index)
      {
        support::stop_for_kill();
      }
      return std::monostate{};
    }

  private:
    mapped::JournalCheckpoint _point;
    std::uint64_t _index;
  };
}

int main(int argc, char** argv)
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace support = angaria::test_support;
  try
  {
    if (argc != 5)
    {
      throw std::runtime_error("usage: native_loss_worker directory receive|recover checkpoint index");
    }
    ::alarm(8);
    StopAt stop(static_cast<runtime::mapped::JournalCheckpoint>(std::stoul(argv[3])), std::stoull(argv[4]));
    if (std::string_view{argv[2]} == "recover")
    {
      runtime::RuntimeAccess access(support::open_runtime_state(argv[1]), true, &stop);
      throw std::runtime_error("Loss recovery did not reach the selected checkpoint");
    }
    auto session = support::require_value(Session::attach(argv[1]));
    auto consumer = support::require_value(session.attach_task("Consumer"));
    runtime::SourceOperationHooks hooks;
    hooks._journal = &stop;
    auto source = support::require_value(consumer.open_source_set("Inputs"));
    runtime::SourceAccess::set_hooks(source, &hooks);
    const auto event = source.receive();
    if (const auto* error = std::get_if<Error>(&event))
    {
      std::cerr << error->_path << ": " << error->_message << '\n';
    }
    throw std::runtime_error("Loss receive did not reach the selected checkpoint");
  }
  catch (...)
  {
    const auto error = runtime::runtime_caught_error();
    std::cerr << error._path << ": " << error._message << '\n';
    return 1;
  }
}
