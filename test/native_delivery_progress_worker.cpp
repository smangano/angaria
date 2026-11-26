// Independent exec worker for private delivery progress and compact claim
// cleanup crash qualification. These private operations isolate replay stores;
// public delivery is qualified separately by the Source/transport workers.
// The worker pauses at a real journal checkpoint for the parent to terminate it.

#include "delivery_progress.h"
#include "delivery_progress_fixture.h"
#include "process_support.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include <signal.h>
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
        support::report('R');
        for (;;)
        {
          ::pause();
        }
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
  try
  {
    if (argc != 8)
    {
      throw std::runtime_error("usage: native_delivery_progress_worker mode directory checkpoint index task source queue");
    }
    ::alarm(8);
    const std::string mode{argv[1]};
    StopAt stop(static_cast<mapped::JournalCheckpoint>(std::stoul(argv[3])), std::stoull(argv[4]));
    auto state = support::open_runtime_state(argv[2]);
    runtime::RuntimeAccess access(state, true, &stop);
    const auto task = static_cast<TaskId>(std::stoul(argv[5]));
    const auto source = static_cast<SourceQueueSetId>(std::stoul(argv[6]));
    const auto queue = static_cast<QueueId>(std::stoul(argv[7]));
    const TaskClaim claim{state->_controller->session_id(), task, 1};
    const DeliveryToken token{claim, source, queue, 1, 1};
    if (mode == "issue")
    {
      support::require_value(runtime::runtime_issue_delivery(access, claim, source, queue,
          UtcTimestamp{std::chrono::nanoseconds{-7}}));
    }
    else if (mode == "acknowledge")
    {
      support::require_value(runtime::runtime_acknowledge_delivery(access, token,
          UtcTimestamp{std::chrono::nanoseconds{-31}}));
    }
    else if (mode == "release")
    {
      support::require_value(runtime::runtime_release_delivery(access, token));
    }
    else if (mode == "source")
    {
      support::require_value(runtime::runtime_release_source(access, claim, source));
    }
    else if (mode == "claim" || mode == "replace")
    {
      support::require_value(access._journal.execute(access._guard,
          support::progress_claim_plan(access, task, mode == "replace")));
    }
    else if (mode != "recover")
    {
      throw std::runtime_error("Unknown delivery progress worker mode");
    }
    std::cerr << "Delivery progress worker did not reach its selected checkpoint\n";
    return 2;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
