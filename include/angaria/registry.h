#pragma once

#include "angaria/bip.h"
#include "angaria/types.h"
#include "angaria/descriptors.h"
#include "angaria/results.h"
#include <boost/interprocess/sync/named_mutex.hpp>
#include <memory>
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

  /**
   * Registry metadata for a Task - an execution unit using Angaria to communicate.
   */
  struct RegistryTask
  {
    TID_t _taskId;
    char _taskName[NAMELEN];
    LCTOFF_t _createTime;     // Initial registration time relative to the log creation time.
    LCTOFF_t _lastCreateTime; // Most recent registration time relative to the log creation time.
    PID_t _lastPid;           // Process or thread ID from the most recent registration.
    bool _active;
    uint16_t _restartCount; // Zero on initial registration.
    VARCHAR_t _doc;
  };

  /**
   * Registry metadata for a Queue - the basic communication facility between Tasks
   */
  struct RegistryQueue
  {
    QID_t _queueId;
    char _queueName[NAMELEN];
    TID_t _createdByTask;
    QSID_t _ownedByQueueSet;
    LCTOFF_t _createTime;
    uint64_t _maxMessageCount;
    VARCHAR_t _doc;
  };

  /**
   * Registry metadata for a Queue Set - a collection of queues that a Task can use to
   * implement prioritized reads or multi task publishing.
   */
  struct RegistryQueueSet
  {
    QSID_t _queueSetId;
    char _queueSetName[NAMELEN];
    EQSetType _queueSetType;
    QID_t _membership[MAX_QUEUES_PER_SET];
    uint8_t _priority[MAX_QUEUES_PER_SET];
    LCTOFF_t _createTime;
    TID_t _createdByTask;
    TID_t _ownedByTask;
    VARCHAR_t _doc;
  };

  /**
   * Registry metadata for a Message - the basic unit of data that is transmitted between tasks
   * via queues.
   */

  struct RegistryMessage
  {
    EMsgType _msgType;
    char _msgName[NAMELEN];
    LCTOFF_t _createTime;
    uint32_t _msgVersion;
    TID_t _createTaskId;
    VARCHAR_t _doc;
  };

  /**
   * Registry metadata describing the fields that make up a message.
   */
  struct RegistryMessageDef
  {
    EMsgType _msgType;
    uint16_t _fieldCount;
    EFieldType _fields[];
  };

  /**
   * Registry metadata describing a specific field in a message.
   */
  struct RegistryField
  {
    EFieldType _fieldType;
    char _fieldName[NAMELEN];
    std::size_t _fieldSize;
    EFieldBase _fieldBaseType;
    VARCHAR_t _doc;
  };

  class Registry
  {
  public:
    Result<TaskId> register_task(const TaskDescriptor& descriptor);
    Result<SourceQueueSetId> register_source_set(const SourceQueueSetDescriptor& descriptor);
    Result<QueueId> register_queue(const QueueDescriptor& descriptor);
    Result<PublishingQueueSetId> register_publishing_set(const PublishingQueueSetDescriptor& descriptor);
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

    // Prototype registration overloads retained while the field API is migrated.
    using NamedMutex = boost::interprocess::named_mutex;
    using NamedMutexPtr = std::unique_ptr<NamedMutex>;

    Registry() = default;

    static Registry& instance()
    {
      static Registry theRegistry;
      return theRegistry;
    }

    TID_t register_task(const std::string& name, PID_t pid, const std::string& doc = "");

    QID_t register_queue(const std::string& name, TID_t owner, const std::string& doc = "");
    QID_t register_bounded_queue(const std::string& name, TID_t owner, uint64_t maxMsgs,
                                 const std::string& doc = "");

    /**
     * Register a Queue Set
     * name - the unique name of the queue set.
     * type - either a EQSetType::SOURCE or a EQSetType::PUBLISHER
     * creator - the task ID of the task creating the queue set. Also becomes the owner until another task
     * adopts the queue set.
     * doc - An optional description of how this queue set is used in the application.
     */
    QSID_t register_queue_set(const std::string& name, EQSetType type, TID_t creator,
                              const std::string& doc = "");

    /**
     * After a queue set is created but before it is used the queue set can be adopted by another task.
     * This can only happen once during the lifetime of the application
     *
     * throws NoSuchQueueSetError if qsetID is not known
     * throws NoSuchTaskError if ownerTask is not known
     * throws NotAllowed  if another task has already taken ownership (other than the creator)
     */
    void adopt_queue_set(QSID_t qsetID, TID_t ownerTask);

    /**
     * Add a queue to a source queue set with specified priority.
     *
     * qsetID - the queue set to modify
     * qid - the queue to add
     * priority - the priority of the queue within the queue set. Larger means more priority.
     *
     * throws NoSuchQueueSetError if qsetID is not known
     * throws NoSuchQueueError of qid does not exist
     * throws NotAllowed if qid is already a member of another queue set
     * throws NotAllowed if queue set is a publisher
     */
    void add_queue_as_source(QSID_t qsetID, QID_t qid, uint8_t priority);

    /**
     * Add a queue to a publisher queue set
     *
     * qsetID - the queue set to modify
     * qid - the queue to add
     *
     * throws NoSuchQueueSetError if qsetID is not known
     * throws NoSuchQueueError of qid does not exist
     * throws NotAllowed if qid is already a member of another queue set
     * throws NotAllowed if queue set is a source
     */
    void add_queue_as_dest(QSID_t qsetID, QID_t qid);

    void add_field_to_msg(EMsgType msgType, EFieldType fieldType);
    void register_field(EFieldType fieldType, std::string name, std::size_t size, EFieldBase base,
                        std::string doc);

  private:
    friend class Session;
    friend class Task;
    explicit Registry(std::shared_ptr<detail::SessionState> state,
                      std::shared_ptr<detail::TaskState> actor = {});
    std::shared_ptr<detail::SessionState> _state;
    std::shared_ptr<detail::TaskState> _actor;
  };
} // namespace angaria

#include "angaria/detail/registry.h"
