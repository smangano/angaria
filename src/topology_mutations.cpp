#include "topology_mutations.h"

#include "angaria/detail/checked_arithmetic.h"
#include "registry_access.h"
#include "topology_append.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace angaria::detail
{
  namespace
  {
    template <typename Id, typename Loaded>
    const Loaded& require_entity(const std::vector<Loaded>& entries, Id id, const char* path)
    {
      return require_registered_entity(entries, id, path, "Entity identity must be nonzero",
                                         "Entity of the requested kind is not registered");
    }

    void require_owner(TaskId owner, const TopologySnapshot& topology, const char* path)
    {
      if (owner == TaskId::Invalid)
      {
        runtime_fail(ErrorCode::InvalidArgument, path, "Adoption requires a nonzero registered Task");
      }
      check_task(owner, topology, path);
    }

    void require_draft(EntityState state, const char* path)
    {
      if (state != EntityState::Draft)
      {
        runtime_fail(ErrorCode::InvalidState, path, "Active ownership and Queue assignments are fixed");
      }
    }

    struct QueueRevision
    {
      const LoadedQueue* _current{};
      TaskId _producer{};
      SourceQueueSetId _source{};
      std::uint8_t _priority{};
      EntityState _state{};
    };

    struct SourceRevision
    {
      const LoadedSource* _current{};
      TaskId _owner{};
      EntityState _state{};
      QueueId _removed{};
      QueueId _added{};
    };

    struct PublishingRevision
    {
      const LoadedPublishing* _current{};
      TaskId _owner{};
      EntityState _state{};
      std::uint64_t _topologyRevision{};
      std::span<const QueueId> _destinations;
    };

    QueueRevision queue_revision(const LoadedQueue& queue)
    {
      const auto& definition = queue._definition;
      return QueueRevision{&queue, definition._descriptor._producer, definition._descriptor._sourceSet,
                           definition._descriptor._priority, definition._state};
    }

    SourceRevision source_revision(const LoadedSource& source)
    {
      return SourceRevision{&source, source._definition._descriptor._owner, source._definition._state, {}, {}};
    }

    PublishingRevision publishing_revision(const LoadedPublishing& publishing)
    {
      const auto& definition = publishing._definition;
      return PublishingRevision{&publishing, definition._descriptor._owner, definition._state,
                                 definition._topologyRevision, definition._descriptor._destinations};
    }

    std::size_t member_count(const SourceRevision& source)
    {
      return source._current->_definition._members.size() - (source._removed != QueueId::Invalid ? 1 : 0) +
             (source._added != QueueId::Invalid ? 1 : 0);
    }

    std::uint64_t definition_bytes(std::uint64_t fixed, const std::string& name, const std::string& doc,
                                     std::uint64_t count = 0)
    {
      return add_bytes(add_bytes(add_bytes(fixed, multiply_bytes(count, 2)), name.size()), doc.size());
    }

    // Revision descriptions borrow the validated snapshot until complete space
    // preflight succeeds. Descriptor/list copies and ring allocation follow it.
    OperationResult commit_revisions(RuntimeAccess& access, const std::shared_ptr<TaskState>& actor,
                                       const TopologySnapshot& topology,
                                       const std::optional<QueueRevision>& queue,
                                       std::span<const SourceRevision> sources,
                                       const std::optional<PublishingRevision>& publishing,
                                       std::optional<std::uint64_t> ringCapacity = {})
    {
      std::vector<std::uint64_t> bodies;
      std::uint64_t commandBytes{};
      if (queue)
      {
        const auto& descriptor = queue->_current->_definition._descriptor;
        bodies.push_back(definition_bytes(80, descriptor._name, descriptor._doc));
        commandBytes = add_bytes(commandBytes, 48);
      }
      for (const auto& source : sources)
      {
        const auto& old = source._current->_definition;
        const auto count = member_count(source);
        bodies.push_back(definition_bytes(96, old._descriptor._name, old._descriptor._doc, count));
        commandBytes = add_bytes(commandBytes,
            add_bytes(48, multiply_bytes(std::max(count, old._members.size()), 32)));
      }
      if (publishing)
      {
        const auto& old = publishing->_current->_definition;
        bodies.push_back(definition_bytes(96, old._descriptor._name, old._descriptor._doc, publishing->_destinations.size()));
        commandBytes = add_bytes(commandBytes, add_bytes(80,
            multiply_bytes(std::max(publishing->_destinations.size(), old._descriptor._destinations.size()), 32)));
      }
      std::uint64_t ringBytes{};
      if (ringCapacity)
      {
        ringBytes = multiply_bytes(*ringCapacity, mapped::ring_slot_bytes);
        if (!fits_size_t(ringBytes))
        {
          runtime_fail(ErrorCode::LogFull, "queue.ring_capacity", "Ring slots exceed the host's addressable allocation");
        }
        bodies.push_back(ringBytes);
        commandBytes = add_bytes(commandBytes, 32);
      }
      auto prepared = prepare_append(access, actor, bodies, commandBytes);
      std::size_t recordIndex{};
      const auto revision = prepared._plan._newRevision;
      if (queue)
      {
        auto definition = queue->_current->_definition;
        definition._descriptor._producer = queue->_producer;
        definition._descriptor._sourceSet = queue->_source;
        definition._descriptor._priority = queue->_priority;
        definition._state = queue->_state;
        definition._registration._revision = revision;
        mapped::JournalRecord record;
        record._kind = 3;
        record._previous = queue->_current->_reference;
        const auto reference = prepared._references[recordIndex++];
        record._immutable = runtime_value(mapped::encode_queue_definition(definition, reference));
        prepared._plan._records.push_back(std::move(record));
        add_index(prepared, queue->_current->_indexOffset, static_cast<std::uint16_t>(definition._id), reference, definition._state);
      }
      std::map<QueueId, std::uint8_t> priorities;
      if (!sources.empty())
      {
        for (const auto& member : topology._queues)
        {
          priorities.emplace(member._definition._id, member._definition._descriptor._priority);
        }
        if (queue)
        {
          priorities[queue->_current->_definition._id] = queue->_priority;
        }
      }
      for (const auto& source : sources)
      {
        auto definition = source._current->_definition;
        definition._descriptor._owner = source._owner;
        definition._state = source._state;
        definition._registration._revision = revision;
        if (source._removed != QueueId::Invalid)
        {
          std::erase(definition._members, source._removed);
        }
        if (source._added != QueueId::Invalid)
        {
          definition._members.push_back(source._added);
        }
        mapped::JournalRecord record;
        record._kind = 4;
        record._previous = source._current->_reference;
        const auto reference = prepared._references[recordIndex++];
        record._immutable = runtime_value(mapped::encode_source_definition(definition, reference));
        prepared._plan._records.push_back(std::move(record));
        add_index(prepared, source._current->_indexOffset, static_cast<std::uint16_t>(definition._id), reference, definition._state);
        const auto count = std::max(definition._members.size(), source._current->_definition._members.size());
        for (std::size_t member = 0; member < count; ++member)
        {
          const auto id = member < definition._members.size() ? definition._members[member] : QueueId::Invalid;
          add_membership(prepared, access, source._current->_slot, member,
              id, id == QueueId::Invalid ? 0 : priorities.at(id), id == QueueId::Invalid ? 0 : revision);
        }
      }
      if (publishing)
      {
        auto definition = publishing->_current->_definition;
        definition._descriptor._owner = publishing->_owner;
        definition._descriptor._destinations.assign(publishing->_destinations.begin(), publishing->_destinations.end());
        definition._state = publishing->_state;
        definition._topologyRevision = publishing->_topologyRevision;
        definition._registration._revision = revision;
        mapped::JournalRecord record;
        record._kind = 5;
        record._previous = publishing->_current->_reference;
        const auto reference = prepared._references[recordIndex++];
        record._immutable = runtime_value(mapped::encode_publishing_definition(definition, reference));
        prepared._plan._records.push_back(std::move(record));
        add_index(prepared, publishing->_current->_indexOffset, static_cast<std::uint16_t>(definition._id), reference, definition._state);
        const std::array<std::uint64_t, 2> snapshot{definition._topologyRevision, reference};
        prepared._plan._commands.push_back(runtime_write_words(publishing->_current->_stateOffset, snapshot));
        const auto count = std::max(definition._descriptor._destinations.size(),
                                    publishing->_current->_definition._descriptor._destinations.size());
        const auto group = access._session->_controller->configuration()._limits._sourceSets + publishing->_current->_slot;
        for (std::size_t member = 0; member < count; ++member)
        {
          const auto id = member < definition._descriptor._destinations.size() ? definition._descriptor._destinations[member] : QueueId::Invalid;
          add_membership(prepared, access, group, member, id, 0, id == QueueId::Invalid ? 0 : definition._topologyRevision);
        }
      }
      if (ringCapacity)
      {
        mapped::JournalRecord ring;
        ring._kind = 8;
        ring._mutable.resize(static_cast<std::size_t>(ringBytes));
        prepared._plan._records.push_back(std::move(ring));
        const std::array<std::uint64_t, 2> geometry{prepared._references[recordIndex], *ringCapacity};
        prepared._plan._commands.push_back(runtime_write_words(queue->_current->_stateOffset + 16, geometry));
      }
      runtime_value(access.execute(prepared._plan));
      return std::monostate{};
    }

    OperationResult adopt_queue(RuntimeAccess& access, const std::shared_ptr<TaskState>& actor,
                                  const TopologySnapshot& topology, const TopologyChange& change)
    {
      const auto& queue = require_entity(topology._queues, static_cast<QueueId>(change._id), "queue.id");
      require_owner(change._owner, topology, "queue.producer");
      if (queue._definition._descriptor._producer == change._owner)
      {
        return std::monostate{};
      }
      require_draft(queue._definition._state, "queue.state");
      for (const auto& publishing : topology._publishing)
      {
        const auto& descriptor = publishing._definition._descriptor;
        if (descriptor._owner != change._owner &&
            std::find(descriptor._destinations.begin(), descriptor._destinations.end(), queue._definition._id) != descriptor._destinations.end())
        {
          runtime_fail(ErrorCode::WrongOwner, "queue.producer", "A Publishing set still references the Queue under another producer");
        }
      }
      auto updated = queue_revision(queue);
      updated._producer = change._owner;
      return commit_revisions(access, actor, topology, updated, {}, {});
    }

    OperationResult adopt_source(RuntimeAccess& access, const std::shared_ptr<TaskState>& actor,
                                   const TopologySnapshot& topology, const TopologyChange& change)
    {
      const auto& source = require_entity(topology._sources, static_cast<SourceQueueSetId>(change._id), "source_set.id");
      require_owner(change._owner, topology, "source_set.owner");
      if (source._definition._descriptor._owner == change._owner)
      {
        return std::monostate{};
      }
      require_draft(source._definition._state, "source_set.state");
      auto updated = source_revision(source);
      updated._owner = change._owner;
      const std::array changes{updated};
      return commit_revisions(access, actor, topology, {}, changes, {});
    }

    OperationResult adopt_publishing(RuntimeAccess& access, const std::shared_ptr<TaskState>& actor,
                                       const TopologySnapshot& topology, const TopologyChange& change)
    {
      const auto& publishing = require_entity(topology._publishing, static_cast<PublishingQueueSetId>(change._id), "publishing_set.id");
      require_owner(change._owner, topology, "publishing_set.owner");
      if (publishing._definition._descriptor._owner == change._owner)
      {
        return std::monostate{};
      }
      require_draft(publishing._definition._state, "publishing_set.state");
      std::map<QueueId, TaskId> producers;
      for (const auto& queue : topology._queues)
      {
        producers.emplace(queue._definition._id, queue._definition._descriptor._producer);
      }
      for (const auto id : publishing._definition._descriptor._destinations)
      {
        if (producers.at(id) != change._owner)
        {
          runtime_fail(ErrorCode::WrongOwner, "publishing_set.owner", "Publishing destinations belong to another producer");
        }
      }
      auto updated = publishing_revision(publishing);
      updated._owner = change._owner;
      return commit_revisions(access, actor, topology, {}, {}, updated);
    }

    OperationResult assign_source(RuntimeAccess& access, const std::shared_ptr<TaskState>& actor,
                                    const TopologySnapshot& topology, const TopologyChange& change)
    {
      const auto& queue = require_entity(topology._queues, static_cast<QueueId>(change._id), "queue.id");
      const auto oldSource = queue._definition._descriptor._sourceSet;
      if (oldSource == change._source && queue._definition._descriptor._priority == change._priority)
      {
        return std::monostate{};
      }
      require_draft(queue._definition._state, "queue.state");
      const LoadedSource* next{};
      if (change._source != SourceQueueSetId::Invalid)
      {
        next = &require_entity(topology._sources, change._source, "queue.source_set");
        if (oldSource != change._source && next->_definition._members.size() >=
            access._session->_controller->configuration()._limits._membershipsPerSet)
        {
          runtime_fail(ErrorCode::CapacityExceeded, "limits.memberships_per_set", "Source has no unused membership capacity");
        }
        if (next->_definition._state == EntityState::Active && queue._definition._descriptor._producer == TaskId::Invalid)
        {
          runtime_fail(ErrorCode::InvalidState, "queue.producer", "An active Source requires fully owned member Queues");
        }
      }
      std::vector<SourceRevision> sources;
      if (oldSource == change._source)
      {
        if (next)
        {
          sources.push_back(source_revision(*next));
        }
      }
      else
      {
        if (oldSource != SourceQueueSetId::Invalid)
        {
          const auto& old = require_entity(topology._sources, oldSource, "queue.source_set");
          if (old._definition._state == EntityState::Active && old._definition._members.size() == 1)
          {
            runtime_fail(ErrorCode::InvalidState, "source_set.members", "An active Source cannot become empty");
          }
          auto revised = source_revision(old);
          revised._removed = queue._definition._id;
          sources.push_back(revised);
        }
        if (next)
        {
          auto revised = source_revision(*next);
          revised._added = queue._definition._id;
          sources.push_back(revised);
        }
      }
      auto updated = queue_revision(queue);
      updated._source = change._source;
      updated._priority = change._priority;
      return commit_revisions(access, actor, topology, updated, sources, {});
    }

    OperationResult set_destinations(RuntimeAccess& access, const std::shared_ptr<TaskState>& actor,
                                       const TopologySnapshot& topology, const TopologyChange& change)
    {
      const auto& publishing = require_entity(topology._publishing, static_cast<PublishingQueueSetId>(change._id), "publishing_set.id");
      const auto& old = publishing._definition;
      if (old._state == EntityState::Active && old._descriptor._owner != actor->_claim._task)
      {
        runtime_fail(ErrorCode::WrongOwner, "publishing_set.owner", "Only the active Publishing owner can replace destinations");
      }
      if (old._state == EntityState::Active && change._destinations.empty())
      {
        runtime_fail(ErrorCode::InvalidArgument, "publishing_set.destinations", "An active Publishing set requires destinations");
      }
      if (!change._destinations.empty() && old._descriptor._owner == TaskId::Invalid)
      {
        runtime_fail(ErrorCode::InvalidArgument, "publishing_set.owner", "Publishing destinations require a producer owner");
      }
      if (change._destinations.size() > access._session->_controller->configuration()._limits._membershipsPerSet)
      {
        runtime_fail(ErrorCode::CapacityExceeded, "limits.memberships_per_set", "Publishing destination count exceeds capacity");
      }
      std::set<QueueId> unique;
      std::map<QueueId, const LoadedQueue*> queues;
      for (const auto& queue : topology._queues)
      {
        queues.emplace(queue._definition._id, &queue);
      }
      for (const auto destination : change._destinations)
      {
        if (destination == QueueId::Invalid || !unique.insert(destination).second)
        {
          runtime_fail(ErrorCode::InvalidArgument, "publishing_set.destinations", "Destinations must be unique nonzero Queue identities");
        }
        const auto found = queues.find(destination);
        if (found == queues.end())
        {
          runtime_fail(ErrorCode::NotFound, "publishing_set.destinations", "Destination Queue is not registered");
        }
        if (found->second->_definition._descriptor._producer != old._descriptor._owner)
        {
          runtime_fail(ErrorCode::WrongOwner, "publishing_set.destinations", "Destination Queue belongs to another producer");
        }
        if (old._state == EntityState::Active && found->second->_definition._state != EntityState::Active)
        {
          runtime_fail(ErrorCode::NotActive, "publishing_set.destinations", "An active Publishing set requires active destination Queues");
        }
      }
      if (std::equal(change._destinations.begin(), change._destinations.end(),
                      old._descriptor._destinations.begin(), old._descriptor._destinations.end()))
      {
        return std::monostate{};
      }
      if (old._topologyRevision == std::numeric_limits<std::uint64_t>::max())
      {
        runtime_fail(ErrorCode::CapacityExceeded, "publishing_set.topology_revision", "Publishing topology revision is exhausted");
      }
      auto updated = publishing_revision(publishing);
      updated._destinations = change._destinations;
      updated._topologyRevision = old._topologyRevision + 1;
      return commit_revisions(access, actor, topology, {}, {}, updated);
    }

    OperationResult activate_source(RuntimeAccess& access, const std::shared_ptr<TaskState>& actor,
                                      const TopologySnapshot& topology, const TopologyChange& change)
    {
      const auto& source = require_entity(topology._sources, static_cast<SourceQueueSetId>(change._id), "source_set.id");
      if (source._definition._state == EntityState::Active)
      {
        return std::monostate{};
      }
      if (source._definition._descriptor._owner == TaskId::Invalid || source._definition._members.empty())
      {
        runtime_fail(ErrorCode::InvalidState, "source_set", "Source activation requires an owner and nonempty membership");
      }
      std::map<QueueId, const LoadedQueue*> queues;
      for (const auto& queue : topology._queues)
      {
        queues.emplace(queue._definition._id, &queue);
      }
      for (const auto member : source._definition._members)
      {
        const auto& queue = queues.at(member)->_definition._descriptor;
        if (queue._producer == TaskId::Invalid || queue._sourceSet != source._definition._id)
        {
          runtime_fail(ErrorCode::InvalidState, "source_set.members", "Source activation requires fully configured member Queues");
        }
      }
      auto updated = source_revision(source);
      updated._state = EntityState::Active;
      const std::array changes{updated};
      return commit_revisions(access, actor, topology, {}, changes, {});
    }

    OperationResult activate_queue(RuntimeAccess& access, const std::shared_ptr<TaskState>& actor,
                                     const TopologySnapshot& topology, const TopologyChange& change)
    {
      const auto& queue = require_entity(topology._queues, static_cast<QueueId>(change._id), "queue.id");
      if (queue._definition._state == EntityState::Active)
      {
        return std::monostate{};
      }
      const auto& descriptor = queue._definition._descriptor;
      if (descriptor._producer == TaskId::Invalid || descriptor._sourceSet == SourceQueueSetId::Invalid)
      {
        runtime_fail(ErrorCode::InvalidState, "queue", "Queue activation requires complete producer and Source ownership");
      }
      const auto& source = require_entity(topology._sources, descriptor._sourceSet, "queue.source_set");
      if (source._definition._state != EntityState::Active)
      {
        runtime_fail(ErrorCode::NotActive, "queue.source_set", "Queue activation requires an active Source");
      }
      auto updated = queue_revision(queue);
      updated._state = EntityState::Active;
      return commit_revisions(access, actor, topology, updated, {}, {}, *descriptor._ringCapacity);
    }

    OperationResult activate_publishing(RuntimeAccess& access, const std::shared_ptr<TaskState>& actor,
                                          const TopologySnapshot& topology, const TopologyChange& change)
    {
      const auto& publishing = require_entity(topology._publishing, static_cast<PublishingQueueSetId>(change._id), "publishing_set.id");
      const auto& old = publishing._definition;
      if (old._state == EntityState::Active)
      {
        return std::monostate{};
      }
      if (old._descriptor._owner == TaskId::Invalid || old._descriptor._destinations.empty())
      {
        runtime_fail(ErrorCode::InvalidState, "publishing_set", "Publishing activation requires a producer owner and destinations");
      }
      std::map<QueueId, const LoadedQueue*> queues;
      for (const auto& queue : topology._queues)
      {
        queues.emplace(queue._definition._id, &queue);
      }
      for (const auto destination : old._descriptor._destinations)
      {
        if (queues.at(destination)->_definition._state != EntityState::Active)
        {
          runtime_fail(ErrorCode::NotActive, "publishing_set.destinations", "Publishing activation requires active destination Queues");
        }
      }
      auto updated = publishing_revision(publishing);
      updated._state = EntityState::Active;
      return commit_revisions(access, actor, topology, {}, {}, updated);
    }
  } // namespace

  OperationResult runtime_update_topology(std::shared_ptr<SessionState> session,
                                          std::shared_ptr<TaskState> actor,
                                          const TopologyChange& change,
                                          mapped::JournalFaultInjector* injector)
  {
    try
    {
      check_registry_actor(session, actor, "Registration requires an actor-bound Registry");
      RuntimeAccess access(std::move(session), true, injector);
      runtime_check_task(access, actor);
      const auto topology = runtime_topology(access);
      switch (change._operation)
      {
      case TopologyMutation::AdoptQueue:
        return adopt_queue(access, actor, topology, change);
      case TopologyMutation::AdoptSource:
        return adopt_source(access, actor, topology, change);
      case TopologyMutation::AdoptPublishing:
        return adopt_publishing(access, actor, topology, change);
      case TopologyMutation::AssignSource:
        return assign_source(access, actor, topology, change);
      case TopologyMutation::SetDestinations:
        return set_destinations(access, actor, topology, change);
      case TopologyMutation::ActivateQueue:
        return activate_queue(access, actor, topology, change);
      case TopologyMutation::ActivateSource:
        return activate_source(access, actor, topology, change);
      case TopologyMutation::ActivatePublishing:
        return activate_publishing(access, actor, topology, change);
      }
      runtime_fail(ErrorCode::InvalidArgument, "topology.operation", "Unknown topology mutation");
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }
} // namespace angaria::detail

namespace angaria
{
  OperationResult Registry::adopt_queue(QueueId id, TaskId producer)
  {
    detail::TopologyChange change;
    change._operation = detail::TopologyMutation::AdoptQueue;
    change._id = static_cast<std::uint16_t>(id);
    change._owner = producer;
    return detail::runtime_update_topology(_state, _actor, change);
  }

  OperationResult Registry::adopt_source_set(SourceQueueSetId id, TaskId owner)
  {
    detail::TopologyChange change;
    change._operation = detail::TopologyMutation::AdoptSource;
    change._id = static_cast<std::uint16_t>(id);
    change._owner = owner;
    return detail::runtime_update_topology(_state, _actor, change);
  }

  OperationResult Registry::adopt_publishing_set(PublishingQueueSetId id, TaskId owner)
  {
    detail::TopologyChange change;
    change._operation = detail::TopologyMutation::AdoptPublishing;
    change._id = static_cast<std::uint16_t>(id);
    change._owner = owner;
    return detail::runtime_update_topology(_state, _actor, change);
  }

  OperationResult Registry::assign_source(QueueId id, SourceQueueSetId source, std::uint8_t priority)
  {
    detail::TopologyChange change;
    change._operation = detail::TopologyMutation::AssignSource;
    change._id = static_cast<std::uint16_t>(id);
    change._source = source;
    change._priority = priority;
    return detail::runtime_update_topology(_state, _actor, change);
  }

  OperationResult Registry::set_destinations(PublishingQueueSetId id, std::span<const QueueId> destinations)
  {
    detail::TopologyChange change;
    change._operation = detail::TopologyMutation::SetDestinations;
    change._id = static_cast<std::uint16_t>(id);
    change._destinations = destinations;
    return detail::runtime_update_topology(_state, _actor, change);
  }

  OperationResult Registry::activate_source_set(SourceQueueSetId id)
  {
    detail::TopologyChange change;
    change._operation = detail::TopologyMutation::ActivateSource;
    change._id = static_cast<std::uint16_t>(id);
    return detail::runtime_update_topology(_state, _actor, change);
  }

  OperationResult Registry::activate_queue(QueueId id)
  {
    detail::TopologyChange change;
    change._operation = detail::TopologyMutation::ActivateQueue;
    change._id = static_cast<std::uint16_t>(id);
    return detail::runtime_update_topology(_state, _actor, change);
  }

  OperationResult Registry::activate_publishing_set(PublishingQueueSetId id)
  {
    detail::TopologyChange change;
    change._operation = detail::TopologyMutation::ActivatePublishing;
    change._id = static_cast<std::uint16_t>(id);
    return detail::runtime_update_topology(_state, _actor, change);
  }
} // namespace angaria
