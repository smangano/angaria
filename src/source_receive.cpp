#include "source_runtime_state.h"

#include "performance_diagnostics.h"

#include "delivery_loss.h"
#include "message_retention.h"
#include "payload_runtime.h"
#include "registry_access.h"
#include "topology_state.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <ratio>
#include <thread>
#include <utility>
#include <variant>

namespace angaria::detail
{
  namespace
  {
    using WaitClock = std::chrono::steady_clock;

    std::optional<WaitClock::time_point> receive_deadline(std::chrono::nanoseconds timeout)
    {
      if (timeout.count() < 0)
      {
        runtime_fail(ErrorCode::InvalidArgument, "source.timeout", "Receive timeout cannot be negative");
      }
      if (timeout.count() == 0)
      {
        return std::nullopt;
      }
      // Check in wide arithmetic before converting or adding to the clock's
      // native duration. Round a positive sub-tick timeout up to one tick.
      using Conversion = std::ratio_divide<std::chrono::nanoseconds::period, WaitClock::duration::period>;
      const auto scaled = static_cast<__int128>(timeout.count()) * Conversion::num;
      const auto ticks = (scaled + Conversion::den - 1) / Conversion::den;
      const auto now = WaitClock::now().time_since_epoch().count();
      const auto end = static_cast<__int128>(now) + ticks;
      using Rep = WaitClock::duration::rep;
      if (ticks > std::numeric_limits<Rep>::max() || end > std::numeric_limits<Rep>::max() ||
          end < std::numeric_limits<Rep>::min())
      {
        runtime_fail(ErrorCode::InvalidArgument, "source.timeout", "Receive timeout exceeds the monotonic deadline range");
      }
      return WaitClock::time_point{WaitClock::duration{static_cast<Rep>(end)}};
    }

    bool expired(const std::optional<WaitClock::time_point>& deadline)
    {
      return deadline && WaitClock::now() >= *deadline;
    }

    SourceOperationHooks* operation_hooks(const std::shared_ptr<SourceQueueSetState>& state,
                                           SourceOperationHooks* requested)
    {
      return requested ? requested : state->_hooks;
    }

    void require_delivery_capability(const std::shared_ptr<TaskState>& task)
    {
      if ((task->_session->_controller->configuration()._requiredFeatures &
           mapped::native_delivery_feature) == 0)
      {
        runtime_fail(ErrorCode::UnsupportedPolicy, "source.feature", "Session does not enable native delivery");
      }
    }

    const LoadedSource& checked_source(const TopologySnapshot& topology,
                                         const SourceQueueSetState& state, const std::shared_ptr<TaskState>& task)
    {
      const auto& source = require_registered_entity(topology._sources, state._id, "source.id");
      if (source._definition._descriptor._owner != task->_claim._task)
      {
        runtime_fail(ErrorCode::WrongOwner, "source.owner", "Consumer Task does not own this Source");
      }
      if (source._definition._state != EntityState::Active)
      {
        runtime_fail(ErrorCode::NotActive, "source.state", "Receive requires an Active Source");
      }
      return source;
    }

