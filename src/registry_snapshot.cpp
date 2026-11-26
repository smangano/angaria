#include "runtime_state.h"
#include "registry_access.h"
#include "topology_state.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace angaria
{
  namespace
  {
    template <typename Definition, typename Decoder>
    std::vector<Definition> definitions_at(detail::RuntimeAccess& access, std::uint16_t kind,
                                            std::uint64_t revision, Decoder decode)
    {
      using Id = decltype(Definition{}._id);
      std::map<Id, Definition> selected;
      // References follow committed record order. Replacing a map entry keeps
      // the final definition when one transaction records an entity twice.
      // Current validation has already checked chains and every historical cut.
      for (const auto reference : access.record_references(kind))
      {
        auto definition = detail::runtime_value(decode(access.immutable_record(reference, kind), reference));
        if (definition._registration._revision <= revision)
        {
          const auto id = definition._id;
          selected.insert_or_assign(id, std::move(definition));
        }
      }
      std::vector<Definition> result;
      result.reserve(selected.size());
      for (auto& [id, definition] : selected)
      {
        (void)id;
        result.push_back(std::move(definition));
      }
      return result;
    }

    template <typename Entry>
    void sort_by_id(std::vector<Entry>& entries)
    {
      std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right)
      {
        return left._id < right._id;
      });
    }

    void add_queue(RegistrySnapshot& snapshot, const detail::mapped::QueueDefinition& definition)
    {
      snapshot._queues.push_back(QueueSnapshot{definition._id, definition._descriptor,
          definition._registration, definition._state});
    }

    void add_source(RegistrySnapshot& snapshot, const detail::mapped::SourceDefinition& definition)
    {
      snapshot._sources.push_back(SourceSnapshot{definition._id, definition._descriptor,
          definition._registration, definition._state, definition._members});
    }

    void add_publishing(RegistrySnapshot& snapshot, const detail::mapped::PublishingDefinition& definition)
    {
      snapshot._publishing.push_back(PublishingSnapshot{definition._id, definition._descriptor,
          definition._registration, definition._state, definition._topologyRevision});
    }
  } // namespace

  Result<RegistrySnapshot> Registry::snapshot(std::optional<std::uint64_t> revision) const
  {
    try
    {
      // Match typed Registry reads: reject inherited or wrong-thread actor use
      // before acquiring a process-local mutex, then verify its live claim.
      auto access = detail::registry_access(_state, _actor);
      (void)detail::runtime_session_definition(access);
      const auto fields = detail::runtime_fields(access);
      const auto schemas = detail::runtime_schemas(access, fields);
      const auto topology = detail::runtime_topology(access);

      const auto current = access._snapshot._revision;
      const auto selected = revision.value_or(current);
      if (selected == 0)
      {
        detail::runtime_fail(ErrorCode::InvalidArgument, "registry.revision", "Registry revisions start at one");
      }
      if (selected > current)
      {
        detail::runtime_fail(ErrorCode::NotFound, "registry.revision", "Requested Registry revision has not been committed");
      }
      RegistrySnapshot result;
      result._revision = selected;
      result._currentRevision = current;
      for (const auto& task : topology._tasks)
      {
        const auto& definition = task._definition;
        if (definition._registration._revision <= selected)
        {
          result._tasks.push_back(TaskSnapshot{definition._id, definition._descriptor, definition._registration});
        }
      }
      for (const auto& field : fields)
      {
        const auto& definition = field._definition;
        if (definition._registration._revision <= selected)
        {
          result._fields.push_back(FieldSnapshot{definition._descriptor, definition._registration});
        }
      }
      for (const auto& schema : schemas)
      {
        const auto& definition = schema._definition;
        if (definition._registration._revision <= selected)
        {
          result._messages.push_back(MessageSnapshot{definition._descriptor, definition._registration});
        }
      }
      if (selected == current)
      {
        for (const auto& queue : topology._queues)
        {
          add_queue(result, queue._definition);
        }
        for (const auto& source : topology._sources)
        {
          add_source(result, source._definition);
        }
        for (const auto& publishing : topology._publishing)
        {
          add_publishing(result, publishing._definition);
        }
      }
      else
      {
        for (const auto& definition : definitions_at<detail::mapped::QueueDefinition>(
                 access, 3, selected, detail::mapped::decode_queue_definition))
        {
          add_queue(result, definition);
        }
        for (const auto& definition : definitions_at<detail::mapped::SourceDefinition>(
                 access, 4, selected, detail::mapped::decode_source_definition))
        {
          add_source(result, definition);
        }
        for (const auto& definition : definitions_at<detail::mapped::PublishingDefinition>(
                 access, 5, selected, detail::mapped::decode_publishing_definition))
        {
          add_publishing(result, definition);
        }
      }
      sort_by_id(result._tasks);
      sort_by_id(result._queues);
      sort_by_id(result._sources);
      sort_by_id(result._publishing);
      std::sort(result._fields.begin(), result._fields.end(), [](const auto& left, const auto& right)
      {
        return left._descriptor._id < right._descriptor._id;
      });
      std::sort(result._messages.begin(), result._messages.end(), [](const auto& left, const auto& right)
      {
        return std::tie(left._descriptor._schema._type, left._descriptor._schema._version) <
               std::tie(right._descriptor._schema._type, right._descriptor._schema._version);
      });
      return result;
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }
} // namespace angaria
