#pragma once

#include "topology_state.h"

#include <span>

namespace angaria::detail
{
  // Validate recovered core delivery history against this already checked
  // topology and its complete decoded Queue/Source histories. All inputs must
  // come from this same RuntimeAccess under its continuously held gate, without
  // intervening Session mutation. Borrowed history does not escape this call.
  // Immutable Publication checks and fresh atomic mutable-state reads remain;
  // this function never starts delivery, changes state, or calls runtime_topology.
  void runtime_validate_delivery(RuntimeAccess& access, const TopologySnapshot& topology,
      std::span<const RetainedTopologyDefinition<mapped::QueueDefinition>> queues,
      std::span<const RetainedTopologyDefinition<mapped::SourceDefinition>> sources);
} // namespace angaria::detail
