// Independent exec worker for the private journal crash tests. It pauses at a
// selected real write/recovery checkpoint so the parent can SIGKILL it and
// reopen the file with fresh process state; this is a test harness, not a
// public transaction API example. See test_mutation_journal.cpp for outcomes.

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

  template <typename T>
  T require_value(Result<T> result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    return std::get<T>(std::move(result));
  }

  void put_integer(std::vector<std::byte>& bytes, std::size_t offset,
                    std::uint64_t value, std::size_t width = 8)
  {
    for (std::size_t index = 0; index < width; ++index)
    {
      bytes.at(offset + index) = static_cast<std::byte>((value >> (8 * index)) & 0xff);
    }
  }

  std::uint64_t section_offset(const mapped::SuperblockConfiguration& configuration,
                                mapped::SectionKind kind)
  {
    const auto layout = mapped::make_fixed_layout(configuration._limits);
    if (!layout)
    {
      throw std::runtime_error("worker configuration has no layout");
    }
    for (const auto& section : layout->_sections)
    {
      if (section._kind == kind)
      {
        return section._offset;
      }
    }
    throw std::runtime_error("worker section is missing");
  }

  mapped::RedoCommand write_word(std::uint64_t target, std::uint64_t value)
  {
    mapped::RedoCommand command;
    command._target = target;
    command._payload.resize(8);
    put_integer(command._payload, 0, value);
    return command;
  }

  // Match the parent's synthetic draft so it can distinguish old words from
  // fully accepted words after reopening. Public Task mutations construct and
  // validate their own plans; applications do not populate these raw fields.
  mapped::TransactionPlan make_plan(const mapped::JournalSnapshot& base,
                                    const mapped::SuperblockConfiguration& configuration,
                                    bool append)
  {
    mapped::TransactionPlan plan;
    plan._base = base;
    plan._kind = append ? mapped::JournalKind::Append : mapped::JournalKind::FixedState;
    plan._newRevision = base._revision + (append ? 1 : 0);
    const auto task_state = section_offset(configuration, mapped::SectionKind::TaskState);
    plan._commands.push_back(write_word(task_state, 11));
    plan._commands.push_back(write_word(task_state + 8, 22));
    if (append)
    {
      mapped::JournalRecord record;
      record._kind = 2;
      record._immutable.resize(76);
      put_integer(record._immutable, 0, 1);
      put_integer(record._immutable, 8, 1, 2);
      put_integer(record._immutable, 24, plan._newRevision);
      put_integer(record._immutable, 32, base._tail + 64 + 72);
      put_integer(record._immutable, 40, 4);
      put_integer(record._immutable, 64, 1, 2);
      record._immutable[72] = std::byte{'t'};
      record._immutable[73] = std::byte{'a'};
      record._immutable[74] = std::byte{'s'};
      record._immutable[75] = std::byte{'k'};
      plan._records.push_back(std::move(record));
      mapped::RedoCommand index;
      index._target = section_offset(configuration, mapped::SectionKind::TaskIndex);
      index._payload.resize(32);
      put_integer(index._payload, 0, 1);
      put_integer(index._payload, 8, base._tail);
      put_integer(index._payload, 16, plan._newRevision);
      plan._commands.push_back(std::move(index));
    }
    return plan;
  }

  mapped::TransactionPlan make_publication_plan(const mapped::JournalSnapshot& base)
  {
    mapped::TransactionPlan plan;
    plan._base = base;
    plan._kind = mapped::JournalKind::Append;
    plan._subtype = mapped::TransactionSubtype::Publication;
    plan._actor = static_cast<TaskId>(1);
    plan._actorIncarnation = 7;
    plan._newRevision = base._revision;

    mapped::JournalRecord record;
    record._kind = 9;
    record._immutable.resize(232); // Fixed body, two destinations, eight payload bytes.
    const auto body = base._tail + mapped::record_envelope_bytes;
    const auto receipts = body + record._immutable.size();
    put_integer(record._immutable, 0, 1, 2); // Producer.
    put_integer(record._immutable, 2, 1, 2); // Publishing set.
    put_integer(record._immutable, 4, 1, 2); // Message type.
    put_integer(record._immutable, 6, 1, 2); // Binary payload.
    put_integer(record._immutable, 8, 1, 4); // Schema version.
    put_integer(record._immutable, 16, 7);
    put_integer(record._immutable, 24, 99);
    put_integer(record._immutable, 32, 1000);
    put_integer(record._immutable, 40, 3);
    put_integer(record._immutable, 48, 2, 4);
    put_integer(record._immutable, 56, body + 224);
    put_integer(record._immutable, 64, 8);
    // This fixture exercises journal framing; registered schema/Task existence
    // is validated by the later domain layer, not by the journal.
    put_integer(record._immutable, 80, body + 128);
    put_integer(record._immutable, 88, 2);
    put_integer(record._immutable, 96, receipts);
    put_integer(record._immutable, 104, 2);
    put_integer(record._immutable, 112, 1, 2); // All destinations.
    for (std::size_t destination = 0; destination != 2; ++destination)
    {
      const auto row = 128 + destination * 48;
      put_integer(record._immutable, row, destination + 1, 2);
      put_integer(record._immutable, row + 2, 1, 2); // Accepted.
      put_integer(record._immutable, row + 8, 101 + destination);
      put_integer(record._immutable, row + 24, 1000 + destination);
      put_integer(record._immutable, row + 32, receipts + destination * 128);
    }
    put_integer(record._immutable, 224, 0x0123456789abcdef);
    record._mutable.resize(256);
    put_integer(record._mutable, 0, 1); // Both receipts initially pending.
    put_integer(record._mutable, 128, 1);
    plan._records.push_back(std::move(record));

    plan._commands.push_back(write_word(receipts, 2)); // Issue the first receipt.
    mapped::RedoCommand issuance;
    issuance._target = receipts + 8;
    issuance._payload.resize(32);
    put_integer(issuance._payload, 0, 0x0000000100010002); // Task 2, Source 1, dequeue flag.
    put_integer(issuance._payload, 8, 7);
    put_integer(issuance._payload, 16, 13);
    put_integer(issuance._payload, 24, 1002);
    plan._commands.push_back(std::move(issuance));
    return plan;
  }

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
