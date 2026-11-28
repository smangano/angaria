// Independent exec process for private producer-log qualification. Pipes carry
// test barriers only; accepted data always travels through the actual mapped log.
#include "producer_log_fixture.h"
#include "process_support.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include <unistd.h>

namespace
{
  namespace recorded = angaria::detail::recorded;
  namespace support = angaria::test_support;

  class PauseAtCheckpoint final : public recorded::ProducerLogObserver
  {
  public:
    explicit PauseAtCheckpoint(recorded::ProducerLogCheckpoint point) : _point(point)
    {
    }

    angaria::OperationResult checkpoint(recorded::ProducerLogCheckpoint point) override
    {
      if (!_stopped && point == _point)
      {
        _stopped = true;
        support::report('R');
        support::wait_for_parent();
      }
      return std::monostate{};
    }

  private:
    recorded::ProducerLogCheckpoint _point;
    bool _stopped{};
  };
}

int main(int argc, char** argv)
{
  try
  {
    if (argc != 3)
    {
      throw std::runtime_error("usage: producer_log_worker directory partial|before|after|hold");
    }
    ::alarm(15);
    const std::filesystem::path directory{argv[1]};
    const std::string mode{argv[2]};
    if (mode != "partial" && mode != "before" && mode != "after" && mode != "hold")
    {
      throw std::runtime_error("Unknown producer-log worker checkpoint");
    }
    const auto point = mode == "partial" ? recorded::ProducerLogCheckpoint::PartialMaterialization :
                       mode == "before" ? recorded::ProducerLogCheckpoint::BeforeCommit :
                                          recorded::ProducerLogCheckpoint::AfterCommit;
    PauseAtCheckpoint observer{point};
    const auto options = support::producer_log_options();
    auto writer = support::require_value(recorded::ProducerLog::open_writer(
        support::producer_log_paths(directory), options, mode == "hold" ? nullptr : &observer));
    const auto claim = support::require_value(writer.claim());
    support::report('O');
    support::report(claim._incarnation);
    if (mode == "hold")
    {
      support::wait_for_parent();
      return 0;
    }

    const auto publication = support::require_value(writer.next_publication_id());
    const std::array payload{std::byte{0x31}, std::byte{0x42}, std::byte{0x53}, std::byte{0x64}};
    const auto appended = support::require_value(writer.append(
        support::producer_append_request(publication, payload)));
    support::report('A');
    support::report(appended._recordOrdinal);
    support::report(appended._queueSequence);
  }
  catch (const std::exception& error)
  {
    std::cerr << "producer-log worker: " << error.what() << '\n';
    return 1;
  }
}
