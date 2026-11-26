#pragma once

#include "runtime_state.h"

#include <chrono>
#include <memory>
#include <optional>

namespace angaria::detail
{
  // Gate-bound loss of an overwritten pending prefix. The caller authenticates
  // its native Task before acquiring RuntimeAccess; this rechecks the captured
  // claim and complete topology. It neither issues a delivery nor appends data.
  // An empty optional means no overwritten prefix requires a gap. As with
  // other reserved progress, OutcomeUnknown requires recovery before reuse.
  Result<std::optional<DeliveryGap>> runtime_lose_delivery_prefix(
      RuntimeAccess& access, const std::shared_ptr<TaskState>& consumer,
      SourceQueueSetId source, QueueId queue, UtcTimestamp lostAt,
      std::optional<std::chrono::steady_clock::time_point> deadline = {});
} // namespace angaria::detail
