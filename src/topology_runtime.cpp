#include "topology_state.h"

#include "delivery_runtime.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace angaria::detail
{
  namespace
  {
    bool all_zero(std::span<const std::uint64_t> words)
    {
      return std::all_of(words.begin(), words.end(), [](auto value)
      {
        return value == 0;
      });
    }

    bool has_native_delivery(RuntimeAccess& access)
    {
      return (access._session->_controller->configuration()._requiredFeatures &
              mapped::native_delivery_feature) != 0;
    }

    template <typename Definition>
    struct HistoricalDefinition
    {
      Definition _definition;
      std::uint64_t _reference{};
    };

    template <typename Definition>
    struct History
    {
      std::vector<HistoricalDefinition<Definition>> _records;
      std::map<std::uint16_t, std::size_t> _latest;
      std::map<std::uint16_t, std::uint64_t> _firstRevision;
    };

    void check_policy(const mapped::QueueDefinition& definition)
    {
      const auto& descriptor = definition._descriptor;
      if (descriptor._delivery != DeliveryPolicy::RecoverFromLog)
      {
        runtime_fail(ErrorCode::UnsupportedPolicy, "queue.delivery", "Queue delivery policy is not implemented by this native profile");
      }
      if (!descriptor._ringCapacity || !mapped::ring_storage_bytes(*descriptor._ringCapacity))
      {
        runtime_fail(ErrorCode::IoError, "queue.ring_capacity", "Queue ring geometry is not representable by two-plane storage");
      }
    }

    void check_policy(const mapped::SourceDefinition& definition)
    {
      const auto& descriptor = definition._descriptor;
      if (descriptor._selection != SelectionPolicy::Strict || descriptor._wait != WaitPolicy::Spin)
      {
        runtime_fail(ErrorCode::UnsupportedPolicy, "source_set.policy", "Source policy is not implemented by this native profile");
      }
    }

    void check_policy(const mapped::PublishingDefinition& definition)
    {
      if (definition._descriptor._acceptance != AcceptancePolicy::AllDestinations)
      {
        runtime_fail(ErrorCode::UnsupportedPolicy, "publishing_set.acceptance", "Publishing acceptance policy is not implemented by this native profile");
      }
    }

    void check_transition(const mapped::QueueDefinition& old, const mapped::QueueDefinition& definition)
    {
      if (old._state == EntityState::Active &&
          (definition._state != EntityState::Active ||
           old._descriptor._producer != definition._descriptor._producer ||
           old._descriptor._sourceSet != definition._descriptor._sourceSet ||
           old._descriptor._priority != definition._descriptor._priority ||
           old._descriptor._delivery != definition._descriptor._delivery ||
           old._descriptor._ringCapacity != definition._descriptor._ringCapacity))
      {
        runtime_fail(ErrorCode::IoError, "queue.history", "An active Queue changed its fixed ownership or policy");
      }
    }

    void check_transition(const mapped::SourceDefinition& old, const mapped::SourceDefinition& definition)
    {
      if (old._state == EntityState::Active &&
          (definition._state != EntityState::Active ||
           old._descriptor._owner != definition._descriptor._owner ||
           old._descriptor._selection != definition._descriptor._selection ||
           old._descriptor._wait != definition._descriptor._wait ||
           old._descriptor._randomSeed != definition._descriptor._randomSeed))
      {
        runtime_fail(ErrorCode::IoError, "source_set.history", "An active Source changed its fixed ownership or policy");
      }
    }

    void check_transition(const mapped::PublishingDefinition& old, const mapped::PublishingDefinition& definition)
    {
      if (old._state == EntityState::Active &&
          (definition._state != EntityState::Active ||
           old._descriptor._owner != definition._descriptor._owner ||
           old._descriptor._acceptance != definition._descriptor._acceptance))
      {
        runtime_fail(ErrorCode::IoError, "publishing_set.history", "An active Publishing set changed its fixed ownership or policy");
      }
      const bool changed = old._descriptor._destinations != definition._descriptor._destinations;
      if ((changed && (old._topologyRevision == std::numeric_limits<std::uint64_t>::max() ||
                       definition._topologyRevision != old._topologyRevision + 1)) ||
          (!changed && definition._topologyRevision != old._topologyRevision))
      {
        runtime_fail(ErrorCode::IoError, "publishing_set.topology_revision", "Destination changes do not match the retained topology counter");
      }
    }

    template <typename Definition, typename Decoder>
    History<Definition> read_history(RuntimeAccess& access, std::uint16_t kind, Decoder decode,
                                      const std::map<TaskId, std::uint64_t>& tasks)
    {
      History<Definition> history;
      const auto references = access.record_references(kind);
      history._records.reserve(references.size());
      for (const auto reference : references)
      {
        const auto& envelope = access.record_envelope(reference, kind);
        auto definition = runtime_value(decode(access.immutable_record(reference, kind), reference));
        const auto id = static_cast<std::uint16_t>(definition._id);
        const auto& registration = definition._registration;
        const auto creator = tasks.find(registration._createdBy);
        if (registration._revision != envelope._registryRevision ||
            registration._revision > access._snapshot._revision || creator == tasks.end() ||
            creator->second > registration._revision)
        {
          runtime_fail(ErrorCode::IoError, "topology.definition", "Definition has inconsistent revision or creator");
        }
        check_policy(definition);
        const auto previous = history._latest.find(id);
        if (previous == history._latest.end())
        {
          if (envelope._previous != 0)
          {
            runtime_fail(ErrorCode::IoError, "topology.previous", "Initial definition has a predecessor");
          }
          history._firstRevision.emplace(id, registration._revision);
        }
        else
        {
          const auto& retained = history._records[previous->second];
          const auto& old = retained._definition;
          if (envelope._previous != retained._reference ||
              old._registration._revision > registration._revision ||
              old._registration._createdBy != registration._createdBy ||
              old._registration._createdAt != registration._createdAt ||
              old._descriptor._name != definition._descriptor._name)
          {
            runtime_fail(ErrorCode::IoError, "topology.previous", "Retained definition chain or creation attribution is inconsistent");
          }
        }
        history._latest[id] = history._records.size();
        history._records.push_back(HistoricalDefinition<Definition>{std::move(definition), reference});
      }
      return history;
    }

    template <typename Loaded, typename Definition>
    std::vector<Loaded> current_definitions(RuntimeAccess& access, const History<Definition>& history,
                                            mapped::SectionKind indexKind, mapped::SectionKind stateKind,
                                            std::uint64_t capacity, std::uint64_t stateBytes,
                                            std::map<QueueId, std::uint64_t>* ringReferences = nullptr)
    {
      const auto indexBegin = access.section_offset(indexKind);
      const auto stateBegin = access.section_offset(stateKind);
      const auto indexes = access.read_words(indexBegin, static_cast<std::size_t>(capacity * 8));
      const auto states = access.read_words(stateBegin, static_cast<std::size_t>(capacity * stateBytes / 8));
      std::set<std::uint16_t> ids;
      std::vector<Loaded> loaded;
      loaded.reserve(history._latest.size());
      for (std::uint64_t slot = 0; slot < capacity; ++slot)
      {
        const auto row = std::span{indexes}.subspan(static_cast<std::size_t>(slot * 8), 8);
        const auto state = std::span{states}.subspan(static_cast<std::size_t>(slot * stateBytes / 8),
                                                   static_cast<std::size_t>(stateBytes / 8));
        if (row[0] == 0)
        {
          if (!all_zero(row) || !all_zero(state))
          {
            runtime_fail(ErrorCode::IoError, "topology.index", "Empty topology allocation slot contains index or live state");
          }
          continue;
        }
        if (row[0] > mapped::max_entity_id || row[1] == 0 || row[2] == 0 ||
            row[2] > access._snapshot._revision || (row[3] != 1 && row[3] != 2) || !all_zero(row.subspan(4)))
        {
          runtime_fail(ErrorCode::IoError, "topology.index", "Invalid topology identity, revision, state, or reserved data");
        }
        const auto id = static_cast<std::uint16_t>(row[0]);
        const auto found = history._latest.find(id);
        if (!ids.insert(id).second || found == history._latest.end())
        {
          runtime_fail(ErrorCode::IoError, "topology.index", "Duplicate or unknown topology identity");
        }
        const auto& latest = history._records[found->second];
        if (row[1] != latest._reference || row[2] != latest._definition._registration._revision ||
            row[3] != (latest._definition._state == EntityState::Draft ? 1u : 2u))
        {
          runtime_fail(ErrorCode::IoError, "topology.index", "Topology index does not reference its latest retained definition");
        }
        if constexpr (std::is_same_v<Definition, mapped::PublishingDefinition>)
        {
          if (state[0] != latest._definition._topologyRevision || state[1] != latest._reference || !all_zero(state.subspan(2)))
          {
            runtime_fail(ErrorCode::IoError, "publishing_set.state", "Publishing snapshot state is inconsistent");
          }
        }
        else if constexpr (std::is_same_v<Definition, mapped::QueueDefinition>)
        {
          if (latest._definition._state == EntityState::Draft)
          {
            if (!all_zero(state))
            {
              runtime_fail(ErrorCode::IoError, "queue.state", "Draft Queue contains endpoint or ring state");
            }
          }
          else
          {
            const bool delivery = has_native_delivery(access);
            if (state[2] == 0 || state[3] != latest._definition._descriptor._ringCapacity ||
                (delivery ? state[7] != 0 || !all_zero(state.subspan(12)) :
                 state[0] != 0 || state[1] != 0 || !all_zero(state.subspan(4))))
            {
              runtime_fail(ErrorCode::IoError, "queue.state", "Active Queue has invalid ring geometry or unsupported delivery state");
            }
            ringReferences->emplace(latest._definition._id, state[2]);
          }
        }
        else
        {
          // Core delivery assigns only the first eight Source words. Strict/Spin
          // scheduling and the recovery reservation stay zero. Draft Sources
          // and legacy profiles retain the previous all-zero requirement.
          const bool delivery = has_native_delivery(access) &&
                                latest._definition._state == EntityState::Active;
          if (!all_zero(delivery ? state.subspan(8) : state))
          {
            runtime_fail(ErrorCode::IoError, "source_set.state", "Source contains unsupported endpoint or scheduling state");
          }
        }
        loaded.push_back(Loaded{latest._definition, latest._reference,
            indexBegin + slot * mapped::index_entry_bytes, stateBegin + slot * stateBytes, slot});
      }
      // Comparing latest identities also detects a wholly cleared newest/only
      // index row, while earlier retained revisions remain legitimate history.
      if (loaded.size() != history._latest.size())
      {
        runtime_fail(ErrorCode::IoError, "topology.index", "A retained topology identity has no current index");
      }
      return loaded;
    }

    void check_task(TaskId id, std::uint64_t revision, const std::map<TaskId, std::uint64_t>& tasks)
    {
      if (id != TaskId::Invalid)
      {
        const auto found = tasks.find(id);
        if (found == tasks.end() || found->second > revision)
        {
          runtime_fail(ErrorCode::IoError, "topology.owner", "Definition references a Task absent at its registration");
        }
      }
    }

    template <typename Id>
    void check_reference(Id id, std::uint64_t revision,
                           const std::map<std::uint16_t, std::uint64_t>& first,
                           const char* path)
    {
      const auto found = first.find(static_cast<std::uint16_t>(id));
      if (found == first.end() || found->second > revision)
      {
        runtime_fail(ErrorCode::IoError, path, "Definition references an entity absent at its registration");
      }
    }

    void check_members(RuntimeAccess& access, std::uint64_t group,
                         std::span<const std::uint64_t> expected,
                         std::uint64_t revision, std::uint32_t capacity)
    {
      const auto begin = access.section_offset(mapped::SectionKind::MembershipState) +
                         group * capacity * mapped::membership_bytes;
      const auto words = access.read_words(begin, std::size_t{capacity} * 4);
      for (std::size_t slot = 0; slot < capacity; ++slot)
      {
        const auto row = std::span{words}.subspan(slot * 4, 4);
        if (slot < expected.size())
        {
          if (row[0] != expected[slot] || row[1] != revision || row[2] != 0 || row[3] != 0)
          {
            runtime_fail(ErrorCode::IoError, "topology.membership", "Membership state disagrees with its immutable definition");
          }
        }
        else if (!all_zero(row))
        {
          runtime_fail(ErrorCode::IoError, "topology.membership", "Unused membership slot contains state");
        }
      }
    }

    template <typename Id, typename Definition>
    const Definition& effective_definition(const std::map<Id, const Definition*>& definitions,
                                            Id id, const char* path)
    {
      const auto found = definitions.find(id);
      if (found == definitions.end())
      {
        runtime_fail(ErrorCode::IoError, path, "Historical relationship names an entity absent at that revision");
      }
      return *found->second;
    }

    struct RevisionChanges
    {
      std::map<QueueId, const mapped::QueueDefinition*> _queues;
      std::map<SourceQueueSetId, const mapped::SourceDefinition*> _sources;
      std::map<PublishingQueueSetId, const mapped::PublishingDefinition*> _publishing;
    };

    // Replay only definition changes, not every entity at every revision. Each
    // batch checks transitions against the preceding committed definitions and
    // installs its final per-entity definitions before checking relationships.
    // Intermediate same-revision records retain provenance, not visible states.
    std::map<QueueId, std::uint64_t> check_historical_topology(
        const History<mapped::QueueDefinition>& queues,
        const History<mapped::SourceDefinition>& sources,
        const History<mapped::PublishingDefinition>& publishing,
        const std::map<TaskId, std::uint64_t>& tasks, std::uint32_t capacity)
    {
      std::map<std::uint64_t, RevisionChanges> revisions;
      std::unordered_map<const mapped::SourceDefinition*, std::unordered_set<QueueId>> members;
      for (const auto& entry : queues._records)
      {
        const auto& definition = entry._definition;
        const auto revision = definition._registration._revision;
        check_task(definition._descriptor._producer, revision, tasks);
        if (definition._descriptor._sourceSet != SourceQueueSetId::Invalid)
        {
          check_reference(definition._descriptor._sourceSet, revision, sources._firstRevision, "queue.source_set");
        }
        revisions[revision]._queues[definition._id] = &definition;
      }
      for (const auto& entry : sources._records)
      {
        const auto& definition = entry._definition;
        const auto revision = definition._registration._revision;
        check_task(definition._descriptor._owner, revision, tasks);
        if (definition._members.size() > capacity)
        {
          runtime_fail(ErrorCode::IoError, "source_set.members", "Historical Source membership exceeds the session capacity");
        }
        for (const auto member : definition._members)
        {
          check_reference(member, revision, queues._firstRevision, "source_set.members");
        }
        members.emplace(&definition, std::unordered_set<QueueId>{definition._members.begin(), definition._members.end()});
        revisions[revision]._sources[definition._id] = &definition;
      }
      for (const auto& entry : publishing._records)
      {
        const auto& definition = entry._definition;
        const auto revision = definition._registration._revision;
        check_task(definition._descriptor._owner, revision, tasks);
        if (definition._descriptor._destinations.size() > capacity)
        {
          runtime_fail(ErrorCode::IoError, "publishing_set.destinations", "Historical Publishing membership exceeds the session capacity");
        }
        for (const auto member : definition._descriptor._destinations)
        {
          check_reference(member, revision, queues._firstRevision, "publishing_set.destinations");
        }
        revisions[revision]._publishing[definition._id] = &definition;
      }

      std::map<QueueId, const mapped::QueueDefinition*> effectiveQueues;
      std::map<SourceQueueSetId, const mapped::SourceDefinition*> effectiveSources;
      std::map<PublishingQueueSetId, const mapped::PublishingDefinition*> effectivePublishing;
      std::map<QueueId, std::set<PublishingQueueSetId>> publishers;
      std::map<QueueId, std::uint64_t> firstActivation;
      const auto check_publishing = [&](const mapped::PublishingDefinition& definition)
      {
        for (const auto destination : definition._descriptor._destinations)
        {
          const auto& queue = effective_definition(effectiveQueues, destination, "publishing_set.destinations");
          if (definition._descriptor._owner == TaskId::Invalid ||
              queue._descriptor._producer != definition._descriptor._owner ||
              (definition._state == EntityState::Active && queue._state != EntityState::Active))
          {
            runtime_fail(ErrorCode::IoError, "publishing_set.destinations", "Historical Publishing destination ownership or activation is inconsistent");
          }
        }
      };
      for (const auto& [revision, changes] : revisions)
      {
        const auto previousQueues = [&]
        {
          std::map<QueueId, const mapped::QueueDefinition*> previous;
          for (const auto& [id, definition] : changes._queues)
          {
            const auto old = effectiveQueues.find(id);
            if (old != effectiveQueues.end())
            {
              check_transition(*old->second, *definition);
            }
            previous.emplace(id, old == effectiveQueues.end() ? nullptr : old->second);
            effectiveQueues[id] = definition;
          }
          return previous;
        }();
        const auto previousSources = [&]
        {
          std::map<SourceQueueSetId, const mapped::SourceDefinition*> previous;
          for (const auto& [id, definition] : changes._sources)
          {
            const auto old = effectiveSources.find(id);
            if (old != effectiveSources.end())
            {
              check_transition(*old->second, *definition);
            }
            previous.emplace(id, old == effectiveSources.end() ? nullptr : old->second);
            effectiveSources[id] = definition;
          }
          return previous;
        }();
        for (const auto& [id, definition] : changes._publishing)
        {
          const auto previous = effectivePublishing.find(id);
          if (previous != effectivePublishing.end())
          {
            check_transition(*previous->second, *definition);
            for (const auto destination : previous->second->_descriptor._destinations)
            {
              publishers[destination].erase(id);
            }
          }
          else if (definition->_topologyRevision != 1)
          {
            runtime_fail(ErrorCode::IoError, "publishing_set.topology_revision", "Initial committed Publishing topology revision is not one");
          }
          effectivePublishing[id] = definition;
          for (const auto destination : definition->_descriptor._destinations)
          {
            publishers[destination].insert(id);
          }
        }
        for (const auto& [id, definition] : changes._sources)
        {
          for (const auto member : definition->_members)
          {
            const auto& queue = effective_definition(effectiveQueues, member, "source_set.members");
            if (queue._descriptor._sourceSet != id ||
                (definition->_state == EntityState::Active && queue._descriptor._producer == TaskId::Invalid))
            {
              runtime_fail(ErrorCode::IoError, "source_set.members", "Historical Source membership or active Queue completeness is inconsistent");
            }
          }
          const auto* previous = previousSources.at(id);
          if (previous)
          {
            const auto& currentMembers = members.at(definition);
            for (const auto removed : previous->_members)
            {
              if (!currentMembers.contains(removed) &&
                  effective_definition(effectiveQueues, removed, "source_set.members")._descriptor._sourceSet == id)
              {
                runtime_fail(ErrorCode::IoError, "source_set.members", "Historical Source removed a Queue still assigned to it");
              }
            }
          }
        }
        for (const auto& [id, definition] : changes._queues)
        {
          const auto sourceId = definition->_descriptor._sourceSet;
          if (sourceId != SourceQueueSetId::Invalid)
          {
            const auto& source = effective_definition(effectiveSources, sourceId, "queue.source_set");
            if (!members.at(&source).contains(id) ||
                (source._state == EntityState::Active && definition->_descriptor._producer == TaskId::Invalid) ||
                (definition->_state == EntityState::Active && source._state != EntityState::Active))
            {
              runtime_fail(ErrorCode::IoError, "queue.source_set", "Historical Queue assignment or Source activation is inconsistent");
            }
          }
          if (definition->_state == EntityState::Active)
          {
            // The portable decoder requires nonzero producer/source identities.
            firstActivation.try_emplace(id, revision);
          }
          const auto* previous = previousQueues.at(id);
          if (previous && previous->_descriptor._sourceSet != SourceQueueSetId::Invalid &&
              previous->_descriptor._sourceSet != sourceId)
          {
            const auto& oldSource = effective_definition(effectiveSources, previous->_descriptor._sourceSet, "queue.source_set");
            if (members.at(&oldSource).contains(id))
            {
              runtime_fail(ErrorCode::IoError, "queue.source_set", "Historical Queue remains listed in its former Source");
            }
          }
          if (previous && previous->_descriptor._producer != definition->_descriptor._producer)
          {
            // A Queue owner change must also preserve every Publishing set
            // currently referencing it, even when that set has no new record.
            const auto references = publishers.find(id);
            if (references != publishers.end())
            {
              for (const auto publisher : references->second)
              {
                const auto& set = effective_definition(effectivePublishing, publisher, "publishing_set.destinations");
                if (set._descriptor._owner == TaskId::Invalid || set._descriptor._owner != definition->_descriptor._producer)
                {
                  runtime_fail(ErrorCode::IoError, "queue.producer", "Historical Queue ownership invalidates a Publishing reference");
                }
              }
            }
          }
        }
        for (const auto& [id, definition] : changes._publishing)
        {
          (void)id;
          check_publishing(*definition);
        }
      }
      return firstActivation;
    }

    void check_rings(RuntimeAccess& access, const TopologySnapshot& topology,
                       const std::map<QueueId, std::uint64_t>& references,
                       const std::map<QueueId, std::uint64_t>& firstActivation)
    {
      std::set<std::uint64_t> owned;
      for (const auto& queue : topology._queues)
      {
        if (queue._definition._state != EntityState::Active)
        {
          continue;
        }
        const auto reference = references.at(queue._definition._id);
        const auto& envelope = access.record_envelope(reference, 8);
        const auto expected = mapped::ring_storage_bytes(*queue._definition._descriptor._ringCapacity);
        if (!owned.insert(reference).second || envelope._encoding != 1 || envelope._immutableBytes != 0 ||
            envelope._previous != 0 || !expected || envelope._totalBytes != *expected ||
            envelope._registryRevision != firstActivation.at(queue._definition._id))
        {
          runtime_fail(ErrorCode::IoError, "queue.ring", "Active Queue ring ownership, geometry, or activation revision is inconsistent");
        }
        if (has_native_delivery(access))
        {
          // The delivery validator checks each occupied slot against committed
          // Queue history and also verifies allocation padding atomically.
          continue;
        }
        // Ring slots and allocation padding are mutable words. Read bounded
        // batches atomically under the gate, without a mapping per word.
        const auto words = (envelope._totalBytes - mapped::record_envelope_bytes) / 8;
        for (std::uint64_t offset = 0; offset < words;)
        {
          const auto count = std::min<std::uint64_t>(words - offset, 4096);
          if (!all_zero(access.read_words(reference + mapped::record_envelope_bytes + offset * 8,
                                           static_cast<std::size_t>(count))))
          {
            runtime_fail(ErrorCode::IoError, "queue.ring", "Ring contains unsupported delivery state");
          }
          offset += count;
        }
      }
      const auto retained = access.record_references(8);
      if (std::vector<std::uint64_t>{owned.begin(), owned.end()} != retained)
      {
        runtime_fail(ErrorCode::IoError, "queue.ring", "Retained ring storage has no unique active Queue owner");
      }
    }
  } // namespace

  TopologySnapshot runtime_topology(RuntimeAccess& access)
  {
    TopologySnapshot topology;
    topology._tasks = runtime_tasks(access);
    std::map<TaskId, std::uint64_t> tasks;
    for (const auto& task : topology._tasks)
    {
      tasks.emplace(task._definition._id, task._definition._registration._revision);
    }
    const auto queues = read_history<mapped::QueueDefinition>(access, 3, mapped::decode_queue_definition, tasks);
    const auto sources = read_history<mapped::SourceDefinition>(access, 4, mapped::decode_source_definition, tasks);
    const auto publishing = read_history<mapped::PublishingDefinition>(access, 5, mapped::decode_publishing_definition, tasks);
    const auto& limits = access._session->_controller->configuration()._limits;
    if ((!has_native_delivery(access) && !access.record_references(9).empty()) ||
        !access.record_references(10).empty())
    {
      runtime_fail(ErrorCode::UnsupportedPolicy, "topology.delivery", "Publication and retry delivery state are not implemented by this native profile");
    }
    std::map<QueueId, std::uint64_t> ringReferences;
    topology._queues = current_definitions<LoadedQueue>(access, queues, mapped::SectionKind::QueueIndex,
        mapped::SectionKind::QueueState, limits._queues, mapped::queue_state_bytes, &ringReferences);
    topology._sources = current_definitions<LoadedSource>(access, sources, mapped::SectionKind::SourceIndex,
        mapped::SectionKind::SourceState, limits._sourceSets, mapped::source_state_bytes);
    topology._publishing = current_definitions<LoadedPublishing>(access, publishing, mapped::SectionKind::PublishingIndex,
        mapped::SectionKind::PublishingState, limits._publishingSets, mapped::publishing_state_bytes);

    std::set<std::string> queueNames;
    std::map<QueueId, const LoadedQueue*> queueIds;
    for (const auto& queue : topology._queues)
    {
      if (!queueNames.insert(queue._definition._descriptor._name).second)
      {
        runtime_fail(ErrorCode::IoError, "queue.name", "Queue names are not unique");
      }
      queueIds.emplace(queue._definition._id, &queue);
    }
    std::set<std::uint16_t> setIds;
    std::set<std::string> setNames;
    std::map<SourceQueueSetId, const LoadedSource*> sourceIds;
    std::map<QueueId, SourceQueueSetId> assignments;
    std::set<std::uint64_t> sourceSlots;
    std::set<std::uint64_t> publishingSlots;
    for (const auto& source : topology._sources)
    {
      if (!setIds.insert(static_cast<std::uint16_t>(source._definition._id)).second ||
          !setNames.insert(source._definition._descriptor._name).second ||
          source._definition._members.size() > limits._membershipsPerSet)
      {
        runtime_fail(ErrorCode::IoError, "source_set.index", "Source identity/name or membership capacity is inconsistent");
      }
      sourceIds.emplace(source._definition._id, &source);
      sourceSlots.insert(source._slot);
      std::vector<std::uint64_t> expected;
      expected.reserve(source._definition._members.size());
      for (const auto id : source._definition._members)
      {
        const auto found = queueIds.find(id);
        if (found == queueIds.end() || found->second->_definition._descriptor._sourceSet != source._definition._id ||
            !assignments.emplace(id, source._definition._id).second)
        {
          runtime_fail(ErrorCode::IoError, "source_set.members", "Source membership references a Queue assigned elsewhere");
        }
        expected.push_back(static_cast<std::uint16_t>(id) |
                           (std::uint64_t{found->second->_definition._descriptor._priority} << 16));
      }
      check_members(access, source._slot, expected, source._definition._registration._revision, limits._membershipsPerSet);
    }
    for (const auto& set : topology._publishing)
    {
      const auto& definition = set._definition;
      if (!setIds.insert(static_cast<std::uint16_t>(definition._id)).second ||
          !setNames.insert(definition._descriptor._name).second ||
          definition._descriptor._destinations.size() > limits._membershipsPerSet)
      {
        runtime_fail(ErrorCode::IoError, "publishing_set.index", "Publishing identity/name or membership capacity is inconsistent");
      }
      publishingSlots.insert(set._slot);
      std::vector<std::uint64_t> expected;
      expected.reserve(definition._descriptor._destinations.size());
      for (const auto id : definition._descriptor._destinations)
      {
        const auto queue = queueIds.find(id);
        if (queue == queueIds.end() || definition._descriptor._owner == TaskId::Invalid ||
            queue->second->_definition._descriptor._producer != definition._descriptor._owner)
        {
          runtime_fail(ErrorCode::IoError, "publishing_set.destinations", "Publishing destination ownership is inconsistent");
        }
        expected.push_back(static_cast<std::uint16_t>(id));
      }
      check_members(access, limits._sourceSets + set._slot, expected, definition._topologyRevision, limits._membershipsPerSet);
    }
    // Empty allocation slots have no memberships either. The source-capacity
    // prefix, rather than the number currently registered, locates publishers.
    for (std::uint64_t slot = 0; slot < limits._sourceSets; ++slot)
    {
      if (!sourceSlots.contains(slot))
      {
        check_members(access, slot, {}, 0, limits._membershipsPerSet);
      }
    }
    for (std::uint64_t slot = 0; slot < limits._publishingSets; ++slot)
    {
      if (!publishingSlots.contains(slot))
      {
        check_members(access, limits._sourceSets + slot, {}, 0, limits._membershipsPerSet);
      }
    }

    const auto firstActivation = check_historical_topology(queues, sources, publishing, tasks, limits._membershipsPerSet);
    check_rings(access, topology, ringReferences, firstActivation);
    for (const auto& queue : topology._queues)
    {
      const auto sourceId = queue._definition._descriptor._sourceSet;
      if (sourceId == SourceQueueSetId::Invalid)
      {
        continue;
      }
      const auto found = sourceIds.find(sourceId);
      const auto assigned = assignments.find(queue._definition._id);
      if (found == sourceIds.end() || assigned == assignments.end() || assigned->second != sourceId)
      {
        runtime_fail(ErrorCode::IoError, "queue.source_set", "Queue assignment has no matching Source membership");
      }
    }
    if (has_native_delivery(access))
    {
      runtime_validate_delivery(access, topology);
    }
    return topology;
  }
} // namespace angaria::detail

