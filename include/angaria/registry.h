#pragma once

#include "angaria/descriptors.h"
#include "angaria/results.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <vector>

/**
 * The registry stores metadata in shared memory. Registered tasks can read and update it.
 * Updates persist for the lifetime of the session.
 */

namespace angaria
{
  class Session;

  namespace detail
  {
    class SessionState;
    class TaskState;
  } // namespace detail

  class Registry
  {
  public:
    // Register an immutable logical Task through a live actor-bound Registry.
    // An exact descriptor returns its existing ID; registration does not attach it.
    Result<TaskId> register_task(const TaskDescriptor& descriptor);
    // Register draft topology. A Queue's Source membership commits with it;
    // policy inspection returns resolved values, and exact retries are no-ops.
    Result<SourceQueueSetId> register_source_set(const SourceQueueSetDescriptor& descriptor);
    Result<QueueId> register_queue(const QueueDescriptor& descriptor);
    Result<PublishingQueueSetId> register_publishing_set(const PublishingQueueSetDescriptor& descriptor);
    // Definitions are immutable. Exact descriptors return their existing IDs;
    // a message schema preserves its ordered, possibly repeated Field IDs.
    Result<FieldId> register_field(const FieldDescriptor& descriptor);
    Result<SchemaId> register_message(const MessageDescriptor& descriptor);

    Result<TaskId> find_task(std::string_view name) const;
    Result<QueueId> find_queue(std::string_view name) const;
    Result<SourceQueueSetId> find_source_set(std::string_view name) const;
    Result<PublishingQueueSetId> find_publishing_set(std::string_view name) const;
    Result<FieldId> find_field(std::string_view name) const;
    Result<SchemaId> find_message(std::string_view name, std::uint32_t version) const;

    Result<TaskDescriptor> describe_task(TaskId id) const;
    Result<QueueDescriptor> describe_queue(QueueId id) const;
    Result<SourceQueueSetDescriptor> describe_source_set(SourceQueueSetId id) const;
    Result<PublishingQueueSetDescriptor> describe_publishing_set(PublishingQueueSetId id) const;
    Result<FieldDescriptor> describe_field(FieldId id) const;
    Result<MessageDescriptor> describe_message(SchemaId id) const;
    Result<std::vector<QueueId>> source_members(SourceQueueSetId id) const;

    // Copy one coherent catalog under the mutation gate. Omission selects the
    // current revision; 1..current select retained final definitions at that
    // Registry revision. Zero is InvalidArgument and a future cut is NotFound.
    // Session views may inspect without claiming a Task. Validation/recovery
    // can still write the Session; this is not a read-only file-open operation.
    Result<RegistrySnapshot> snapshot(std::optional<std::uint64_t> revision = std::nullopt) const;

    Result<RegistrationMetadata> metadata(TaskId id) const;
    Result<RegistrationMetadata> metadata(QueueId id) const;
    Result<RegistrationMetadata> metadata(SourceQueueSetId id) const;
    Result<RegistrationMetadata> metadata(PublishingQueueSetId id) const;
    Result<RegistrationMetadata> metadata(FieldId id) const;
    Result<RegistrationMetadata> metadata(SchemaId id) const;
    Result<TaskRuntimeInfo> task_status(TaskId id) const;

    Result<EntityState> state(QueueId id) const;
    Result<EntityState> state(SourceQueueSetId id) const;
    Result<EntityState> state(PublishingQueueSetId id) const;

    OperationResult activate_source_set(SourceQueueSetId id);
    OperationResult activate_queue(QueueId id);
    OperationResult activate_publishing_set(PublishingQueueSetId id);
    OperationResult adopt_queue(QueueId id, TaskId producer);
    OperationResult adopt_source_set(SourceQueueSetId id, TaskId owner);
    OperationResult adopt_publishing_set(PublishingQueueSetId id, TaskId owner);
    OperationResult assign_source(QueueId id, SourceQueueSetId source, std::uint8_t priority);
    OperationResult set_destinations(PublishingQueueSetId id, std::span<const QueueId> destinations);

    Registry() = default;

  private:
    friend class Session;
    friend class Task;
    explicit Registry(std::shared_ptr<detail::SessionState> state,
                      std::shared_ptr<detail::TaskState> actor = {});
    std::shared_ptr<detail::SessionState> _state;
    std::shared_ptr<detail::TaskState> _actor;
  };
} // namespace angaria
