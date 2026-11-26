#include "runtime_state.h"

#include "schema_metadata.h"

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
#include <string_view>
#include <utility>
#include <vector>

namespace angaria::detail
{
  namespace
  {
    void check_registry_actor(const std::shared_ptr<SessionState>& session,
                               const std::shared_ptr<TaskState>& actor)
    {
      if (!session || !session->_controller)
      {
        runtime_fail(ErrorCode::InvalidState, "session", "Registry has no live Session state");
      }
      // Check inherited native ownership before entering any process-local
      // mutex, including for Session views that cannot mutate the Registry.
      runtime_value(session->_controller->check_owner());
      if (!actor)
      {
        runtime_fail(ErrorCode::ReadOnlyRegistry, "registry", "Registration requires an actor-bound Registry");
      }
      runtime_check_task_owner(actor);
    }

    bool same_descriptor(const FieldDescriptor& left, const FieldDescriptor& right)
    {
      if (left._id != right._id || left._name != right._name || left._base != right._base ||
          left._size != right._size || left._doc != right._doc ||
          left._logical._kind != right._logical._kind ||
          left._logical._enumerators.size() != right._logical._enumerators.size())
      {
        return false;
      }
      if (left._logical == right._logical)
      {
        return true;
      }
      // Registration order has no enum meaning. Borrow labels while comparing
      // the canonical stored mapping, so exact retries need no definition copy
      // and remain possible when the log has no room for another record.
      std::map<EnumerationNumber, std::string_view> requested;
      for (const auto& entry : right._logical._enumerators)
      {
        requested.emplace(entry._value, entry._name);
      }
      for (const auto& entry : left._logical._enumerators)
      {
        const auto found = requested.find(entry._value);
        if (found == requested.end() || found->second != entry._name)
        {
          return false;
        }
      }
      return true;
    }

    bool same_descriptor(const MessageDescriptor& left, const MessageDescriptor& right)
    {
      return left._schema == right._schema && left._name == right._name &&
             left._fields == right._fields && left._doc == right._doc;
    }

    std::uint64_t add_bytes(std::uint64_t left, std::uint64_t right)
    {
      const auto result = mapped::checked_add(left, right);
      if (!result || *result > mapped::max_log_bytes)
      {
        runtime_fail(ErrorCode::LogFull, "registry.definition", "Definition exceeds format byte bounds");
      }
      return *result;
    }

    std::uint64_t record_bytes(std::uint64_t bodyBytes)
    {
      const auto alignedBody = mapped::checked_align(bodyBytes, mapped::record_alignment);
      const auto required = alignedBody ? mapped::checked_add(mapped::record_envelope_bytes, *alignedBody) :
                                         std::nullopt;
      const auto total = required ? mapped::checked_align(*required, mapped::section_alignment) : std::nullopt;
      if (!total || *total > mapped::max_log_bytes)
      {
        runtime_fail(ErrorCode::LogFull, "registry.definition", "Definition allocation exceeds format byte bounds");
      }
      return *total;
    }

    template <typename Loaded>
    std::uint64_t allocate_index(RuntimeAccess& access, mapped::SectionKind kind,
                                  std::uint64_t capacity, std::span<const Loaded> entries,
                                  const char* path)
    {
      if (entries.size() >= capacity)
      {
        runtime_fail(ErrorCode::CapacityExceeded, path, "Session has no unused definition capacity");
      }
      // IDs and schema versions are application values, not array positions.
      // Find an empty allocation slot from validated index offsets instead.
      std::set<std::uint64_t> occupied;
      for (const auto& entry : entries)
      {
        occupied.insert(entry._indexOffset);
      }
      auto offset = access.section_offset(kind);
      while (occupied.contains(offset))
      {
        offset += mapped::index_entry_bytes;
      }
      return offset;
    }

