#include "delivery_progress.h"

#include "angaria/detail/byte_operations.h"
#include "delivery_words.h"
#include "delivery_history.h"
#include "mapped_payload.h"
#include "registry_access.h"
#include "topology_state.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace angaria::detail
{
  namespace
  {
    void require(bool condition, ErrorCode code, const char* message)
    {
      if (!condition)
      {
        runtime_fail(code, "delivery.progress", message);
      }
    }

    template <typename Loaded, typename Id>
    const Loaded& find_entity(const std::vector<Loaded>& entries, Id id, ErrorCode code)
    {
      return require_registered_entity(entries, id, "delivery.progress",
          "Delivery participant identity is empty", "Delivery participant is not registered", code, code);
    }

    void check_progress_owner(RuntimeAccess& access, const TaskClaim& consumer,
                                 ErrorCode staleCode)
    {
      runtime_value(access._guard.check_owner());
      const auto& configuration = access._session->_controller->configuration();
      require((configuration._requiredFeatures & mapped::native_delivery_feature) != 0,
              ErrorCode::UnsupportedPolicy, "Session does not enable native delivery progress");
      require(consumer._session == configuration._session && consumer._incarnation != 0,
              staleCode, "Consumer claim belongs to another Session or is empty");
    }

    void check_progress_claim(RuntimeAccess& access, const TopologySnapshot& topology,
                                 const TaskClaim& consumer, ErrorCode staleCode)
    {
      const auto& task = find_entity(topology._tasks, consumer._task, staleCode);
      const auto words = access.read_words(task._stateOffset, 32);
      require(words[0] == consumer._incarnation && (words[6] & task_attached_flag) != 0,
              staleCode, "Consumer claim no longer owns this Task");
      if (task._definition._descriptor._kind == TaskKind::Thread)
      {
        const auto& parent = find_entity(topology._tasks,
            task._definition._descriptor._process.value_or(TaskId::Invalid), staleCode);
        const auto parentWords = access.read_words(parent._stateOffset, 32);
        require(parentWords[0] == words[7] && (parentWords[6] & task_attached_flag) != 0 &&
                    parentWords[3] == words[3],
                staleCode, "Thread consumer belongs to an ended parent Process claim");
      }
    }

    TopologySnapshot progress_topology(RuntimeAccess& access, const TaskClaim& consumer,
                                         ErrorCode staleCode)
    {
      check_progress_owner(access, consumer, staleCode);
      auto topology = runtime_topology(access);
      check_progress_claim(access, topology, consumer, staleCode);
      return topology;
    }

    const LoadedSource& consumer_source(const TopologySnapshot& topology, const TaskClaim& consumer,
                                           SourceQueueSetId source, ErrorCode code)
    {
      const auto& loaded = find_entity(topology._sources, source, code);
      require(loaded._definition._descriptor._owner == consumer._task, code,
              "Consumer Task does not own this Source");
      return loaded;
    }

    DeliveryProgress pinned_delivery(const TaskClaim& consumer, SourceQueueSetId source,
                                        std::span<const std::uint64_t> words)
    {
      return {{consumer, source, QueueId{static_cast<std::uint16_t>(words[2])}, words[3], words[0]},
              words[4], words[5], words[7],
              UtcTimestamp{std::chrono::nanoseconds{std::bit_cast<std::int64_t>(words[6])}}};
    }

    void commit_progress(RuntimeAccess& access, const TaskClaim& consumer,
                             std::vector<mapped::RedoCommand> commands)
    {
      mapped::TransactionPlan plan;
      plan._base = access._snapshot;
      plan._kind = mapped::JournalKind::FixedState;
      plan._actor = consumer._task;
      plan._actorIncarnation = consumer._incarnation;
      plan._newRevision = access._snapshot._revision;
      plan._commands = std::move(commands);
      const auto result = runtime_value(access.execute(plan));
      // FixedState does not append or replace record boundaries. Retain the
      // completed generation so this same guard can prepare immediate cleanup
      // if later local mapping/result preparation fails.
      access._snapshot._generation = result._generation;
    }

    void release_pin(RuntimeAccess& access, const TaskClaim& consumer,
                         const LoadedSource& source, std::span<const std::uint64_t> words)
    {
      const std::array<std::uint64_t, 1> pending{1};
      const std::array<std::uint64_t, 6> empty{};
      std::vector<mapped::RedoCommand> commands;
      // Only the state changes: historical consumer/issuance/dequeue tags are
      // retained for exact old-claim cleanup and repeatable compact replay.
      commands.push_back(runtime_write_words(words[5], pending));
      commands.push_back(runtime_write_words(source._stateOffset + 16, empty));
      commit_progress(access, consumer, std::move(commands));
    }

    std::vector<std::uint64_t> exact_pin(RuntimeAccess& access, const LoadedSource& source,
                                           const DeliveryToken& token)
    {
      const auto words = access.read_words(source._stateOffset, 8);
      require(words[2] != 0 && token._sequence != 0 && token._issuance != 0 &&
                  pinned_delivery(token._consumer, token._sourceSet, words)._token == token &&
                  words[1] == token._consumer._incarnation,
              ErrorCode::StaleDelivery, "Delivery token does not identify the Source's current issuance");
      return words;
    }

    DeliveryProgress issue_delivery(RuntimeAccess& access, const TopologySnapshot& topology,
                                        const TaskClaim& consumer, SourceQueueSetId source,
                                        QueueId queue, UtcTimestamp dequeuedAt)
    {
      const auto& loadedSource = consumer_source(topology, consumer, source, ErrorCode::WrongOwner);
      const auto sourceWords = access.read_words(loadedSource._stateOffset, 8);
      if (sourceWords[2] != 0)
      {
        return pinned_delivery(consumer, source, sourceWords);
      }
      require(loadedSource._definition._state == EntityState::Active, ErrorCode::NotActive,
              "Fresh issuance requires an Active Source");
      const auto& loadedQueue = find_entity(topology._queues, queue, ErrorCode::NotFound);
      require(loadedQueue._definition._descriptor._sourceSet == source, ErrorCode::WrongOwner,
              "Selected Queue does not belong to this Source");
      require(loadedQueue._definition._state == EntityState::Active, ErrorCode::NotActive,
              "Fresh issuance requires an Active Queue");
      const auto queueWords = access.read_words(loadedQueue._stateOffset, 12);
      require(queueWords[9] != 0, ErrorCode::NotFound, "Queue has no pending delivery");
      const auto counter = sourceWords[1] == consumer._incarnation ? sourceWords[0] : 0;
      require(counter != std::numeric_limits<std::uint64_t>::max(), ErrorCode::CapacityExceeded,
              "Source issuance counter is exhausted for this consumer claim");
      const auto sequence = queueWords[8] + 1;
      const auto delivery = runtime_queue_delivery(access, queue, sequence);
      require(delivery._link == queueWords[9], ErrorCode::IoError,
              "Queue oldest pending link disagrees with its receipt");
      const auto oldReceipt = runtime_value(mapped::decode_receipt_words(access.read_words(delivery._receipt, 16)));
      require(oldReceipt._state == mapped::ReceiptState::Pending, ErrorCode::IoError,
              "Oldest Queue delivery is not pending");

      mapped::Receipt receipt;
      receipt._state = mapped::ReceiptState::Issued;
      receipt._consumerTask = consumer._task;
      receipt._sourceSet = source;
      receipt._presence = 1;
      receipt._consumerIncarnation = consumer._incarnation;
      receipt._issuance = counter + 1;
      receipt._dequeuedAt = dequeuedAt;
      const auto receiptWords = runtime_value(mapped::encode_receipt_words(receipt));
      const std::array<std::uint64_t, 8> issuedSource{
          receipt._issuance, consumer._incarnation,
          static_cast<std::uint16_t>(queue) | (std::uint64_t{1} << 16), sequence,
          delivery._publication, delivery._receipt,
          std::bit_cast<std::uint64_t>(dequeuedAt.time_since_epoch().count()), delivery._link};
      std::vector<mapped::RedoCommand> commands;
      commands.push_back(runtime_write_words(delivery._receipt, std::span{receiptWords}.first(7)));
      commands.push_back(runtime_write_words(loadedSource._stateOffset, issuedSource));
      commit_progress(access, consumer, std::move(commands));
      return pinned_delivery(consumer, source, issuedSource);
    }
  } // namespace

  Result<DeliveryProgress> runtime_issue_delivery(RuntimeAccess& access, const TaskClaim& consumer,
                                                   SourceQueueSetId source, QueueId queue,
                                                   UtcTimestamp dequeuedAt)
  {
    try
    {
      const auto topology = progress_topology(access, consumer, ErrorCode::WrongOwner);
      return issue_delivery(access, topology, consumer, source, queue, dequeuedAt);
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  Result<DeliveryProgress> runtime_issue_delivery(RuntimeAccess& access, const TopologySnapshot& topology,
                                                   const TaskClaim& consumer, SourceQueueSetId source,
                                                   QueueId queue, UtcTimestamp dequeuedAt)
  {
    try
    {
      check_progress_owner(access, consumer, ErrorCode::WrongOwner);
      check_progress_claim(access, topology, consumer, ErrorCode::WrongOwner);
      return issue_delivery(access, topology, consumer, source, queue, dequeuedAt);
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  OperationResult runtime_acknowledge_delivery(RuntimeAccess& access, const DeliveryToken& token,
                                                 UtcTimestamp acknowledgedAt)
  {
    try
    {
      const auto topology = progress_topology(access, token._consumer, ErrorCode::StaleDelivery);
      const auto& source = consumer_source(topology, token._consumer, token._sourceSet, ErrorCode::StaleDelivery);
      const auto sourceWords = exact_pin(access, source, token);
      const auto& queue = find_entity(topology._queues, token._queue, ErrorCode::StaleDelivery);
      const auto queueWords = access.read_words(queue._stateOffset, 12);
      require(queueWords[8] != std::numeric_limits<std::uint64_t>::max() &&
                  token._sequence == queueWords[8] + 1 && queueWords[9] == sourceWords[7],
              ErrorCode::IoError, "Acknowledgment is not the oldest nonterminal Queue delivery");
      auto receipt = runtime_value(mapped::decode_receipt_words(access.read_words(sourceWords[5], 16)));
      receipt._state = mapped::ReceiptState::Acknowledged;
      receipt._presence = 7;
      receipt._acknowledgedAt = receipt._terminalAt = acknowledgedAt;
      const auto receiptWords = runtime_value(mapped::encode_receipt_words(receipt));
      const auto next = token._sequence == queueWords[0] ? 0 :
          runtime_queue_delivery(access, token._queue, token._sequence + 1)._link;
      const std::array<std::uint64_t, 4> acknowledged{
          token._sequence, next, std::bit_cast<std::uint64_t>(acknowledgedAt.time_since_epoch().count()), 1};
      const std::array<std::uint64_t, 6> empty{};
      std::vector<mapped::RedoCommand> commands;
      commands.push_back(runtime_write_words(sourceWords[5], std::span{receiptWords}.first(7)));
      commands.push_back(runtime_write_words(queue._stateOffset + 64, acknowledged));
      commands.push_back(runtime_write_words(source._stateOffset + 16, empty));
      commit_progress(access, token._consumer, std::move(commands));
      return std::monostate{};
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  OperationResult runtime_release_delivery(RuntimeAccess& access, const DeliveryToken& token)
  {
    try
    {
      const auto topology = progress_topology(access, token._consumer, ErrorCode::StaleDelivery);
      const auto& source = consumer_source(topology, token._consumer, token._sourceSet, ErrorCode::StaleDelivery);
      const auto words = exact_pin(access, source, token);
      release_pin(access, token._consumer, source, words);
      return std::monostate{};
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  OperationResult runtime_release_source(RuntimeAccess& access, const TaskClaim& consumer,
                                           SourceQueueSetId source)
  {
    try
    {
      const auto topology = progress_topology(access, consumer, ErrorCode::StaleDelivery);
      const auto& loaded = consumer_source(topology, consumer, source, ErrorCode::StaleDelivery);
      const auto words = access.read_words(loaded._stateOffset, 8);
      if (words[2] != 0)
      {
        release_pin(access, consumer, loaded, words);
      }
      return std::monostate{};
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }
} // namespace angaria::detail
