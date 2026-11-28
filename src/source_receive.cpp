#include "source_runtime_state.h"

#include <chrono>
#include <limits>
#include <mutex>
#include <thread>
#include <variant>

namespace angaria::detail
{
  namespace
  {
    void require_active(const SourceQueueSetState& state)
    {
      if (!state._reservation || state._reservation->_phase.load(std::memory_order_acquire) != SourcePhase::Active)
      {
        runtime_fail(ErrorCode::InvalidState, "source", "Source is closed");
      }
    }
  }

  ReceiveResult runtime_receive_source(const std::shared_ptr<SourceQueueSetState>& state, std::chrono::nanoseconds timeout)
  {
    try
    {
      const auto task = runtime_source_task(state);
      runtime_check_messaging_owner(task);
      if (timeout.count() < 0)
      {
        runtime_fail(ErrorCode::InvalidArgument, "source.timeout", "Receive timeout cannot be negative");
      }
      const auto start = timeout.count() == 0 ? std::chrono::steady_clock::time_point{} : std::chrono::steady_clock::now();
      const auto remaining = std::chrono::steady_clock::time_point::max() - start;
      if (timeout > remaining)
      {
        runtime_fail(ErrorCode::InvalidArgument, "source.timeout", "Receive timeout exceeds the clock range");
      }
      const auto deadline = start + timeout;
      for (;;)
      {
        {
          std::lock_guard lock(state->_mutex);
          require_active(*state);
          if (auto* consumer = std::get_if<recorded::RecordedAcknowledgedConsumer>(&state->_consumer))
          {
            auto delivery = runtime_value(consumer->try_receive());
            if (delivery)
            {
              return std::move(*delivery);
            }
          }
          else
          {
            auto message = runtime_value(std::get<recorded::RecordedConsumer>(state->_consumer).try_receive());
            if (message)
            {
              return std::move(*message);
            }
          }
        }
        if (timeout.count() == 0 || std::chrono::steady_clock::now() >= deadline)
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

  OperationResult runtime_acknowledge_source(const std::shared_ptr<SourceQueueSetState>& state, const DeliveryToken& token)
  {
    try
    {
      runtime_check_messaging_owner(runtime_source_task(state));
      std::lock_guard lock(state->_mutex);
      require_active(*state);
      const auto consumer = std::get_if<recorded::RecordedAcknowledgedConsumer>(&state->_consumer);
      if (!consumer)
      {
        runtime_fail(ErrorCode::UnsupportedPolicy, "source.acknowledgment", "This Source has no acknowledgment obligation");
      }
      return consumer->acknowledge(token);
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  OperationResult runtime_release_source_delivery(const std::shared_ptr<SourceQueueSetState>& state, const DeliveryToken& token)
  {
    try
    {
      runtime_check_messaging_owner(runtime_source_task(state));
      std::lock_guard lock(state->_mutex);
      require_active(*state);
      auto* consumer = std::get_if<recorded::RecordedAcknowledgedConsumer>(&state->_consumer);
      if (!consumer)
      {
        runtime_fail(ErrorCode::UnsupportedPolicy, "source.acknowledgment", "This Source has no processing token to release");
      }
      return consumer->release(token);
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }
}

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
}
