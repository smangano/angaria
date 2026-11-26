#pragma once

#include "delivery_progress.h"
#include "mapped_payload.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

namespace angaria::detail
{
  enum class SourcePhase
  {
    Active,
    Closing,
    DeferredClose,
    Closed
  };

  // Allocated when opening, and retained by the Task after an unsuccessful
  // destructor cleanup. It never owns a Task, mapping, or application view.
  struct SourceReservation
  {
    SourceQueueSetId _id{};
    TaskClaim _claim;
    std::atomic<SourcePhase> _phase{SourcePhase::Active};
    std::optional<DeliveryToken> _token;
    // Set before fresh issuance: recovery may install its pin even if no token
    // was returned. Discovery may clean only this exact claim/issuance number.
    std::optional<std::uint64_t> _pendingIssuance;
  };

  // Private qualification hooks. Production endpoints do not install hooks;
  // tests keep their hook/injector alive until the endpoint closes.
  struct SourceOperationHooks
  {
    virtual ~SourceOperationHooks() = default;
    mapped::JournalFaultInjector* _journal{};
    RuntimeAccessReadObserver* _reads{};
    virtual OperationResult before_mapping(const DeliveryProgress&)
    {
      return std::monostate{};
    }
    virtual OperationResult before_binding(const DeliveryProgress&)
    {
      return std::monostate{};
    }
  };

  class SourceQueueSetState
  {
  public:
    ~SourceQueueSetState();
    std::shared_ptr<TaskState> _task;
    // Immutable native ownership tags reject inherited/wrong-thread use before
    // atomic shared_ptr snapshots, whose implementation may use a local lock.
    std::uint64_t _ownerPid{};
    std::thread::id _ownerThread;
    bool _threadAffine{};
    SourceQueueSetId _id{};
    std::shared_ptr<SourceReservation> _reservation;
    MappedPayload _payload;
    std::optional<Delivery> _delivery;
    SourceOperationHooks* _hooks{};
    bool _registered{};
  };

  // Gate-bound reconciliation changes only preallocated deferred reservations.
  // Call after current-claim authentication, before new delivery/progress work.
  void runtime_reconcile_sources(RuntimeAccess& access, const std::shared_ptr<TaskState>& task);
  std::shared_ptr<TaskState> runtime_source_task(const std::shared_ptr<SourceQueueSetState>& state);
  void runtime_require_source_active(const std::shared_ptr<SourceQueueSetState>& state);
  void runtime_clear_source_view(SourceQueueSetState& state) noexcept;
  OperationResult runtime_close_source(const std::shared_ptr<SourceQueueSetState>& state,
                                         SourceOperationHooks* hooks = nullptr);
  void runtime_abandon_source(const std::shared_ptr<SourceQueueSetState>& state) noexcept;

  ReceiveResult runtime_receive_source(const std::shared_ptr<SourceQueueSetState>& state,
                                         std::chrono::nanoseconds timeout,
                                         SourceOperationHooks* hooks = nullptr);
  OperationResult runtime_acknowledge_source(const std::shared_ptr<SourceQueueSetState>& state,
                                               const DeliveryToken& token,
                                               SourceOperationHooks* hooks = nullptr);
  OperationResult runtime_release_source_delivery(const std::shared_ptr<SourceQueueSetState>& state,
                                                    const DeliveryToken& token,
                                                    SourceOperationHooks* hooks = nullptr);

  struct SourceAccess
  {
    static std::shared_ptr<SourceQueueSetState> state(const SourceQueueSet& source);
    static void set_hooks(SourceQueueSet& source, SourceOperationHooks* hooks);
  };
} // namespace angaria::detail
