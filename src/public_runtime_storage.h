#pragma once

#include "recorded_queue.h"
#include "runtime_state.h"

#include <map>
#include <memory>

namespace angaria::detail
{
  struct RecordedProducerResources
  {
    recorded::ProducerLogPaths _paths;
    recorded::ProducerLogOptions _options;
  };

  // Built from the authoritative, validated Registry under its administrative
  // gate. Established endpoints retain this immutable catalog independently.
  struct RecordedSessionResources
  {
    RegistrySnapshot _catalog;
    std::map<TaskId, RecordedProducerResources> _producers;
    std::map<QueueId, recorded::RecordedQueueOptions> _queues;
  };

  std::shared_ptr<const RecordedSessionResources> runtime_recorded_resources(RuntimeAccess& access, TaskClaim actor);
  void runtime_require_unsealed(RuntimeAccess& access);
} // namespace angaria::detail
