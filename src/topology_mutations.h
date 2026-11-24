#pragma once

#include "topology_state.h"

#include <cstdint>
#include <memory>
#include <span>

namespace angaria::detail
{
  enum class TopologyMutation
  {
    AdoptQueue,
    AdoptSource,
    AdoptPublishing,
    AssignSource,
    SetDestinations,
    ActivateQueue,
    ActivateSource,
    ActivatePublishing
  };

  // Borrowed input for one synchronous mutation. Public wrappers supply typed
  // identities; only qualification workers supply a journal fault injector.
  struct TopologyChange
  {
    TopologyMutation _operation{};
    std::uint16_t _id{};
    TaskId _owner{};
    SourceQueueSetId _source{};
    std::uint8_t _priority{1};
    std::span<const QueueId> _destinations;
  };

  OperationResult runtime_update_topology(std::shared_ptr<SessionState> session,
                                          std::shared_ptr<TaskState> actor,
                                          const TopologyChange& change,
                                          mapped::JournalFaultInjector* injector = nullptr);
} // namespace angaria::detail
