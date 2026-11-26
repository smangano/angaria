#pragma once

#include "wire_fixture.h"
#include "mutation_journal.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace angaria::test_support
{
  namespace mapped = angaria::detail::mapped;

  // Shared private journal drafts used by the parent tests and exec worker.
  // These hand-authored fields test framing and recovery, not usable Session
  // bootstrap or application acceptance. No production encoder builds them.
  inline std::uint64_t section_offset(const mapped::SuperblockConfiguration& configuration,
                                mapped::SectionKind kind, const std::string& context = "test")
  {
    const auto layout = mapped::make_fixed_layout(configuration._limits);
    if (!layout)
    {
      throw std::runtime_error(context + " configuration has no layout");
    }
    for (const auto& section : layout->_sections)
    {
      if (section._kind == kind)
      {
        return section._offset;
      }
    }
    throw std::runtime_error(context + " section is missing");
  }

  inline mapped::RedoCommand write_word(std::uint64_t target, std::uint64_t value)
  {
    mapped::RedoCommand command;
    command._target = target;
    command._payload.resize(8);
    put_integer(command._payload, 0, value);
    return command;
  }

  inline mapped::TransactionPlan make_plan(const mapped::JournalSnapshot& base,
                                    const mapped::SuperblockConfiguration& configuration,
                                    bool append = false, const std::string& context = "test")
  {
    mapped::TransactionPlan plan;
    plan._base = base;
    plan._kind = append ? mapped::JournalKind::Append : mapped::JournalKind::FixedState;
    plan._newRevision = base._revision + (append ? 1 : 0);
    const auto task_state = section_offset(configuration, mapped::SectionKind::TaskState, context);
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
      index._target = section_offset(configuration, mapped::SectionKind::TaskIndex, context);
      index._payload.resize(32);
      put_integer(index._payload, 0, 1);
      put_integer(index._payload, 8, base._tail);
      put_integer(index._payload, 16, plan._newRevision);
      plan._commands.push_back(std::move(index));
    }
    return plan;
  }

  inline mapped::TransactionPlan make_publication_plan(const mapped::JournalSnapshot& base)
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

} // namespace angaria::test_support
