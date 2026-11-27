#pragma once

#include "runtime_state.h"
#include "topology_metadata.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace angaria::detail
{
  // Owning decoded topology history within one gate-held operation. Delivery
  // validation borrows these records only while their checked owner is alive.
  template <typename Definition>
  struct RetainedTopologyDefinition
  {
    Definition _definition;
    std::uint64_t _reference{};
  };

  struct LoadedQueue
  {
    mapped::QueueDefinition _definition;
    std::uint64_t _reference{};
    std::uint64_t _indexOffset{};
    std::uint64_t _stateOffset{};
    std::uint64_t _slot{};
  };

  struct LoadedSource
  {
    mapped::SourceDefinition _definition;
    std::uint64_t _reference{};
    std::uint64_t _indexOffset{};
    std::uint64_t _stateOffset{};
    std::uint64_t _slot{};
  };

  struct LoadedPublishing
  {
    mapped::PublishingDefinition _definition;
    std::uint64_t _reference{};
    std::uint64_t _indexOffset{};
    std::uint64_t _stateOffset{};
    std::uint64_t _slot{};
  };

  // One recovered, gate-bound snapshot validates all current relationships and
  // their retained definition chains before any typed view uses a subset.
  struct TopologySnapshot
  {
    std::vector<LoadedTask> _tasks;
    std::vector<LoadedQueue> _queues;
    std::vector<LoadedSource> _sources;
    std::vector<LoadedPublishing> _publishing;
  };

  TopologySnapshot runtime_topology(RuntimeAccess& access);

  Result<QueueId> runtime_register_queue(std::shared_ptr<SessionState> session,
                                         std::shared_ptr<TaskState> actor,
                                         const QueueDescriptor& descriptor,
                                         mapped::JournalFaultInjector* injector = nullptr);
  Result<SourceQueueSetId> runtime_register_source_set(std::shared_ptr<SessionState> session,
                                                      std::shared_ptr<TaskState> actor,
                                                      const SourceQueueSetDescriptor& descriptor,
                                                      mapped::JournalFaultInjector* injector = nullptr);
  Result<PublishingQueueSetId> runtime_register_publishing_set(std::shared_ptr<SessionState> session,
                                                              std::shared_ptr<TaskState> actor,
                                                              const PublishingQueueSetDescriptor& descriptor,
                                                              mapped::JournalFaultInjector* injector = nullptr);
} // namespace angaria::detail