namespace angaria
{
  namespace
  {
    detail::RuntimeAccess topology_access(const std::shared_ptr<detail::SessionState>& state,
                                           const std::shared_ptr<detail::TaskState>& actor)
    {
      if (actor)
      {
        detail::runtime_check_task_owner(actor);
      }
      detail::RuntimeAccess access(state);
      if (actor)
      {
        detail::runtime_check_task(access, actor);
      }
      return access;
    }

    template <typename Id, typename Loaded>
    const Loaded& by_id(const std::vector<Loaded>& entries, Id id, const char* path)
    {
      if (static_cast<std::uint16_t>(id) == 0)
      {
        detail::runtime_fail(ErrorCode::InvalidArgument, path, "Entity identity is zero");
      }
      const auto found = std::find_if(entries.begin(), entries.end(), [id](const auto& entry)
      {
        return entry._definition._id == id;
      });
      if (found == entries.end())
      {
        detail::runtime_fail(ErrorCode::NotFound, path, "Entity is not registered with this kind");
      }
      return *found;
    }

    template <typename Loaded>
    auto by_name(const std::vector<Loaded>& entries, std::string_view name, const char* path)
    {
      detail::runtime_value(detail::mapped::validate_metadata_text(name, true, true, path));
      for (const auto& entry : entries)
      {
        if (entry._definition._descriptor._name == name)
        {
          return entry._definition._id;
        }
      }
      detail::runtime_fail(ErrorCode::NotFound, path, "Entity name is not registered with this kind");
    }
  } // namespace

