#pragma once

#include "public_runtime_storage.h"

namespace angaria::detail
{
  // Called while this Task's producer mutex is held. Initial binding performs
  // administrative validation once; established messaging never opens a gate.
  std::shared_ptr<recorded::ProducerLog> runtime_producer_log(const std::shared_ptr<TaskState>& task, bool admit = true);
  std::shared_ptr<const RecordedSessionResources> runtime_task_resources(const std::shared_ptr<TaskState>& task,
      std::optional<QueueId> queue = {}, std::optional<SourceQueueSetId> source = {}, bool admit = true);
  PublicationReport runtime_recorded_publication_status(const std::shared_ptr<TaskState>& task,
      PublicationId publication, std::span<DestinationOutcome> outcomes);
} // namespace angaria::detail