    mapped::TransactionPlan append_plan(RuntimeAccess& access, const std::shared_ptr<TaskState>& actor,
                                         std::uint16_t kind, std::uint64_t key,
                                         std::uint64_t indexOffset, std::uint64_t previous,
                                         std::uint64_t bodyBytes)
    {
      if (access._snapshot._revision == std::numeric_limits<std::uint64_t>::max())
      {
        runtime_fail(ErrorCode::CapacityExceeded, "registry.revision", "Registry revision is exhausted");
      }
      if (access._snapshot._generation == std::numeric_limits<std::uint64_t>::max())
      {
        runtime_fail(ErrorCode::CapacityExceeded, "journal.generation", "Operation generation is exhausted");
      }
      const auto revision = access._snapshot._revision + 1;
      const auto manifestOffset = add_bytes(access._snapshot._tail, record_bytes(bodyBytes));
      const std::array<std::uint64_t, 4> row{key, access._snapshot._tail, revision, 0};
      mapped::Manifest manifest;
      manifest._actor = actor->_claim._task;
      manifest._actorIncarnation = actor->_claim._incarnation;
      manifest._baseRevision = access._snapshot._revision;
      manifest._newRevision = revision;
      manifest._records = {access._snapshot._tail};
      manifest._commands.push_back(runtime_write_words(indexOffset, row));
      const auto commands = runtime_value(mapped::encode_commands(manifest._commands));
      // Budget the full manifest, one record reference, command stream, and
      // record padding before copying names or a potentially large Field list.
      const auto manifestBodyBytes = add_bytes(64 + 8, commands.size());
      const auto tail = add_bytes(manifestOffset, record_bytes(manifestBodyBytes));
      if (tail > access._session->_controller->configuration()._maxLogBytes)
      {
        runtime_fail(ErrorCode::LogFull, "session.max_log_bytes", "Complete definition and manifest exceed the log byte ceiling");
      }
      runtime_value(mapped::encode_manifest_body(manifest, manifestOffset));

      mapped::TransactionPlan plan;
      plan._base = access._snapshot;
      plan._kind = mapped::JournalKind::Append;
      plan._actor = manifest._actor;
      plan._actorIncarnation = manifest._actorIncarnation;
      plan._newRevision = revision;
      plan._commands = std::move(manifest._commands);
      mapped::JournalRecord record;
      record._kind = kind;
      record._previous = previous;
      plan._records.push_back(std::move(record));
      return plan;
    }

    RegistrationMetadata registration(const mapped::TransactionPlan& plan)
    {
      return RegistrationMetadata{plan._actor,
          UtcTimestamp{std::chrono::nanoseconds{runtime_now_ns()}}, plan._newRevision};
    }

    std::uint64_t schema_key(SchemaId schema)
    {
      return (std::uint64_t{static_cast<std::uint16_t>(schema._type)} << 32) | schema._version;
    }
  } // namespace

