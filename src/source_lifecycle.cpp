#include "source_runtime_state.h"

#include "registry_access.h"
#include "topology_state.h"

#include <memory>
#include <mutex>
#include <utility>
#include <variant>

#include <unistd.h>

namespace angaria::detail
{
  namespace
  {
    void clean_reservation(RuntimeAccess& access, const SourceReservation& reservation)
    {
      const auto topology = runtime_topology(access);
      const auto& source = require_registered_entity(topology._sources, reservation._id, "source.close");
      if (source._definition._descriptor._owner != reservation._claim._task)
      {
        runtime_fail(ErrorCode::StaleDelivery, "source.close", "Source no longer belongs to the closing consumer");
      }
      const auto words = access.read_words(source._stateOffset, 8);
      if (words[2] == 0)
      {
        // An already completed release/acknowledgment is a settled cleanup.
        // Retain its issuance counter so the next borrow cannot reuse a token.
        return;
      }
      const DeliveryToken current{reservation._claim, reservation._id,
          QueueId{static_cast<std::uint16_t>(words[2])}, words[3], words[0]};
      if (words[1] != reservation._claim._incarnation ||
          (reservation._token ? *reservation._token != current :
              !reservation._pendingIssuance || *reservation._pendingIssuance != current._issuance))
      {
        runtime_fail(ErrorCode::StaleDelivery, "source.close", "Cleanup identity does not match the Source's current issuance");
      }
      // The current source slot authenticates an unreturned issuance against
      // the counter captured before issue; an older marker cannot clear a newer
      // borrow. Release checks every token component again before mutation.
      runtime_value(runtime_release_delivery(access, current));
    }

    void finish_reservation(RuntimeAccess& access, const std::shared_ptr<TaskState>& task,
                              const std::shared_ptr<SourceReservation>& reservation)
    {
      clean_reservation(access, *reservation);
      reservation->_token.reset();
      reservation->_pendingIssuance.reset();
      {
        std::lock_guard lock(task->_endpointMutex);
        const auto found = task->_openSources.find(reservation->_id);
        if (found != task->_openSources.end() && found->second == reservation)
        {
          task->_openSources.erase(found);
        }
      }
      reservation->_phase.store(SourcePhase::Closed, std::memory_order_release);
    }

    OperationResult close_source(SourceQueueSetState* state, SourceOperationHooks* hooks)
    {
      if (!state || !state->_reservation ||
          state->_reservation->_phase.load(std::memory_order_acquire) == SourcePhase::Closed)
      {
        return std::monostate{};
      }
      try
      {
        if (state->_ownerPid != static_cast<std::uint64_t>(::getpid()) ||
            (state->_threadAffine && state->_ownerThread != std::this_thread::get_id()))
        {
          runtime_fail(ErrorCode::WrongOwner, "source.owner", "Source belongs to another process or thread");
        }
        auto task = std::atomic_load_explicit(&state->_task, std::memory_order_acquire);
        runtime_check_task_owner(task);
        auto& phase = state->_reservation->_phase;
        auto expected = SourcePhase::Active;
        (void)phase.compare_exchange_strong(expected, SourcePhase::Closing,
                                             std::memory_order_acq_rel);
        auto* selected = hooks ? hooks : state->_hooks;
        {
          RuntimeAccess access(task->_session, true, selected ? selected->_journal : nullptr);
          runtime_check_task(access, task);
          // Other deferred Sources fence Task progress too; keep the requested
          // Source's explicit Closing reservation until its own cleanup settles.
          runtime_reconcile_sources(access, task);
          if (phase.load(std::memory_order_acquire) != SourcePhase::Closed)
          {
            finish_reservation(access, task, state->_reservation);
          }
          runtime_clear_source_view(*state);
          state->_registered = false;
        }
        // Ending the final endpoint owner may detach the Task and take the same
        // mutation gate. Never drop that last owner while RuntimeAccess is alive.
        std::atomic_store_explicit(&state->_task, std::shared_ptr<TaskState>{}, std::memory_order_release);
        return std::monostate{};
      }
      catch (...)
      {
        return runtime_caught_error();
      }
    }

