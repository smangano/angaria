#pragma once

#include "topology_state.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace angaria::detail
{
  // One common allocator/preflight path for topology registration and revisions.
  // Body sizes include either immutable definitions or a ring's mutable slots.
  struct PreparedAppend
  {
    mapped::TransactionPlan _plan;
    std::vector<std::uint64_t> _references;
  };

  void check_task(TaskId id, const TopologySnapshot& topology, const char* path);
  std::uint64_t add_bytes(std::uint64_t left, std::uint64_t right);
  std::uint64_t multiply_bytes(std::uint64_t count, std::uint64_t width);
  std::uint64_t record_bytes(std::uint64_t bodyBytes);
  // Shared complete append accounting, including record references, redo,
  // envelopes, and alignment. Configuration preflight uses the executor's math.
  std::uint64_t append_tail(std::uint64_t tail, std::span<const std::uint64_t> bodies,
                             std::uint64_t commandBytes, std::uint64_t maxLogBytes);
  PreparedAppend prepare_append(RuntimeAccess& access, const std::shared_ptr<TaskState>& actor,
                                  std::span<const std::uint64_t> bodies, std::uint64_t commandBytes);
  void add_index(PreparedAppend& prepared, std::uint64_t offset, std::uint16_t id,
                   std::uint64_t reference, EntityState state = EntityState::Draft);
  void add_membership(PreparedAppend& prepared, RuntimeAccess& access,
                        std::uint64_t group, std::uint64_t slot,
                        QueueId queue, std::uint8_t priority, std::uint64_t revision);
} // namespace angaria::detail
