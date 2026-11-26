#include "runtime_state.h"

#include "angaria/detail/record_layout.h"
#include "registry_access.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace angaria::detail
{
  namespace
  {
    bool same_descriptor(const TaskDescriptor& left, const TaskDescriptor& right)
    {
      return left._name == right._name && left._kind == right._kind &&
             left._process == right._process && left._doc == right._doc;
    }

    std::uint64_t registration_bytes(std::uint64_t left, std::uint64_t right)
    {
      const auto result = mapped::checked_add(left, right);
      if (!result || *result > mapped::max_log_bytes)
      {
        runtime_fail(ErrorCode::LogFull, "task.definition", "Task registration exceeds format byte bounds");
      }
      return *result;
    }

    std::uint64_t record_bytes(std::uint64_t bodyBytes)
    {
      const auto total = mapped::record_storage_bytes(bodyBytes);
      if (!total)
      {
        runtime_fail(ErrorCode::LogFull, "task.definition", "Task registration allocation exceeds format byte bounds");
      }
      return *total;
    }

    void check_parent(const TaskDescriptor& descriptor, std::span<const LoadedTask> tasks)
    {
      if (descriptor._kind != TaskKind::Thread)
      {
        return;
      }
      const auto parent = std::find_if(tasks.begin(), tasks.end(), [&](const auto& task)
      {
        return task._definition._id == *descriptor._process;
      });
      if (parent == tasks.end())
      {
        runtime_fail(ErrorCode::NotFound, "task.process", "Thread Task parent is not registered");
      }
      if (parent->_definition._descriptor._kind != TaskKind::Process)
      {
        runtime_fail(ErrorCode::InvalidArgument, "task.process", "Thread Task parent must be a Process Task");
      }
    }

    struct TaskAllocation
    {
      TaskId _id{};
      std::uint64_t _indexOffset{};
    };

    TaskAllocation allocate_task(RuntimeAccess& access, std::span<const LoadedTask> tasks)
    {
      const auto capacity = access._session->_controller->configuration()._limits._tasks;
      if (tasks.size() >= capacity)
      {
        runtime_fail(ErrorCode::CapacityExceeded, "limits.tasks", "Session has no unused Task capacity");
      }
      std::vector<bool> usedIds(std::size_t{capacity} + 1);
      std::vector<bool> usedSlots(capacity);
      const auto states = access.section_offset(mapped::SectionKind::TaskState);
      for (const auto& task : tasks)
      {
        usedIds[static_cast<std::uint16_t>(task._definition._id)] = true;
        usedSlots[static_cast<std::size_t>((task._stateOffset - states) / mapped::task_state_bytes)] = true;
      }
      std::uint32_t id = 1;
      while (id <= capacity && usedIds[id])
      {
        ++id;
      }
      std::uint32_t slot{};
      while (slot < capacity && usedSlots[slot])
      {
        ++slot;
      }
      if (id > capacity || slot == capacity)
      {
        runtime_fail(ErrorCode::CapacityExceeded, "limits.tasks", "Session has no unused Task identity or slot");
      }
      // Allocation slots and logical IDs are independent format concepts. A
      // sparse existing table must not turn an ID into a TaskState array index.
      return TaskAllocation{static_cast<TaskId>(id),
          access.section_offset(mapped::SectionKind::TaskIndex) + slot * mapped::index_entry_bytes};
    }
  } // namespace

  Result<TaskId> runtime_register_task(std::shared_ptr<SessionState> session,
                                      std::shared_ptr<TaskState> actor,
                                      const TaskDescriptor& descriptor,
                                      mapped::JournalFaultInjector* injector,
                                      TaskRegistrationObserver* observer)
  {
    try
    {
      check_registry_actor(session, actor, "Task registration requires an actor-bound Registry");
      RuntimeAccess access(std::move(session), true, injector);
      runtime_check_task(access, actor);
      runtime_value(mapped::validate_task_descriptor(descriptor));
      const auto tasks = runtime_tasks(access);
      const auto existing = std::find_if(tasks.begin(), tasks.end(), [&](const auto& task)
      {
        return task._definition._descriptor._name == descriptor._name;
      });
      if (existing != tasks.end())
      {
        if (!same_descriptor(existing->_definition._descriptor, descriptor))
        {
          runtime_fail(ErrorCode::Conflict, "task.name", "Task name already has a different registered descriptor");
        }
        // Retry changes neither registration attribution nor counters/history,
        // even when another registration or an exhausted log now prevents growth.
        return existing->_definition._id;
      }
      check_parent(descriptor, tasks);
      const auto allocation = allocate_task(access, tasks);
      if (access._snapshot._revision == std::numeric_limits<std::uint64_t>::max())
      {
        runtime_fail(ErrorCode::CapacityExceeded, "registry.revision", "Registry revision is exhausted");
      }
      if (access._snapshot._generation == std::numeric_limits<std::uint64_t>::max())
      {
        runtime_fail(ErrorCode::CapacityExceeded, "journal.generation", "Operation generation is exhausted");
      }
      const auto revision = access._snapshot._revision + 1;
      const auto bodyBytes = registration_bytes(registration_bytes(72, descriptor._name.size()), descriptor._doc.size());
      const auto manifestOffset = registration_bytes(access._snapshot._tail, record_bytes(bodyBytes));
      const std::array<std::uint64_t, 4> row{
          static_cast<std::uint16_t>(allocation._id), access._snapshot._tail, revision, 0};
      auto index = runtime_write_words(allocation._indexOffset, row);
      mapped::Manifest manifest;
      manifest._actor = actor->_claim._task;
      manifest._actorIncarnation = actor->_claim._incarnation;
      manifest._baseRevision = access._snapshot._revision;
      manifest._newRevision = revision;
      manifest._records = {access._snapshot._tail};
      manifest._commands.push_back(std::move(index));
      const auto commands = runtime_value(mapped::encode_commands(manifest._commands));
      // The manifest has a 64-byte fixed body and one eight-byte record
      // reference before its encoded commands. Check all bytes before encoding
      // at a near-limit offset or copying the potentially large descriptor.
      const auto manifestBodyBytes = registration_bytes(64 + 8, commands.size());
      const auto newTail = registration_bytes(manifestOffset, record_bytes(manifestBodyBytes));
      if (newTail > access._session->_controller->configuration()._maxLogBytes)
      {
        runtime_fail(ErrorCode::LogFull, "session.max_log_bytes", "Complete Task definition and manifest exceed the log byte ceiling");
      }
      runtime_value(mapped::encode_manifest_body(manifest, manifestOffset));

      mapped::TaskDefinition definition;
      definition._id = allocation._id;
      definition._descriptor = descriptor;
      definition._registration = RegistrationMetadata{actor->_claim._task,
          UtcTimestamp{std::chrono::nanoseconds{runtime_now_ns()}}, revision};
      mapped::JournalRecord record;
      record._kind = 2;
      record._immutable = runtime_value(mapped::encode_task_definition(definition, access._snapshot._tail));
      mapped::TransactionPlan plan;
      plan._base = access._snapshot;
      plan._kind = mapped::JournalKind::Append;
      plan._actor = manifest._actor;
      plan._actorIncarnation = manifest._actorIncarnation;
      plan._newRevision = revision;
      plan._records.push_back(std::move(record));
      plan._commands = std::move(manifest._commands);

      // Coordination resources precede acceptance so a committed registration
      // is immediately attachable. An interrupted unaccepted attempt may leave
      // this stable empty file for a retry; it must never be unlinked/replaced.
      runtime_value(access._session->_controller->create_task_claim_file(access._guard, allocation._id));
      if (observer)
      {
        runtime_value(observer->claim_file_ready(allocation._id));
      }
      runtime_value(access._journal.execute(access._guard, plan));
      // Return the already selected scalar ID without allocating after commit.
      return allocation._id;
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }
} // namespace angaria::detail

namespace angaria
{
  Result<TaskId> Registry::register_task(const TaskDescriptor& descriptor)
  {
    return detail::runtime_register_task(_state, _actor, descriptor);
  }
} // namespace angaria
