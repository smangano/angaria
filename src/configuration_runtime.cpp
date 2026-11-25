#include "configuration_runtime.h"

#include "runtime_state.h"
#include "schema_metadata.h"
#include "topology_append.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace angaria::detail
{
  namespace
  {
    template <typename Descriptor>
    std::uint64_t definition_bytes(std::uint64_t fixed, const Descriptor& descriptor,
                                     std::uint64_t extra = 0)
    {
      return add_bytes(add_bytes(add_bytes(fixed, extra), descriptor._name.size()), descriptor._doc.size());
    }

    class ConfigurationBudget
    {
    public:
      ConfigurationBudget(std::uint64_t tail, std::uint64_t limit) : _tail(tail), _limit(limit)
      {
      }

      void append(std::initializer_list<std::uint64_t> bodies, std::uint64_t commands,
                     const std::string& path)
      {
        try
        {
          _tail = append_tail(_tail, std::span{bodies.begin(), bodies.size()}, commands, _limit);
        }
        catch (const RuntimeFailure& failure)
        {
          auto error = failure._error;
          error._message += " while planning " + path;
          throw RuntimeFailure{std::move(error)};
        }
      }

    private:
      std::uint64_t _tail;
      std::uint64_t _limit;
    };

    template <typename T>
    T configured_value(Result<T> result, const std::string& path)
    {
      if (auto* error = std::get_if<Error>(&result))
      {
        if (error->_code == ErrorCode::InvalidArgument)
        {
          error->_code = ErrorCode::InvalidConfiguration;
        }
        error->_message += " while applying " + path;
      }
      return runtime_value(std::move(result));
    }

    void match(bool equal, const char* path)
    {
      if (!equal)
      {
        runtime_fail(ErrorCode::ConfigurationMismatch, path, "Supplied setting differs from the recorded effective configuration");
      }
    }
  } // namespace

  void preflight_configuration(const ConfigurationPlan& configuration, std::uint64_t bootstrapTail)
  {
    ConfigurationBudget budget{bootstrapTail, configuration._options._maxLogBytes};
    // The creator's declared entry was already included in the bootstrap.
    for (std::size_t index = 1; index < configuration._tasks.size(); ++index)
    {
      const auto& entry = configuration._tasks[index];
      budget.append({definition_bytes(72, entry._descriptor)}, 48, entry._path);
    }
    for (const auto& entry : configuration._fields)
    {
      budget.append({configured_value(mapped::field_definition_body_bytes(entry._descriptor), entry._path)},
          48, entry._path);
    }
    for (const auto& entry : configuration._messages)
    {
      budget.append({definition_bytes(88, entry._descriptor,
          multiply_bytes(entry._descriptor._fields.size(), 4))}, 48, entry._path);
    }
    for (const auto& entry : configuration._sources)
    {
      budget.append({definition_bytes(96, entry._descriptor)}, 48, entry._path);
    }
    std::vector<std::uint64_t> members(configuration._sources.size());
    for (const auto& entry : configuration._queues)
    {
      // Fresh-session Source IDs follow their planned registration order.
      const auto sourceIndex = static_cast<std::size_t>(entry._descriptor._sourceSet) - 1;
      const auto& source = configuration._sources.at(sourceIndex);
      const auto count = ++members.at(sourceIndex);
      budget.append({definition_bytes(80, entry._descriptor),
          definition_bytes(96, source._descriptor, multiply_bytes(count, 2))},
          add_bytes(96, multiply_bytes(count, 32)), entry._path);
    }
    for (const auto& entry : configuration._publishing)
    {
      const auto count = entry._descriptor._destinations.size();
      budget.append({definition_bytes(96, entry._descriptor, multiply_bytes(count, 2))},
          add_bytes(80, multiply_bytes(count, 32)), entry._path);
    }
    for (std::size_t index = 0; index < configuration._sources.size(); ++index)
    {
      const auto& entry = configuration._sources[index];
      if (const auto count = members[index]; count != 0)
      {
        budget.append({definition_bytes(96, entry._descriptor, multiply_bytes(count, 2))},
            add_bytes(48, multiply_bytes(count, 32)), entry._path + ".activation");
      }
    }
    for (const auto& entry : configuration._queues)
    {
      const auto capacity = entry._descriptor._ringCapacity.value_or(configuration._options._defaults._ringCapacity);
      budget.append({definition_bytes(80, entry._descriptor), multiply_bytes(capacity, mapped::ring_slot_bytes)},
          80, entry._path + ".activation");
    }
    for (const auto& entry : configuration._publishing)
    {
      if (const auto count = entry._descriptor._destinations.size(); count != 0)
      {
        budget.append({definition_bytes(96, entry._descriptor, multiply_bytes(count, 2))},
            add_bytes(80, multiply_bytes(count, 32)), entry._path + ".activation");
      }
    }
  }

  void apply_configuration(CreatedSession& session, const ConfigurationPlan& configuration)
  {
    auto registry = session._creator.registry();
    for (std::size_t index = 1; index < configuration._tasks.size(); ++index)
    {
      const auto& entry = configuration._tasks[index];
      configured_value(registry.register_task(entry._descriptor), entry._path);
    }
    for (const auto& entry : configuration._fields)
    {
      configured_value(registry.register_field(entry._descriptor), entry._path);
    }
    for (const auto& entry : configuration._messages)
    {
      configured_value(registry.register_message(entry._descriptor), entry._path);
    }
    for (const auto& entry : configuration._sources)
    {
      configured_value(registry.register_source_set(entry._descriptor), entry._path);
    }
    for (const auto& entry : configuration._queues)
    {
      configured_value(registry.register_queue(entry._descriptor), entry._path);
    }
    for (const auto& entry : configuration._publishing)
    {
      configured_value(registry.register_publishing_set(entry._descriptor), entry._path);
    }
    for (const auto& entry : configuration._sources)
    {
      const auto id = configured_value(registry.find_source_set(entry._descriptor._name), entry._path);
      if (!configured_value(registry.source_members(id), entry._path).empty())
      {
        configured_value(registry.activate_source_set(id), entry._path);
      }
    }
    for (const auto& entry : configuration._queues)
    {
      const auto id = configured_value(registry.find_queue(entry._descriptor._name), entry._path);
      configured_value(registry.activate_queue(id), entry._path);
    }
    for (const auto& entry : configuration._publishing)
    {
      if (!entry._descriptor._destinations.empty())
      {
        const auto id = configured_value(registry.find_publishing_set(entry._descriptor._name), entry._path);
        configured_value(registry.activate_publishing_set(id), entry._path);
      }
    }
  }

  void match_configuration_options(const SessionOptions& requested, const SessionOptions& recorded)
  {
    match(requested._name == recorded._name, "session.name");
    match(requested._applicationVersion == recorded._applicationVersion, "session.application_version");
    match(requested._maxLogBytes == recorded._maxLogBytes, "session.max_log_bytes");
    match(requested._storage == recorded._storage, "session.storage");
    match(requested._segmentBytes == recorded._segmentBytes, "session.segment_bytes");
    const auto& left = requested._limits;
    const auto& right = recorded._limits;
    match(left._tasks == right._tasks, "limits.tasks");
    match(left._queues == right._queues, "limits.queues");
    match(left._sourceSets == right._sourceSets, "limits.source_sets");
    match(left._publishingSets == right._publishingSets, "limits.publishing_sets");
    match(left._membershipsPerSet == right._membershipsPerSet, "limits.memberships_per_set");
    match(left._fields == right._fields, "limits.fields");
    match(left._messageSchemas == right._messageSchemas, "limits.message_schemas");
    match(left._symbols == right._symbols, "limits.symbols");
    match(left._stringPoolBytes == right._stringPoolBytes, "limits.string_pool_bytes");
    const auto& defaults = requested._defaults;
    const auto& effective = recorded._defaults;
    match(defaults._ringCapacity == effective._ringCapacity, "defaults.queue.ring_capacity");
    match(defaults._delivery == effective._delivery, "defaults.queue.delivery");
    match(defaults._selection == effective._selection, "defaults.source_set.selection");
    match(defaults._wait == effective._wait, "defaults.source_set.wait");
    match(defaults._acceptance == effective._acceptance, "defaults.publishing_set.acceptance");
    // The file locates the actual directory. Its old persisted spelling remains
    // diagnostic, so moving a complete Session does not invalidate attachment.
  }
} // namespace angaria::detail
