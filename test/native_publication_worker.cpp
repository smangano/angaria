// Independent process crash seam for the native send engine. Setup and Task
// ownership use public APIs. Only the final gate-bound call is private so this
// worker can stop at actual journal stores, which the public API cannot inject.

#include "angaria/runtime.h"
#include "native_fixture.h"
#include "process_support.h"
#include "publication_runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

#include <unistd.h>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;
  namespace support = angaria::test_support;

  class StopAt final : public mapped::JournalFaultInjector
  {
  public:
    StopAt(mapped::JournalCheckpoint point, std::uint64_t index) : _point(point), _index(index)
    {
    }

    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t index) override
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
} // namespace

int main(int argc, char** argv)
{
  try
  {
    if (argc != 5)
    {
      throw std::runtime_error("usage: native_publication_worker directory queue checkpoint index");
    }
    ::alarm(8);
    auto session = support::require_value(Session::attach(argv[1]));
    auto producer = support::require_value(session.attach_task(TaskId{1}));
    const auto queue = static_cast<QueueId>(std::stoul(argv[2]));
    auto writer = support::require_value(producer.open_writer(queue));
    (void)writer.id();
    const auto publication = support::require_value(producer.next_publication_id());
    const auto claim = producer.claim();
    support::report('I');
    support::report(publication._producer._incarnation);
    support::report(publication._sequence);

    const std::array payload{std::byte{0x61}, std::byte{0}, std::byte{0xff}};
    MessageHeader header;
    header._schema = SchemaId{MessageType{1}, 1};
    header._format = PayloadFormat::Text;
    header._payloadBytes = payload.size();
    StopAt stop(static_cast<mapped::JournalCheckpoint>(std::stoul(argv[3])), std::stoull(argv[4]));
    runtime::RuntimeAccess access(support::open_runtime_state(argv[1]), true, &stop);
    std::array<DestinationOutcome, 1> outcomes;
    const auto result = runtime::runtime_send(access, claim, queue, publication,
        MessageView{header, payload}, outcomes);
    std::cerr << "Publication worker did not reach selected checkpoint; result "
              << static_cast<unsigned>(result._error) << '\n';
    return 2;
  }
  catch (...)
  {
    const auto error = runtime::runtime_caught_error();
    std::cerr << error._path << ": " << error._message << '\n';
    return 1;
  }
}
