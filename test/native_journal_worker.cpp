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
      throw std::runtime_error("usage: native_journal_worker fixed|append|recover directory checkpoint index");
    }
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
      require_value(journal.recover(guard));
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
