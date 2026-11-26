#pragma once

#include "runtime_state.h"

#include <cstdint>

namespace angaria::detail
{
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
  // The future endpoint caller must authenticate its Task's native owner before
  // constructing RuntimeAccess. These functions recheck the captured claim and
  // complete recovered topology under its guard. They do not acquire mappings.
  // An existing Source pin is authoritative: issue returns it unchanged before
  // considering a fresh Queue selection, log space, or exhausted counters.
  // Success advances access's operation generation for same-guard cleanup.
  // An uncertain result requires recovery before that access is reused.
  Result<DeliveryProgress> runtime_issue_delivery(RuntimeAccess& access, const TaskClaim& consumer,
                                                   SourceQueueSetId source, QueueId queue,
                                                   UtcTimestamp dequeuedAt);
  OperationResult runtime_acknowledge_delivery(RuntimeAccess& access, const DeliveryToken& token,
                                                 UtcTimestamp acknowledgedAt);
  OperationResult runtime_release_delivery(RuntimeAccess& access, const DeliveryToken& token);
  // Claim-bound Source cleanup releases its current issuance; an idle Source
  // is an unchanged success. Counters/context are preserved. Deferred cleanup
  // of a captured older issuance must use its exact token, not this operation.
  OperationResult runtime_release_source(RuntimeAccess& access, const TaskClaim& consumer,
                                           SourceQueueSetId source);
} // namespace angaria::detail
