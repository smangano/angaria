#include "topology_append.h"

#include "registry_access.h"
#include "topology_policy.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace angaria::detail
{
  namespace
  {
    RegistrationMetadata registration(const PreparedAppend& prepared)
    {
      return RegistrationMetadata{prepared._plan._actor,
          UtcTimestamp{std::chrono::nanoseconds{runtime_now_ns()}}, prepared._plan._newRevision};
    }

    template <typename Loaded>
    std::uint64_t allocate_slot(std::span<const Loaded> entries, std::uint64_t capacity, const char* path)
    {
      if (entries.size() >= capacity)
      {
        runtime_fail(ErrorCode::CapacityExceeded, path, "Session has no unused entity capacity");
      }
      std::set<std::uint64_t> occupied;
      for (const auto& entry : entries)
      {
        occupied.insert(entry._slot);
      }
      std::uint64_t slot{};
      while (occupied.contains(slot))
      {
        ++slot;
      }
      return slot;
    }

    std::uint16_t allocate_id(const std::set<std::uint16_t>& occupied)
    {
      for (std::uint32_t id = 1; id <= mapped::max_entity_id; ++id)
      {
        if (!occupied.contains(static_cast<std::uint16_t>(id)))
        {
          return static_cast<std::uint16_t>(id);
        }
      }
      runtime_fail(ErrorCode::CapacityExceeded, "topology.identity", "Entity identity namespace is exhausted");
    }

    std::uint16_t allocate_set_id(const TopologySnapshot& topology)
    {
      std::set<std::uint16_t> ids;
      for (const auto& source : topology._sources)
      {
        ids.insert(static_cast<std::uint16_t>(source._definition._id));
      }
      for (const auto& publishing : topology._publishing)
      {
        ids.insert(static_cast<std::uint16_t>(publishing._definition._id));
      }
      return allocate_id(ids);
    }

    bool same_queue(const QueueDescriptor& left, const QueueDescriptor& right, QueuePolicy effective)
    {
      return left._name == right._name && left._producer == right._producer && left._sourceSet == right._sourceSet &&
             left._priority == right._priority && left._ringCapacity == effective._capacity &&
             left._delivery == effective._delivery && left._doc == right._doc;
    }

    bool same_source(const SourceQueueSetDescriptor& left, const SourceQueueSetDescriptor& right, SourcePolicy effective)
    {
      return left._name == right._name && left._owner == right._owner &&
             left._selection == effective._selection && left._wait == effective._wait &&
             left._randomSeed == right._randomSeed && left._doc == right._doc;
    }

    bool same_publishing(const PublishingQueueSetDescriptor& left, const PublishingQueueSetDescriptor& right,
                           AcceptancePolicy acceptance)
    {
      return left._name == right._name && left._owner == right._owner &&
             left._destinations == right._destinations && left._acceptance == acceptance && left._doc == right._doc;
    }
  } // namespace

  Result<SourceQueueSetId> runtime_register_source_set(std::shared_ptr<SessionState> session,
                                                       std::shared_ptr<TaskState> actor,
                                                       const SourceQueueSetDescriptor& descriptor,
                                                       mapped::JournalFaultInjector* injector)
  {
    try
    {
      check_registry_actor(session, actor, "Registration requires an actor-bound Registry");
      RuntimeAccess access(std::move(session), true, injector);
      runtime_check_task(access, actor);
      runtime_value(mapped::validate_source_set_descriptor(descriptor));
      const auto sessionDefinition = runtime_session_definition(access);
      const auto effective = resolve_source(descriptor, sessionDefinition._options);
      const auto topology = runtime_topology(access);
      for (const auto& source : topology._sources)
      {
        if (source._definition._descriptor._name == descriptor._name)
        {
          if (!same_source(source._definition._descriptor, descriptor, effective))
          {
            runtime_fail(ErrorCode::Conflict, "source_set.name", "Queue Set name already has a different descriptor");
          }
          return source._definition._id;
        }
      }
      for (const auto& publishing : topology._publishing)
      {
        if (publishing._definition._descriptor._name == descriptor._name)
        {
          runtime_fail(ErrorCode::Conflict, "source_set.name", "Queue Set name belongs to a Publishing set");
        }
      }
      check_task(descriptor._owner, topology, "source_set.owner");
      const auto& limits = access._session->_controller->configuration()._limits;
      const auto slot = allocate_slot(std::span{topology._sources}, limits._sourceSets, "limits.source_sets");
      const auto id = static_cast<SourceQueueSetId>(allocate_set_id(topology));
      const std::array<std::uint64_t, 1> bodies{add_bytes(add_bytes(96, descriptor._name.size()), descriptor._doc.size())};
      auto prepared = prepare_append(access, actor, bodies, 48);
      mapped::SourceDefinition definition;
      definition._id = id;
      definition._descriptor = descriptor;
      definition._descriptor._selection = effective._selection;
      definition._descriptor._wait = effective._wait;
      definition._registration = registration(prepared);
      mapped::JournalRecord record;
      record._kind = 4;
      record._immutable = runtime_value(mapped::encode_source_definition(definition, prepared._references[0]));
      prepared._plan._records.push_back(std::move(record));
      add_index(prepared, access.section_offset(mapped::SectionKind::SourceIndex) + slot * mapped::index_entry_bytes,
                static_cast<std::uint16_t>(id), prepared._references[0]);
      runtime_value(access._journal.execute(access._guard, prepared._plan));
      return id;
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  Result<QueueId> runtime_register_queue(std::shared_ptr<SessionState> session,
                                         std::shared_ptr<TaskState> actor,
                                         const QueueDescriptor& descriptor,
                                         mapped::JournalFaultInjector* injector)
  {
    try
    {
      check_registry_actor(session, actor, "Registration requires an actor-bound Registry");
      RuntimeAccess access(std::move(session), true, injector);
      runtime_check_task(access, actor);
      runtime_value(mapped::validate_queue_descriptor(descriptor));
      const auto sessionDefinition = runtime_session_definition(access);
      const auto effective = resolve_queue(descriptor, sessionDefinition._options,
          (access._session->_controller->configuration()._requiredFeatures & mapped::allow_loss_feature) != 0);
      const auto topology = runtime_topology(access);
      for (const auto& queue : topology._queues)
      {
        if (queue._definition._descriptor._name == descriptor._name)
        {
          if (!same_queue(queue._definition._descriptor, descriptor, effective))
          {
            runtime_fail(ErrorCode::Conflict, "queue.name", "Queue name already has a different descriptor");
          }
          return queue._definition._id;
        }
      }
      check_task(descriptor._producer, topology, "queue.producer");
      const auto& limits = access._session->_controller->configuration()._limits;
      const LoadedSource* source{};
      if (descriptor._sourceSet != SourceQueueSetId::Invalid)
      {
        const auto found = std::find_if(topology._sources.begin(), topology._sources.end(), [&](const auto& entry)
        {
          return entry._definition._id == descriptor._sourceSet;
        });
        if (found == topology._sources.end())
        {
          runtime_fail(ErrorCode::NotFound, "queue.source_set", "Source Queue Set is not registered");
        }
        source = &*found;
        if (source->_definition._state == EntityState::Active && descriptor._producer == TaskId::Invalid)
        {
          runtime_fail(ErrorCode::InvalidState, "queue.producer", "An active Source requires fully owned member Queues");
        }
        if (source->_definition._members.size() >= limits._membershipsPerSet)
        {
          runtime_fail(ErrorCode::CapacityExceeded, "limits.memberships_per_set", "Source Queue Set has no unused membership capacity");
        }
      }
      const auto slot = allocate_slot(std::span{topology._queues}, limits._queues, "limits.queues");
      std::set<std::uint16_t> occupied;
      for (const auto& queue : topology._queues)
      {
        occupied.insert(static_cast<std::uint16_t>(queue._definition._id));
      }
      const auto id = static_cast<QueueId>(allocate_id(occupied));
      std::vector<std::uint64_t> bodies{
          add_bytes(add_bytes(80, descriptor._name.size()), descriptor._doc.size())};
      auto commandBytes = std::uint64_t{48};
      if (source)
      {
        const auto& old = source->_definition;
        bodies.push_back(add_bytes(add_bytes(add_bytes(96, multiply_bytes(old._members.size() + 1, 2)),
                                            old._descriptor._name.size()), old._descriptor._doc.size()));
        commandBytes = add_bytes(96, multiply_bytes(old._members.size() + 1, 32));
      }
      auto prepared = prepare_append(access, actor, bodies, commandBytes);
      mapped::QueueDefinition definition;
      definition._id = id;
      definition._descriptor = descriptor;
      definition._descriptor._ringCapacity = effective._capacity;
      definition._descriptor._delivery = effective._delivery;
      definition._registration = registration(prepared);
      mapped::JournalRecord record;
      record._kind = 3;
      record._immutable = runtime_value(mapped::encode_queue_definition(definition, prepared._references[0]));
      prepared._plan._records.push_back(std::move(record));
      add_index(prepared, access.section_offset(mapped::SectionKind::QueueIndex) + slot * mapped::index_entry_bytes,
                static_cast<std::uint16_t>(id), prepared._references[0]);
      if (source)
      {
        auto updated = source->_definition;
        updated._members.push_back(id);
        updated._registration._revision = prepared._plan._newRevision;
        mapped::JournalRecord sourceRecord;
        sourceRecord._kind = 4;
        sourceRecord._previous = source->_reference;
        sourceRecord._immutable = runtime_value(mapped::encode_source_definition(updated, prepared._references[1]));
        prepared._plan._records.push_back(std::move(sourceRecord));
        add_index(prepared, source->_indexOffset, static_cast<std::uint16_t>(updated._id), prepared._references[1], updated._state);
        std::map<QueueId, std::uint8_t> priorities;
        for (const auto& queue : topology._queues)
        {
          priorities.emplace(queue._definition._id, queue._definition._descriptor._priority);
        }
        priorities.emplace(id, descriptor._priority);
        for (std::size_t member = 0; member < updated._members.size(); ++member)
        {
          add_membership(prepared, access, source->_slot, member, updated._members[member],
                          priorities.at(updated._members[member]), prepared._plan._newRevision);
        }
      }
      runtime_value(access._journal.execute(access._guard, prepared._plan));
      return id;
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  Result<PublishingQueueSetId> runtime_register_publishing_set(std::shared_ptr<SessionState> session,
                                                               std::shared_ptr<TaskState> actor,
                                                               const PublishingQueueSetDescriptor& descriptor,
                                                               mapped::JournalFaultInjector* injector)
  {
    try
    {
      check_registry_actor(session, actor, "Registration requires an actor-bound Registry");
      RuntimeAccess access(std::move(session), true, injector);
      runtime_check_task(access, actor);
      runtime_value(mapped::validate_publishing_set_descriptor(descriptor));
      const auto sessionDefinition = runtime_session_definition(access);
      const auto acceptance = resolve_publishing(descriptor, sessionDefinition._options);
      const auto topology = runtime_topology(access);
      for (const auto& publishing : topology._publishing)
      {
        if (publishing._definition._descriptor._name == descriptor._name)
        {
          if (!same_publishing(publishing._definition._descriptor, descriptor, acceptance))
          {
            runtime_fail(ErrorCode::Conflict, "publishing_set.name", "Queue Set name already has a different descriptor");
          }
          return publishing._definition._id;
        }
      }
      for (const auto& source : topology._sources)
      {
        if (source._definition._descriptor._name == descriptor._name)
        {
          runtime_fail(ErrorCode::Conflict, "publishing_set.name", "Queue Set name belongs to a Source set");
        }
      }
      check_task(descriptor._owner, topology, "publishing_set.owner");
      if (!descriptor._destinations.empty() && descriptor._owner == TaskId::Invalid)
      {
        runtime_fail(ErrorCode::InvalidArgument, "publishing_set.owner", "Publishing destinations require a registered producer owner");
      }
      const auto& limits = access._session->_controller->configuration()._limits;
      if (descriptor._destinations.size() > limits._membershipsPerSet)
      {
        runtime_fail(ErrorCode::CapacityExceeded, "limits.memberships_per_set", "Publishing destinations exceed per-set capacity");
      }
      std::map<QueueId, TaskId> producers;
      for (const auto& queue : topology._queues)
      {
        producers.emplace(queue._definition._id, queue._definition._descriptor._producer);
      }
      for (const auto destination : descriptor._destinations)
      {
        const auto found = producers.find(destination);
        if (found == producers.end())
        {
          runtime_fail(ErrorCode::NotFound, "publishing_set.destinations", "Destination Queue is not registered");
        }
        if (found->second != descriptor._owner)
        {
          runtime_fail(ErrorCode::WrongOwner, "publishing_set.destinations", "Destination Queue belongs to a different producer");
        }
      }
      const auto slot = allocate_slot(std::span{topology._publishing}, limits._publishingSets, "limits.publishing_sets");
      const auto id = static_cast<PublishingQueueSetId>(allocate_set_id(topology));
      const std::array<std::uint64_t, 1> bodies{
          add_bytes(add_bytes(add_bytes(96, multiply_bytes(descriptor._destinations.size(), 2)),
                              descriptor._name.size()), descriptor._doc.size())};
      auto prepared = prepare_append(access, actor, bodies,
                                    add_bytes(80, multiply_bytes(descriptor._destinations.size(), 32)));
      mapped::PublishingDefinition definition;
      definition._id = id;
      definition._descriptor = descriptor;
      definition._descriptor._acceptance = acceptance;
      definition._registration = registration(prepared);
      definition._topologyRevision = 1;
      mapped::JournalRecord record;
      record._kind = 5;
      record._immutable = runtime_value(mapped::encode_publishing_definition(definition, prepared._references[0]));
      prepared._plan._records.push_back(std::move(record));
      add_index(prepared, access.section_offset(mapped::SectionKind::PublishingIndex) + slot * mapped::index_entry_bytes,
                static_cast<std::uint16_t>(id), prepared._references[0]);
      const std::array<std::uint64_t, 2> state{1, prepared._references[0]};
      prepared._plan._commands.push_back(runtime_write_words(
          access.section_offset(mapped::SectionKind::PublishingState) + slot * mapped::publishing_state_bytes, state));
      for (std::size_t member = 0; member < descriptor._destinations.size(); ++member)
      {
        add_membership(prepared, access, limits._sourceSets + slot, member, descriptor._destinations[member], 0, 1);
      }
      runtime_value(access._journal.execute(access._guard, prepared._plan));
      return id;
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }
} // namespace angaria::detail

namespace angaria
{
  Result<QueueId> Registry::register_queue(const QueueDescriptor& descriptor)
  {
    return detail::runtime_register_queue(_state, _actor, descriptor);
  }

  Result<SourceQueueSetId> Registry::register_source_set(const SourceQueueSetDescriptor& descriptor)
  {
    return detail::runtime_register_source_set(_state, _actor, descriptor);
  }

  Result<PublishingQueueSetId> Registry::register_publishing_set(const PublishingQueueSetDescriptor& descriptor)
  {
    return detail::runtime_register_publishing_set(_state, _actor, descriptor);
  }
} // namespace angaria