    void abandon_source(SourceQueueSetState& state) noexcept
    {
      if (!state._registered || !state._reservation ||
          state._reservation->_phase.load(std::memory_order_acquire) == SourcePhase::Closed)
      {
        return;
      }
      // An inherited process must not touch a local mutex or shared storage.
      // The reservation itself is already allocated; marking it cannot throw.
      if (state._ownerPid != static_cast<std::uint64_t>(::getpid()))
      {
        return;
      }
      state._reservation->_phase.store(SourcePhase::DeferredClose, std::memory_order_release);
      try
      {
        (void)close_source(&state, nullptr);
      }
      catch (...)
      {
        // Even constructing an allocation-failure diagnostic may fail. The
        // already installed reservation still fences subsequent Task work.
      }
      // No application Source owner remains now. A failed close leaves only
      // the preallocated marker in TaskState, never a mapping or Task cycle.
      runtime_clear_source_view(state);
    }
  } // namespace

  void runtime_require_source_active(const std::shared_ptr<SourceQueueSetState>& state)
  {
    if (!state || !state->_reservation ||
        state->_reservation->_phase.load(std::memory_order_acquire) != SourcePhase::Active)
    {
      runtime_fail(ErrorCode::InvalidState, "source.state", "Source is closing, closed, or moved from");
    }
  }

  std::shared_ptr<TaskState> runtime_source_task(const std::shared_ptr<SourceQueueSetState>& state)
  {
    if (!state)
    {
      runtime_fail(ErrorCode::InvalidState, "source.state", "Source is moved from");
    }
    if (state->_ownerPid != static_cast<std::uint64_t>(::getpid()) ||
        (state->_threadAffine && state->_ownerThread != std::this_thread::get_id()))
    {
      runtime_fail(ErrorCode::WrongOwner, "source.owner", "Source belongs to another process or thread");
    }
    return std::atomic_load_explicit(&state->_task, std::memory_order_acquire);
  }

  void runtime_clear_source_view(SourceQueueSetState& state) noexcept
  {
    state._delivery.reset();
    state._payload = {};
  }

  void runtime_reconcile_sources(RuntimeAccess& access, const std::shared_ptr<TaskState>& task)
  {
    // Snapshot only one shared reservation at a time. Release bookkeeping before
    // any journal work, while retaining the operation's already acquired gate.
    for (;;)
    {
      std::shared_ptr<SourceReservation> deferred;
      {
        std::lock_guard lock(task->_endpointMutex);
        for (const auto& [id, reservation] : task->_openSources)
        {
          (void)id;
          if (reservation->_phase.load(std::memory_order_acquire) == SourcePhase::DeferredClose)
          {
            deferred = reservation;
            break;
          }
        }
      }
      if (!deferred)
      {
        return;
      }
      if (deferred->_claim != task->_claim)
      {
        runtime_fail(ErrorCode::StaleDelivery, "source.close", "Deferred Source belongs to another Task claim");
      }
      finish_reservation(access, task, deferred);
    }
  }

  OperationResult runtime_close_source(const std::shared_ptr<SourceQueueSetState>& state,
                                         SourceOperationHooks* hooks)
  {
    return close_source(state.get(), hooks);
  }

  void runtime_abandon_source(const std::shared_ptr<SourceQueueSetState>& state) noexcept
  {
    if (state)
    {
      abandon_source(*state);
    }
  }

  SourceQueueSetState::~SourceQueueSetState()
  {
    abandon_source(*this);
  }

  std::shared_ptr<SourceQueueSetState> SourceAccess::state(const SourceQueueSet& source)
  {
    return source._state;
  }

  void SourceAccess::set_hooks(SourceQueueSet& source, SourceOperationHooks* hooks)
  {
    if (source._state)
    {
      source._state->_hooks = hooks;
    }
  }
} // namespace angaria::detail

namespace angaria
{
  SourceQueueSet::~SourceQueueSet()
  {
    detail::runtime_abandon_source(_state);
  }

  SourceQueueSet::SourceQueueSet(SourceQueueSet&& other) noexcept : _state(std::move(other._state))
  {
  }

  SourceQueueSet& SourceQueueSet::operator=(SourceQueueSet&& other) noexcept
  {
    if (this != &other)
    {
      detail::runtime_abandon_source(_state);
      _state = std::move(other._state);
    }
    return *this;
  }

  OperationResult SourceQueueSet::close()
  {
    return detail::runtime_close_source(_state);
  }
} // namespace angaria
