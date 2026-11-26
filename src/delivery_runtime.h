#pragma once

#include "topology_state.h"

namespace angaria::detail
{
  // Validate recovered core delivery history against this already checked
  // topology. This reads immutable bytes and atomic local copies under access's
  // gate; it never starts delivery, changes state, or calls runtime_topology.
  void runtime_validate_delivery(RuntimeAccess& access, const TopologySnapshot& topology);
} // namespace angaria::detail