    Delivery prepare_delivery(RuntimeAccess& access, const std::shared_ptr<SourceQueueSetState>& owner,
                                const std::shared_ptr<TaskState>& task, const DeliveryProgress& progress,
                                SourceOperationHooks* hooks)
    {
      auto& state = *owner;
      // A repeat keeps the original mapping and dependency context. Replacing
      // either while the same pin is live could invalidate earlier spans.
      if (state._delivery && state._delivery->_token == progress._token)
      {
        runtime_require_source_active(owner);
        return *state._delivery;
      }
      runtime_clear_source_view(state);
      state._reservation->_token = progress._token;
      state._reservation->_pendingIssuance.reset();
      try
      {
        const auto metadata = runtime_publication_metadata(access, progress._publicationReference);
        const auto& destination = metadata._destinations.front();
        if (destination._queue != progress._token._queue ||
            destination._queueSequence != progress._token._sequence ||
            destination._receiptReference != progress._receiptReference ||
            metadata._layout._destinationsOffset != progress._deliveryLink)
        {
          runtime_fail(ErrorCode::IoError, "source.delivery", "Pinned delivery disagrees with its retained Publication");
        }
        if (hooks)
        {
          runtime_value(hooks->before_mapping(progress));
        }
        auto payload = runtime_value(map_publication_payload(access, progress._publicationReference));
        const auto bytes = runtime_value(payload.bytes());
        auto context = runtime_schema_context(access, metadata._schema);
        if (context._schema._reference != metadata._schemaReference)
        {
          runtime_fail(ErrorCode::IoError, "source.schema", "Pinned Publication does not retain its exact schema");
        }
        auto codec = runtime_payload_codec(context._schema, context._fields,
            access._session->_controller->configuration()._maxLogBytes);
        MessageHeader header;
        header._schema = metadata._schema;
        header._format = metadata._format;
        header._publication = {{task->_claim._session, metadata._producer,
                                 metadata._producerIncarnation}, metadata._publicationSequence};
        header._publishedAt = metadata._publishedAt;
        header._payloadBytes = metadata._payloadBytes;
        header._flags = metadata._flags;
        header._queue = progress._token._queue;
        header._queueSequence = progress._token._sequence;
        header._consumer = progress._token._consumer;
        header._enqueuedAt = destination._enqueuedAt;
        header._dequeuedAt = progress._dequeuedAt;
        if (hooks)
        {
          runtime_value(hooks->before_binding(progress));
        }
        auto message = runtime_value(MessageAccess::bind(std::move(header), bytes,
            task->_claim._session, std::move(codec)));
        // Everything that can allocate has succeeded. Transfer the mapping
        // before offering the borrowed view to the application.
        runtime_require_source_active(owner);
        state._payload = std::move(payload);
        state._delivery = Delivery{std::move(message), progress._token};
        return *state._delivery;
      }
      catch (...)
      {
        const auto preparationError = runtime_caught_error();
        runtime_clear_source_view(state);
        // No public view escaped, but issuance may already be committed. Its
        // exact identity was saved first, so failed cleanup is still discoverable
        // by receive/close/destruction without borrowing an unowned address.
        const auto cleanup = runtime_release_delivery(access, progress._token);
        if (const auto* error = std::get_if<Error>(&cleanup))
        {
          throw RuntimeFailure{*error};
        }
        state._reservation->_token.reset();
        state._reservation->_pendingIssuance.reset();
        throw RuntimeFailure{preparationError};
      }
    }

    OperationResult progress_source(const std::shared_ptr<SourceQueueSetState>& state,
                                       const DeliveryToken& token, bool acknowledge,
                                       SourceOperationHooks* requested)
    {
      try
      {
        const auto task = runtime_source_task(state);
        runtime_check_task_owner(task);
        runtime_require_source_active(state);
        require_delivery_capability(task);
        auto* hooks = operation_hooks(state, requested);
        RuntimeAccess access(task->_session, true, hooks ? hooks->_journal : nullptr);
        runtime_check_task(access, task);
        runtime_reconcile_sources(access, task);
        runtime_require_source_active(state);
        if (token._consumer != task->_claim || token._sourceSet != state->_id)
        {
          runtime_fail(ErrorCode::StaleDelivery, "source.token", "Token belongs to another Source or consumer claim");
        }
        runtime_require_source_active(state);
        auto result = acknowledge ? runtime_acknowledge_delivery(access, token,
            UtcTimestamp{std::chrono::nanoseconds{runtime_now_ns()}}) :
            runtime_release_delivery(access, token);
        if (const auto* error = std::get_if<Error>(&result))
        {
          if (error->_code == ErrorCode::OutcomeUnknown)
          {
            runtime_clear_source_view(*state);
          }
          return *error;
        }
        runtime_clear_source_view(*state);
        state->_reservation->_token.reset();
        state->_reservation->_pendingIssuance.reset();
        return std::monostate{};
      }
      catch (...)
      {
        // Recovery may fail before a RuntimeAccess exists. The borrow is then
        // logically unusable, but retain its mapping until the next gated
        // recovery instead of clearing shared endpoint state without the gate.
        return runtime_caught_error();
      }
    }
  } // namespace

