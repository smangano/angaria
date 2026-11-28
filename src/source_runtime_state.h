#pragma once

#include "public_runtime.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>

namespace angaria::detail
{
  enum class SourcePhase
  {
    Active,
    Closed
  };

  struct SourceReservation
  {
    SourceQueueSetId _id{};
    TaskClaim _claim;
    std::atomic<SourcePhase> _phase{SourcePhase::Active};
  };

  class SourceQueueSetState
  {
  public:
    ~SourceQueueSetState();
    std::shared_ptr<TaskState> _task;
    std::uint64_t _ownerPid{};
    std::thread::id _ownerThread;
    bool _threadAffine{};
    SourceQueueSetId _id{};
    std::shared_ptr<SourceReservation> _reservation;
    std::variant<recorded::RecordedConsumer, recorded::RecordedAcknowledgedConsumer> _consumer;
    std::mutex _mutex;
    bool _registered{};
  };

  std::shared_ptr<TaskState> runtime_source_task(const std::shared_ptr<SourceQueueSetState>& state);
  OperationResult runtime_close_source(const std::shared_ptr<SourceQueueSetState>& state);
  ReceiveResult runtime_receive_source(const std::shared_ptr<SourceQueueSetState>& state,
                                         std::chrono::nanoseconds timeout);
  OperationResult runtime_acknowledge_source(const std::shared_ptr<SourceQueueSetState>& state, const DeliveryToken& token);
  OperationResult runtime_release_source_delivery(const std::shared_ptr<SourceQueueSetState>& state, const DeliveryToken& token);
}