  Result<QueueId> Registry::find_queue(std::string_view name) const
  {
    try
    {
      auto access = topology_access(_state, _actor);
      return by_name(detail::runtime_topology(access)._queues, name, "queue.name");
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<SourceQueueSetId> Registry::find_source_set(std::string_view name) const
  {
    try
    {
      auto access = topology_access(_state, _actor);
      return by_name(detail::runtime_topology(access)._sources, name, "source_set.name");
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<PublishingQueueSetId> Registry::find_publishing_set(std::string_view name) const
  {
    try
    {
      auto access = topology_access(_state, _actor);
      return by_name(detail::runtime_topology(access)._publishing, name, "publishing_set.name");
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<QueueDescriptor> Registry::describe_queue(QueueId id) const
  {
    try
    {
      auto access = topology_access(_state, _actor);
      const auto topology = detail::runtime_topology(access);
      return by_id(topology._queues, id, "queue.id")._definition._descriptor;
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<SourceQueueSetDescriptor> Registry::describe_source_set(SourceQueueSetId id) const
  {
    try
    {
      auto access = topology_access(_state, _actor);
      const auto topology = detail::runtime_topology(access);
      return by_id(topology._sources, id, "source_set.id")._definition._descriptor;
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<PublishingQueueSetDescriptor> Registry::describe_publishing_set(PublishingQueueSetId id) const
  {
    try
    {
      auto access = topology_access(_state, _actor);
      const auto topology = detail::runtime_topology(access);
      return by_id(topology._publishing, id, "publishing_set.id")._definition._descriptor;
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<std::vector<QueueId>> Registry::source_members(SourceQueueSetId id) const
  {
    try
    {
      auto access = topology_access(_state, _actor);
      const auto topology = detail::runtime_topology(access);
      return by_id(topology._sources, id, "source_set.id")._definition._members;
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<RegistrationMetadata> Registry::metadata(QueueId id) const
  {
    try
    {
      auto access = topology_access(_state, _actor);
      const auto topology = detail::runtime_topology(access);
      return by_id(topology._queues, id, "queue.id")._definition._registration;
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<RegistrationMetadata> Registry::metadata(SourceQueueSetId id) const
  {
    try
    {
      auto access = topology_access(_state, _actor);
      const auto topology = detail::runtime_topology(access);
      return by_id(topology._sources, id, "source_set.id")._definition._registration;
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<RegistrationMetadata> Registry::metadata(PublishingQueueSetId id) const
  {
    try
    {
      auto access = topology_access(_state, _actor);
      const auto topology = detail::runtime_topology(access);
      return by_id(topology._publishing, id, "publishing_set.id")._definition._registration;
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<EntityState> Registry::state(QueueId id) const
  {
    try
    {
      auto access = topology_access(_state, _actor);
      const auto topology = detail::runtime_topology(access);
      return by_id(topology._queues, id, "queue.id")._definition._state;
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<EntityState> Registry::state(SourceQueueSetId id) const
  {
    try
    {
      auto access = topology_access(_state, _actor);
      const auto topology = detail::runtime_topology(access);
      return by_id(topology._sources, id, "source_set.id")._definition._state;
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<EntityState> Registry::state(PublishingQueueSetId id) const
  {
    try
    {
      auto access = topology_access(_state, _actor);
      const auto topology = detail::runtime_topology(access);
      return by_id(topology._publishing, id, "publishing_set.id")._definition._state;
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }
} // namespace angaria
