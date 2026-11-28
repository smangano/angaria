#include "public_runtime.h"
#include "registry_access.h"
#include "topology_state.h"

#include <mutex>
#include <optional>
#include <utility>

namespace angaria::detail
{
  std::shared_ptr<const RecordedSessionResources> runtime_task_resources(const std::shared_ptr<TaskState>& task,
      std::optional<QueueId> queue, std::optional<SourceQueueSetId> source, bool admit)
  {
    runtime_check_messaging_owner(task);
    std::lock_guard lock(task->_endpointMutex);
    if (!task->_recordedResources)
    {
      RuntimeAccess access(task->_session);
      runtime_check_task(access, task);
      if (queue || source)
      {
        const auto topology = runtime_topology(access);
        if (queue)
        {
          const auto& selected = require_registered_entity(topology._queues, *queue, "writer.queue");
          if (selected._definition._descriptor._producer != task->_claim._task)
          {
            runtime_fail(ErrorCode::WrongOwner, "writer.queue", "Task does not own this Queue");
          }
          if (selected._definition._state != EntityState::Active)
          {
            runtime_fail(ErrorCode::NotActive, "writer.queue", "Queue is not Active");
          }
        }
        if (source)
        {
          const auto& selected = require_registered_entity(topology._sources, *source, "source.id");
          if (selected._definition._descriptor._owner != task->_claim._task)
          {
            runtime_fail(ErrorCode::WrongOwner, "source.owner", "Task does not own this Source");
          }
          if (selected._definition._state != EntityState::Active)
          {
            runtime_fail(ErrorCode::NotActive, "source.state", "Source is not Active");
          }
          if (selected._definition._members.size() != 1)
          {
            runtime_fail(ErrorCode::UnsupportedPolicy, "source.members", "Point-to-point reception requires one input");
          }
        }
      }
      task->_recordedResources = runtime_recorded_resources(access, admit ? task->_claim : TaskClaim{});
    }
    return task->_recordedResources;
  }

  std::shared_ptr<recorded::ProducerLog> runtime_producer_log(const std::shared_ptr<TaskState>& task, bool admit)
  {
    // Callers have checked ownership before taking this Task's producer mutex.
    // Their retained Task keeps its lease alive throughout the operation.
    if (!task->_producerLog)
    {
      const auto resources = runtime_task_resources(task, {}, {}, admit);
      const auto found = resources->_producers.find(task->_claim._task);
      if (found == resources->_producers.end())
      {
        runtime_fail(ErrorCode::NotActive, "task.producer", "Configure an outgoing Queue before issuing publications");
      }
      task->_producerLog = std::make_shared<recorded::ProducerLog>(runtime_value(
          recorded::ProducerLog::open_writer(found->second._paths, found->second._options, nullptr, task->_claim)));
    }
    return task->_producerLog;
  }

  PublicationReport runtime_recorded_publication_status(const std::shared_ptr<TaskState>& task,
      PublicationId publication, std::span<DestinationOutcome> outcomes)
  {
    const auto log = runtime_producer_log(task, false);
    const auto reference = runtime_value(log->lookup(publication));
    if (!reference)
    {
      return {publication, PublicationStatus::Rejected, task->_recordedResources->_catalog._revision, {}, 0,
              ErrorCode::None};
    }
    const auto record = runtime_value(log->view(*reference));
    if (outcomes.empty())
    {
      return {publication, PublicationStatus::OutcomeUnknown, record._topologyRevision, {}, 1,
              ErrorCode::OutputBufferTooSmall};
    }
    const auto destination = runtime_value(record.destination_at(0));
    outcomes[0] = {destination._queue, DestinationStatus::Accepted, ErrorCode::None, destination._queueSequence};
    return {publication, PublicationStatus::Accepted, record._topologyRevision, outcomes.first(1), 1, ErrorCode::None};
  }
} // namespace angaria::detail
