#pragma once

#include "angaria/detail/record_layout.h"
#include "result_helpers.h"
#include "topology_state.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace angaria::test_support
{
  // Private exact-budget qualification seam. Append a valid retained Queue
  // documentation revision, rather than falsifying the Session's byte ceiling.
  // Normal application scenarios still create all messages through public send.
  inline void fill_log_with_queue_revision(const std::shared_ptr<detail::SessionState>& state,
                                               QueueId id, const TaskClaim& actor)
  {
    detail::RuntimeAccess access(state);
    const auto topology = detail::runtime_topology(access);
    const auto found = std::find_if(topology._queues.begin(), topology._queues.end(), [id](const auto& queue)
    {
      return queue._definition._id == id;
    });
    if (found == topology._queues.end())
    {
      throw std::runtime_error("Exact-fill Queue is not registered");
    }
    const auto& queue = *found;
    const auto maxLogBytes = state->_controller->configuration()._maxLogBytes;
    const auto remaining = maxLogBytes - access._snapshot._tail;
    constexpr std::uint64_t manifestBytes = 192; // fixed64 + ref8 + redo48 + envelope/padding.
    constexpr std::uint64_t queueFixedBytes = 80;
    const auto fixed = detail::mapped::record_envelope_bytes + queueFixedBytes +
        queue._definition._descriptor._name.size();
    if (remaining < manifestBytes + fixed)
    {
      throw std::runtime_error("Insufficient fixture headroom for exact-fill Queue revision");
    }
    auto definition = queue._definition;
    definition._registration._revision = access._snapshot._revision + 1;
    definition._descriptor._doc.assign(static_cast<std::size_t>(remaining - manifestBytes - fixed), 'x');
    detail::mapped::JournalRecord record;
    record._kind = 3;
    record._previous = queue._reference;
    record._immutable = require_value(detail::mapped::encode_queue_definition(definition, access._snapshot._tail));
    detail::mapped::TransactionPlan plan;
    plan._base = access._snapshot;
    plan._kind = detail::mapped::JournalKind::Append;
    plan._actor = actor._task;
    plan._actorIncarnation = actor._incarnation;
    plan._newRevision = definition._registration._revision;
    plan._records.push_back(std::move(record));
    const std::array<std::uint64_t, 4> row{static_cast<std::uint16_t>(id),
        access._snapshot._tail, plan._newRevision, 2};
    plan._commands.push_back(detail::runtime_write_words(queue._indexOffset, row));
    const auto result = require_value(access._journal.execute(access._guard, plan));
    if (result._tail != maxLogBytes)
    {
      throw std::runtime_error("Queue revision did not exactly fill fixture log");
    }
  }
} // namespace angaria::test_support
