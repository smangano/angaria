#pragma once

#include "runtime_state.h"

#include <cstdint>

namespace angaria::detail
{
  struct TopologySnapshot;

  struct DeliveryProgress
  {
    DeliveryToken _token;
    std::uint64_t _publicationReference{};
    std::uint64_t _receiptReference{};
    std::uint64_t _deliveryLink{};
    UtcTimestamp _dequeuedAt{};

    bool operator==(const DeliveryProgress&) const = default;
  };

  // Private reserved-state primitives, not public receive/acknowledge methods.
  // The endpoint caller must authenticate its Task's native owner before
  // constructing RuntimeAccess. These functions recheck the captured claim;
  // generic forms validate complete recovered topology under its guard. The
  // trusted receive overload below reuses that validation. These primitives
  // do not acquire borrowed-payload mappings.
  // An existing Source pin is authoritative: issue returns it unchanged before
  // considering a fresh Queue selection, log space, or exhausted counters.
  // Success advances access's operation generation for same-guard cleanup.
  // An uncertain result requires recovery before that access is reused.
  Result<DeliveryProgress> runtime_issue_delivery(RuntimeAccess& access, const TaskClaim& consumer,
                                                   SourceQueueSetId source, QueueId queue,
                                                   UtcTimestamp dequeuedAt);
  // Trusted receive-only reuse: topology must be the complete result of
  // runtime_topology(access) under this same continuously held guard, with no
  // intervening Session mutation. Do not supply an arbitrary/stale snapshot.
  // Live guard, claim/parent, Source, Queue and receipt checks still run; only
  // the duplicate complete topology/delivery validation is omitted.
  Result<DeliveryProgress> runtime_issue_delivery(RuntimeAccess& access, const TopologySnapshot& topology,
                                                   const TaskClaim& consumer, SourceQueueSetId source,
                                                   QueueId queue, UtcTimestamp dequeuedAt);
  OperationResult runtime_acknowledge_delivery(RuntimeAccess& access, const DeliveryToken& token,
                                                 UtcTimestamp acknowledgedAt);
  OperationResult runtime_release_delivery(RuntimeAccess& access, const DeliveryToken& token);
  // Claim-bound Source cleanup releases its current issuance; an idle Source
  // is an unchanged success. Counters/context are preserved. Deferred cleanup
  // of a captured older issuance must use its exact token, not this operation.
  OperationResult runtime_release_source(RuntimeAccess& access, const TaskClaim& consumer,
                                           SourceQueueSetId source);
} // namespace angaria::detail