  Result<FieldId> runtime_register_field(std::shared_ptr<SessionState> session,
                                         std::shared_ptr<TaskState> actor,
                                         const FieldDescriptor& descriptor,
                                         mapped::JournalFaultInjector* injector)
  {
    try
    {
      check_registry_actor(session, actor);
      RuntimeAccess access(std::move(session), true, injector);
      runtime_check_task(access, actor);
      runtime_value(mapped::validate_field_descriptor(descriptor));
      runtime_validate_field_policy(descriptor, ErrorCode::InvalidArgument);
      if (mapped::field_requires_basic_fields(descriptor) &&
          (access._session->_controller->configuration()._requiredFeatures & mapped::basic_fields_feature) == 0)
      {
        runtime_fail(ErrorCode::UnsupportedPolicy, "field.feature", "This Session predates basic_fields_v1; create a new Session for logical Fields or int8");
      }
      const auto fields = runtime_fields(access);
      for (const auto& field : fields)
      {
        const auto& registered = field._definition._descriptor;
        if (registered._id == descriptor._id || registered._name == descriptor._name)
        {
          if (!same_descriptor(registered, descriptor))
          {
            runtime_fail(ErrorCode::Conflict, "field.identity", "Field ID or name already has a different registered descriptor");
          }
          // No-op retries keep the original attribution and remain possible
          // when another definition or an exhausted log now prevents growth.
          return registered._id;
        }
      }
      const auto index = allocate_index(access, mapped::SectionKind::FieldIndex,
          access._session->_controller->configuration()._limits._fields,
          std::span<const LoadedField>{fields}, "limits.fields");
      const auto bodyBytes = runtime_value(mapped::field_definition_body_bytes(descriptor));
      auto plan = append_plan(access, actor, 6, static_cast<std::uint32_t>(descriptor._id), index, 0, bodyBytes);
      mapped::FieldDefinition definition{runtime_value(mapped::canonicalize_field_descriptor(descriptor)), registration(plan)};
      plan._records.front()._immutable = runtime_value(mapped::encode_field_definition(definition, plan._base._tail));
      runtime_value(access._journal.execute(access._guard, plan));
      return descriptor._id;
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }

  Result<SchemaId> runtime_register_message(std::shared_ptr<SessionState> session,
                                            std::shared_ptr<TaskState> actor,
                                            const MessageDescriptor& descriptor,
                                            mapped::JournalFaultInjector* injector)
  {
    try
    {
      check_registry_actor(session, actor);
      RuntimeAccess access(std::move(session), true, injector);
      runtime_check_task(access, actor);
      runtime_value(mapped::validate_message_descriptor(descriptor));
      const auto fields = runtime_fields(access);
      const auto schemas = runtime_schemas(access, fields);
      std::uint64_t previous{};
      std::uint64_t previousRevision{};
      for (const auto& schema : schemas)
      {
        const auto& registered = schema._definition._descriptor;
        if (registered._schema == descriptor._schema)
        {
          if (!same_descriptor(registered, descriptor))
          {
            runtime_fail(ErrorCode::SchemaConflict, "message.schema", "Message schema identity already has a different descriptor");
          }
          return registered._schema;
        }
        if ((registered._schema._type == descriptor._schema._type) != (registered._name == descriptor._name))
        {
          runtime_fail(ErrorCode::SchemaConflict, "message.identity", "Message type and name must identify the same schema family");
        }
        if (registered._schema._type == descriptor._schema._type &&
            (schema._definition._registration._revision > previousRevision ||
             (schema._definition._registration._revision == previousRevision && schema._reference > previous)))
        {
          previous = schema._reference;
          previousRevision = schema._definition._registration._revision;
        }
      }

      std::set<FieldId> knownFields;
      for (const auto& field : fields)
      {
        knownFields.insert(field._definition._descriptor._id);
      }
      for (const auto field : descriptor._fields)
      {
        if (!knownFields.contains(field))
        {
          runtime_fail(ErrorCode::NotFound, "message.fields", "Message schema references an unregistered Field");
        }
      }
      const auto index = allocate_index(access, mapped::SectionKind::SchemaIndex,
          access._session->_controller->configuration()._limits._messageSchemas,
          std::span<const LoadedSchema>{schemas}, "limits.message_schemas");
      const auto arrayBytes = mapped::checked_multiply(descriptor._fields.size(), 4);
      if (!arrayBytes)
      {
        runtime_fail(ErrorCode::LogFull, "message.fields", "Message Field array exceeds format byte bounds");
      }
      const auto bodyBytes = add_bytes(add_bytes(add_bytes(88, *arrayBytes), descriptor._name.size()), descriptor._doc.size());
      auto plan = append_plan(access, actor, 7, schema_key(descriptor._schema), index, previous, bodyBytes);
      mapped::SchemaDefinition definition{descriptor, registration(plan)};
      plan._records.front()._immutable = runtime_value(mapped::encode_schema_definition(definition, plan._base._tail));
      runtime_value(access._journal.execute(access._guard, plan));
      return descriptor._schema;
    }
    catch (...)
    {
      return runtime_caught_error();
    }
  }
} // namespace angaria::detail

namespace angaria
{
  Result<FieldId> Registry::register_field(const FieldDescriptor& descriptor)
  {
    return detail::runtime_register_field(_state, _actor, descriptor);
  }

  Result<SchemaId> Registry::register_message(const MessageDescriptor& descriptor)
  {
    return detail::runtime_register_message(_state, _actor, descriptor);
  }
} // namespace angaria