  ReceiveResult runtime_receive_source(const std::shared_ptr<SourceQueueSetState>& state,
                                         std::chrono::nanoseconds timeout,
                                         SourceOperationHooks* requested)
  {
    try
    {
      const auto task = runtime_source_task(state);
      runtime_check_task_owner(task);
      runtime_require_source_active(state);
      const auto deadline = receive_deadline(timeout);
      require_delivery_capability(task);
      auto* hooks = operation_hooks(state, requested);
      for (;;)
      {
        // Closing fences a waiting call even when another participant still
        // owns the gate. Do not retain the Task until an otherwise long wait.
        runtime_require_source_active(state);
        // Both a zero-time poll and a finite wait avoid an unbounded gate
        // acquisition. Recovery after acquiring it must still finish safely.
        auto acquired = task->_session->_controller->try_acquire_mutation();
        if (const auto* error = std::get_if<Error>(&acquired))
        {
          if (error->_code != ErrorCode::OwnershipBusy)
          {
            return *error;
          }
          if (!deadline || expired(deadline))
          {
            return ReceiveTimeout{};
          }
        }
        else
        {
          ANGARIA_DIAGNOSTIC_SCOPE(ReceiveSelect);
          RuntimeAccess access(task->_session, std::get<mapped::MutationGuard>(std::move(acquired)),
                                 true, hooks ? hooks->_journal : nullptr, hooks ? hooks->_reads : nullptr);
          runtime_check_task(access, task);
          runtime_reconcile_sources(access, task);
          runtime_require_source_active(state);
          const auto topology = runtime_topology(access);
          const auto& source = checked_source(topology, *state, task);
          const auto words = access.read_words(source._stateOffset, 8);
          if (words[2] != 0)
          {
            const auto progress = runtime_value(runtime_issue_delivery(access, topology, task->_claim,
                state->_id, QueueId::Invalid, UtcTimestamp{}));
            return prepare_delivery(access, state, task, progress, hooks);
          }
          // A recovered acknowledgment/release may have ended a previously
          // uncertain borrow. Clear local ownership before selecting afresh.
          runtime_clear_source_view(*state);
          state->_reservation->_token.reset();
          state->_reservation->_pendingIssuance.reset();
          const LoadedQueue* selected{};
          for (const auto& queue : topology._queues)
          {
            if (queue._definition._descriptor._sourceSet != state->_id ||
                queue._definition._state != EntityState::Active)
            {
              continue;
            }
            if (selected)
            {
              runtime_fail(ErrorCode::UnsupportedPolicy, "source.members",
                           "Fresh receive currently supports at most one Active Queue");
            }
            selected = &queue;
          }
          if (expired(deadline))
          {
            return ReceiveTimeout{};
          }
          if (selected && access.read_words(selected->_stateOffset + 72, 1)[0] != 0)
          {
            if (selected->_definition._descriptor._delivery == DeliveryPolicy::AllowLoss)
            {
              runtime_require_source_active(state);
              if (expired(deadline))
              {
                return ReceiveTimeout{};
              }
              const auto gap = runtime_value(runtime_lose_delivery_prefix(access, task,
                  state->_id, selected->_definition._id,
                  UtcTimestamp{std::chrono::nanoseconds{runtime_now_ns()}}, deadline));
              if (gap)
              {
                // Loss ends delivery obligations, but never issues a Source
                // token or consumes its counter. Offer the gap before a message.
                return *gap;
              }
            }
            if (expired(deadline))
            {
              return ReceiveTimeout{};
            }
            const auto counter = words[1] == task->_claim._incarnation ? words[0] : 0;
            if (counter == std::numeric_limits<std::uint64_t>::max())
            {
              runtime_fail(ErrorCode::CapacityExceeded, "source.issuance", "Source issuance counter is exhausted");
            }
            if (expired(deadline))
            {
              return ReceiveTimeout{};
            }
            const auto dequeuedAt = UtcTimestamp{std::chrono::nanoseconds{runtime_now_ns()}};
            runtime_require_source_active(state);
            // Save a nonallocating discovery key before the journal can commit
            // an issuance whose token never reaches this function's caller.
            state->_reservation->_pendingIssuance = counter + 1;
            // The gate stayed held since topology validation. A successful
            // empty loss result made no transaction; a committed gap returned
            // above. Issuance may therefore reuse this unchanged validation.
            const auto progress = runtime_issue_delivery(access, topology, task->_claim, state->_id,
                selected->_definition._id, dequeuedAt);
            if (const auto* error = std::get_if<Error>(&progress))
            {
              if (error->_code != ErrorCode::OutcomeUnknown)
              {
                state->_reservation->_pendingIssuance.reset();
              }
              return *error;
            }
            return prepare_delivery(access, state, task, std::get<DeliveryProgress>(progress), hooks);
          }
          ANGARIA_DIAGNOSTIC_COUNT(EmptyPolls, 1);
          if (!deadline)
          {
            return ReceiveTimeout{};
          }
        }
        if (expired(deadline))
        {
          return ReceiveTimeout{};
        }
        std::this_thread::yield();
      }
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  OperationResult runtime_acknowledge_source(const std::shared_ptr<SourceQueueSetState>& state,
                                               const DeliveryToken& token, SourceOperationHooks* hooks)
  {
    return progress_source(state, token, true, hooks);
  }

  OperationResult runtime_release_source_delivery(const std::shared_ptr<SourceQueueSetState>& state,
                                                    const DeliveryToken& token, SourceOperationHooks* hooks)
  {
    return progress_source(state, token, false, hooks);
  }
} // namespace angaria::detail

namespace angaria
{
  ReceiveResult SourceQueueSet::receive(std::chrono::nanoseconds timeout)
  {
    return detail::runtime_receive_source(_state, timeout);
  }

  OperationResult SourceQueueSet::acknowledge(const DeliveryToken& token)
  {
    return detail::runtime_acknowledge_source(_state, token);
  }

  OperationResult SourceQueueSet::release(const DeliveryToken& token)
  {
    return detail::runtime_release_source_delivery(_state, token);
  }
} // namespace angaria
