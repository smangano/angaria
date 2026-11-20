#pragma once

#include "angaria/identities.h"
#include "angaria/types.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace angaria
{
  enum class TaskKind
  {
    Process,
    Thread
  };

  enum class EntityState
  {
    Draft,
    Active
  };

  enum class StorageMapping
  {
    TwoPlane,
    Segmented
  };

  enum class SelectionPolicy
  {
    Strict,
    Fair,
    WeightedRandom
  };

  enum class WaitPolicy
  {
    Spin,
    EventFd
  };

  enum class DeliveryPolicy
  {
    RecoverFromLog,
    AllowLoss
  };

  enum class AcceptancePolicy
  {
    AllDestinations,
    Partial
  };

  enum class PayloadFormat
  {
    Binary,
    Json,
    Xml,
    Csv,
    Text
  };

  using UtcTimestamp = std::chrono::sys_time<std::chrono::nanoseconds>;

  // These descriptors own process-local values. Mapped representations have a separate layout.
  struct CapacityLimits
  {
    std::uint32_t _tasks{};
    std::uint32_t _queues{};
    std::uint32_t _sourceSets{};
    std::uint32_t _publishingSets{};
    std::uint32_t _membershipsPerSet{};
    std::uint32_t _fields{};
    std::uint32_t _messageSchemas{};
    std::uint32_t _symbols{};
    std::uint64_t _stringPoolBytes{};
  };

  struct PolicyDefaults
  {
    std::uint64_t _ringCapacity{1024};
    DeliveryPolicy _delivery{DeliveryPolicy::RecoverFromLog};
    SelectionPolicy _selection{SelectionPolicy::Strict};
    WaitPolicy _wait{WaitPolicy::Spin};
    AcceptancePolicy _acceptance{AcceptancePolicy::AllDestinations};
  };

  struct SessionOptions
  {
    std::string _name{};
    std::string _applicationVersion{};
    std::filesystem::path _directory{};
    std::uint64_t _maxLogBytes{};
    StorageMapping _storage{StorageMapping::TwoPlane};
    std::optional<std::uint64_t> _segmentBytes{};
    CapacityLimits _limits{};
    PolicyDefaults _defaults{};
  };

  struct TaskDescriptor
  {
    std::string _name{};
    TaskKind _kind{TaskKind::Process};
    std::optional<TaskId> _process{};
    std::string _doc{};
  };

  // Empty policy overrides inherit the recorded session defaults before activation.
  struct SourceQueueSetDescriptor
  {
    std::string _name{};
    TaskId _owner{};
    std::optional<SelectionPolicy> _selection{};
    std::optional<WaitPolicy> _wait{};
    std::optional<std::uint64_t> _randomSeed{};
    std::string _doc{};
  };

  struct QueueDescriptor
  {
    std::string _name{};
    TaskId _producer{};
    SourceQueueSetId _sourceSet{};
    std::uint8_t _priority{1};
    std::optional<std::uint64_t> _ringCapacity{};
    std::optional<DeliveryPolicy> _delivery{};
    std::string _doc{};
  };

  struct PublishingQueueSetDescriptor
  {
    std::string _name{};
    TaskId _owner{};
    std::vector<QueueId> _destinations{};
    std::optional<AcceptancePolicy> _acceptance{};
    std::string _doc{};
  };

  struct FieldDescriptor
  {
    FieldId _id{};
    std::string _name{};
    EFieldBase _base{EFieldBase::BASE_NONE};
    std::uint32_t _size{};
    std::string _doc{};
  };

  struct MessageDescriptor
  {
    SchemaId _schema{};
    std::string _name{};
    std::vector<FieldId> _fields{};
    std::string _doc{};
  };
  struct RegistrationMetadata
  {
    TaskId _createdBy{};
    UtcTimestamp _createdAt{};
    std::uint64_t _revision{};
  };

  struct TaskRuntimeInfo
  {
    TaskId _task{};
    std::uint64_t _processId{};
    std::uint64_t _threadId{};
    std::uint64_t _incarnation{};
    std::uint64_t _restartCount{};
    std::optional<UtcTimestamp> _lastAttachedAt{};
    bool _attached{};
  };

  struct SessionMetadata
  {
    SessionId _session{};
    TaskId _createdBy{};
    UtcTimestamp _createdAt{};
    std::string _angariaVersion{};
    std::string _applicationVersion{};
    std::string _creatorUser{};
  };

} // namespace angaria
