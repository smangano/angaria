#include "source_runtime_state.h"

#include <memory>
#include <mutex>
#include <utility>
#include <variant>
#include <unistd.h>

namespace angaria::detail
{
  namespace
  {
    OperationResult close_source(SourceQueueSetState* state)
    {
      if (!state || !state->_reservation || state->_reservation->_phase.load(std::memory_order_acquire) == SourcePhase::Closed)
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
        const auto task = std::atomic_load_explicit(&state->_task, std::memory_order_acquire);
        runtime_check_messaging_owner(task);
        std::lock_guard lock(state->_mutex);
        if (state->_reservation->_phase.load(std::memory_order_acquire) == SourcePhase::Closed)
        {
          return std::monostate{};
        }
        if (auto* consumer = std::get_if<recorded::RecordedAcknowledgedConsumer>(&state->_consumer))
        {
          // Failure keeps the live handle and reservation available for retry.
          runtime_value(consumer->close());
        }
        state->_consumer = recorded::RecordedConsumer{};
        {
          std::lock_guard endpointLock(task->_endpointMutex);
          const auto found = task->_openSources.find(state->_id);
          if (found != task->_openSources.end() && found->second == state->_reservation)
          {
            task->_openSources.erase(found);
          }
        }
        state->_registered = false;
        state->_reservation->_phase.store(SourcePhase::Closed, std::memory_order_release);
        std::atomic_store_explicit(&state->_task, std::shared_ptr<TaskState>{}, std::memory_order_release);
        return std::monostate{};
      }
      catch (...)
      {
        return runtime_caught_error();
      }
    }
  }

  SourceQueueSetState::~SourceQueueSetState()
  {
    (void)close_source(this);
    // A failed last-handle close ends its borrow and releases native ownership.
    // Remove the local reservation even when a Thread handle is destroyed on
    // another thread: no live Source remains to justify blocking its owner.
    // A fork child must still leave its parent's local bookkeeping untouched.
    if (_registered && _ownerPid == static_cast<std::uint64_t>(::getpid()))
    {
      const auto task = std::atomic_load_explicit(&_task, std::memory_order_acquire);
      if (task)
      {
        std::lock_guard lock(task->_endpointMutex);
        const auto found = task->_openSources.find(_id);
        if (found != task->_openSources.end() && found->second == _reservation)
        {
          task->_openSources.erase(found);
        }
      }
    }
  }

  std::shared_ptr<TaskState> runtime_source_task(const std::shared_ptr<SourceQueueSetState>& state)
  {
    if (!state)
    {
      runtime_fail(ErrorCode::InvalidState, "source", "Source has no live handle");
    }
    if (state->_ownerPid != static_cast<std::uint64_t>(::getpid()) ||
        (state->_threadAffine && state->_ownerThread != std::this_thread::get_id()))
    {
      runtime_fail(ErrorCode::WrongOwner, "source.owner", "Source belongs to another process or thread");
    }
    return std::atomic_load_explicit(&state->_task, std::memory_order_acquire);
  }

  OperationResult runtime_close_source(const std::shared_ptr<SourceQueueSetState>& state)
  {
    return close_source(state.get());
  }
}

namespace angaria
{
  SourceQueueSet::~SourceQueueSet() = default;

  SourceQueueSet::SourceQueueSet(SourceQueueSet&& other) noexcept : _state(std::move(other._state))
  {
  }

  SourceQueueSet& SourceQueueSet::operator=(SourceQueueSet&& other) noexcept
  {
    if (this != &other)
    {
      _state = std::move(other._state);
    }
    return *this;
  }

  OperationResult SourceQueueSet::close()
  {
    return detail::runtime_close_source(_state);
  }
}
