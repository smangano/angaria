// Independent exec process for the private recorded-SPSC experiment. Session,
// ownership and accepted publication use public APIs. Only partial_append uses
// the existing journal injection seam to stop at a real uncommitted record.
#include "angaria/runtime.h"

#include "native_fixture.h"
#include "process_support.h"
#include "publication_runtime.h"
#include "recorded_spsc.h"
#include "transport_support.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

#include <unistd.h>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;
  namespace support = angaria::test_support;
  namespace spsc = angaria::benchmarks::spsc;

  class StopBeforeCommit final : public mapped::JournalFaultInjector
  {
  public:
    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t index) override
    {
      if (point == mapped::JournalCheckpoint::RecordReady && index == 0)
      {
        support::stop_for_kill();
      }
      return std::monostate{};
    }
  };
}

int main(int argc, char** argv)
{
  try
  {
    if (argc != 3)
    {
      throw std::runtime_error("usage: recorded_spsc_worker directory commit_before_publish|partial_append");
    }
    ::alarm(15);
    const std::filesystem::path directory{argv[1]};
    const std::string mode{argv[2]};
    if (mode != "commit_before_publish" && mode != "partial_append")
    {
      throw std::runtime_error("Unknown recorded-SPSC crash checkpoint");
    }
    auto session = support::require_value(Session::attach(directory));
    auto registry = session.registry();
    const auto producerId = support::require_value(registry.find_task("Producer"));
    const auto queue = support::require_value(registry.find_queue("Updates"));
    const auto schema = support::require_value(registry.find_message("Sample", 1));
    auto producer = support::require_value(session.attach_task(producerId));
    auto writer = support::require_value(producer.open_writer(queue));

    spsc::Options options;
    options._directory = directory;
    options._session = session.identity();
    options._queue = queue;
    options._producer = producerId;
    options._schema = schema;
    options._payloadBytes = 4;
    options._capacity = 2;
    options._maxLogBytes = support::require_value(session.options())._maxLogBytes;
    const auto claim = producer.claim();
    spsc::RecordedSpsc connection(options, spsc::Role::Producer, claim);
    if (!connection.has_credit())
    {
      throw std::runtime_error("Fresh recorded-SPSC crash scenario unexpectedly has no ring credit");
    }
    const auto publication = support::require_value(producer.next_publication_id());
    const auto reference = connection.next_record_reference();
    // The fixture's Sample schema is a single ordinary UInt32: these are the
    // independent little-endian bytes of 99, not an unlogged synthetic message.
    const std::array payload{std::byte{99}, std::byte{0}, std::byte{0}, std::byte{0}};
    MessageHeader header;
    header._schema = schema;
    header._payloadBytes = payload.size();
    std::array<DestinationOutcome, 1> outcomes;
    if (mode == "partial_append")
    {
      StopBeforeCommit stop;
      runtime::RuntimeAccess access(support::open_runtime_state(directory), true, &stop);
      (void)runtime::runtime_send(access, claim, queue, publication,
          MessageView{header, payload}, outcomes);
      throw std::runtime_error("Recorded-SPSC partial append did not reach its selected checkpoint");
    }
    const auto result = writer.send(publication, MessageView{header, payload}, outcomes);
    angaria::benchmarks::check_report(result, publication, queue);
    // Deliberately omit publish_committed(reference, publication). The parent
    // kills this owner after R, then checks live catch-up and owning replay of
    // the authoritative accepted log record despite its missing descriptor.
    (void)reference;
    support::stop_for_kill();
  }
  catch (const angaria::benchmarks::BenchmarkFailure& failure)
  {
    std::cerr << failure._error._path << ": " << failure._error._message << '\n';
    return 1;
  }
  catch (...)
  {
    const auto error = runtime::runtime_caught_error();
    std::cerr << error._path << ": " << error._message << '\n';
    return 1;
  }
}
