// Independent exec worker for the private journal crash tests. It pauses at a
// selected real write/recovery checkpoint so the parent can SIGKILL it and
// reopen the file with fresh process state; this is a test harness, not a
// public transaction API example. See test_mutation_journal.cpp for outcomes.

#include "journal_fixture.h"
#include "result_helpers.h"
#include "mutation_journal.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <signal.h>
#include <unistd.h>

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;

  using angaria::test_support::require_value;

  // Match the parent's synthetic draft so it can distinguish old words from
  // fully accepted words after reopening. Public Task mutations construct and
  // validate their own plans; applications do not populate these raw fields.
  mapped::TransactionPlan make_plan(const mapped::JournalSnapshot& base,
                                    const mapped::SuperblockConfiguration& configuration, bool append)
  {
    return angaria::test_support::make_plan(base, configuration, append, "worker");
  }

  using angaria::test_support::make_publication_plan;

  // Report the selected checkpoint and remain alive there until the parent
  // kills us. Returning from the callback would let the operation progress and
  // make the observed crash state depend on timing instead of the chosen cut.
  class StopAt : public mapped::JournalFaultInjector
  {
  public:
    StopAt(mapped::JournalCheckpoint point, std::uint64_t index) : _point(point), _index(index)
    {
    }

    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t index) override
    {
      if (point == _point && index == _index)
      {
        if (::write(STDOUT_FILENO, "R", 1) != 1)
        {
          ::_exit(3);
        }
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
} // namespace

int main(int argc, char** argv)
{
  try
  {
    if (argc != 5)
    {
      throw std::runtime_error("usage: native_journal_worker fixed|append|publication|recover directory checkpoint index");
    }
    // Bound worker lifetime even if the parent fails before issuing SIGKILL.
    ::alarm(8);
    const std::string mode{argv[1]};
    const auto point = static_cast<mapped::JournalCheckpoint>(std::stoul(argv[3]));
    const auto index = std::stoull(argv[4]);
    StopAt stop(point, index);
    auto controller = require_value(mapped::SessionController::open_existing(argv[2]));
    auto guard = require_value(controller->acquire_mutation());
    auto journal = require_value(mapped::MutationJournal::open(controller, guard, &stop));
    if (mode == "recover")
    {
      // The same hook can stop recovery itself, testing repair after another
      // interrupted repair rather than only after an interrupted writer.
      require_value(journal.recover(guard));
    }
    else if (mode == "publication")
    {
      const auto base = require_value(journal.snapshot(guard));
      require_value(journal.execute(guard, make_publication_plan(base)));
    }
    else if (mode == "fixed" || mode == "append")
    {
      const auto base = require_value(journal.snapshot(guard));
      require_value(journal.execute(guard, make_plan(base, controller->configuration(), mode == "append")));
    }
    else
    {
      throw std::runtime_error("unknown journal worker mode");
    }
    std::cerr << "journal worker did not reach its selected checkpoint\n";
    return 2;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
