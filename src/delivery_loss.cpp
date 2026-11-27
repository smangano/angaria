#include "delivery_loss.h"

#include "angaria/detail/byte_operations.h"
#include "delivery_history.h"
#include "registry_access.h"
#include "topology_state.h"

#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <utility>

namespace angaria::detail
{
  Result<std::optional<DeliveryGap>> runtime_lose_delivery_prefix(
      RuntimeAccess& access, const std::shared_ptr<TaskState>& consumer,
      SourceQueueSetId source, QueueId queue, UtcTimestamp lostAt,
      std::optional<std::chrono::steady_clock::time_point> deadline)
  {
    try
    {
      runtime_check_task(access, consumer);
      const auto topology = runtime_topology(access);
      const auto& loadedSource = require_registered_entity(topology._sources, source, "source.loss");
      const auto& loadedQueue = require_registered_entity(topology._queues, queue, "queue.loss");
      if (loadedSource._definition._descriptor._owner != consumer->_claim._task ||
          loadedQueue._definition._descriptor._sourceSet != source)
      {
        runtime_fail(ErrorCode::WrongOwner, "queue.loss", "Selected Queue does not belong to this consumer Source");
      }
      if (loadedSource._definition._state != EntityState::Active ||
          loadedQueue._definition._state != EntityState::Active)
      {
        runtime_fail(ErrorCode::NotActive, "queue.loss", "Loss requires an Active Source and Queue");
      }
      if (loadedQueue._definition._descriptor._delivery != DeliveryPolicy::AllowLoss)
      {
        return std::optional<DeliveryGap>{};
      }
      if ((access._session->_controller->configuration()._requiredFeatures & mapped::allow_loss_feature) == 0)
      {
        runtime_fail(ErrorCode::UnsupportedPolicy, "queue.loss", "Session does not enable allowed-loss progress");
      }
      const auto sourceWords = access.read_words(loadedSource._stateOffset, 8);
      if (sourceWords[2] != 0)
      {
        runtime_fail(ErrorCode::OwnershipBusy, "source.loss", "An in-flight delivery must end before loss progress");
      }
      const auto words = access.read_words(loadedQueue._stateOffset, 12);
      const auto last = words[0];
      const auto terminal = words[8];
      const auto capacity = words[3];
      // Complete validation above established terminal <= last and capacity >0.
      // Avoid incrementing an exhausted terminal counter when nothing is pending.
      if (terminal == last)
      {
        return std::optional<DeliveryGap>{};
      }
      const auto floor = last < capacity ? 1 : last - capacity + 1;
      const auto first = terminal + 1;
      if (first >= floor)
      {
        return std::optional<DeliveryGap>{};
      }
      const auto lastLost = floor - 1;
      const auto next = runtime_queue_delivery(access, queue, floor);
      if (deadline && std::chrono::steady_clock::now() >= *deadline)
      {
        return std::optional<DeliveryGap>{};
      }
      mapped::RedoCommand loss;
      loss._opcode = mapped::RedoOpcode::LoseRange;
      loss._target = loadedQueue._stateOffset;
      loss._payload.resize(32);
      write_unsigned_le(loss._payload, 0, first);
      write_unsigned_le(loss._payload, 8, lastLost);
      write_unsigned_le(loss._payload, 16,
          std::bit_cast<std::uint64_t>(lostAt.time_since_epoch().count()));
      write_unsigned_le(loss._payload, 24, static_cast<std::uint16_t>(source));
      const std::array<std::uint64_t, 2> progress{lastLost, next._link};
      mapped::TransactionPlan plan;
      plan._base = access._snapshot;
      plan._kind = mapped::JournalKind::FixedState;
      plan._actor = consumer->_claim._task;
      plan._actorIncarnation = consumer->_claim._incarnation;
      plan._newRevision = access._snapshot._revision;
      plan._commands.push_back(std::move(loss));
      plan._commands.push_back(runtime_write_words(loadedQueue._stateOffset + 64, progress));
      if (deadline && std::chrono::steady_clock::now() >= *deadline)
      {
        return std::optional<DeliveryGap>{};
      }
      const auto committed = runtime_value(access.execute(plan));
      access._snapshot._generation = committed._generation;
      return std::optional<DeliveryGap>{DeliveryGap{queue, first, lastLost}};
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }
} // namespace angaria::detail
