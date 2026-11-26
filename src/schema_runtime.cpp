#include "runtime_state.h"

#include "angaria/detail/byte_operations.h"
#include "angaria/detail/field_representation.h"
#include "angaria/detail/identity_helpers.h"
#include "registry_access.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace angaria::detail
{
  namespace
  {
    struct DefinitionIndex
    {
      std::uint64_t _offset{};
      std::uint64_t _key{};
      std::uint64_t _reference{};
      std::uint64_t _revision{};
    };

    std::vector<DefinitionIndex> definition_indexes(RuntimeAccess& access, mapped::SectionKind kind)
    {
      const auto& limits = access._session->_controller->configuration()._limits;
      const auto count = kind == mapped::SectionKind::FieldIndex ? limits._fields : limits._messageSchemas;
      const auto begin = access.section_offset(kind);
      const auto words = access.read_words(begin, std::size_t{count} * 8);
      std::vector<DefinitionIndex> indexes;
      for (std::size_t slot = 0; slot < count; ++slot)
      {
        const auto row = std::span{words}.subspan(slot * 8, 8);
        if (row[0] == 0)
        {
          if (!all_zero(row))
          {
            runtime_fail(ErrorCode::IoError, "registry.index", "Empty definition index contains data");
          }
          continue;
        }
        if (row[1] == 0 || row[2] == 0 || row[2] > access._snapshot._revision ||
            !all_zero(std::span{row}.subspan(3)))
        {
          runtime_fail(ErrorCode::IoError, "registry.index", "Invalid definition reference, revision, or reserved data");
        }
        indexes.push_back(DefinitionIndex{begin + slot * mapped::index_entry_bytes, row[0], row[1], row[2]});
      }
      // These immutable entity kinds have no edit or deletion operation: every
      // retained definition must have exactly one index row. Inspect the cached
      // boundaries once, including definitions hidden by an all-zero index row.
      std::vector<std::uint64_t> references;
      references.reserve(indexes.size());
      for (const auto& index : indexes)
      {
        references.push_back(index._reference);
      }
      std::sort(references.begin(), references.end());
      const std::uint16_t recordKind = kind == mapped::SectionKind::FieldIndex ? 6 : 7;
      if (references != access.record_references(recordKind))
      {
        runtime_fail(ErrorCode::IoError, "registry.index", "Retained definitions and index references disagree");
      }
      return indexes;
    }

    std::map<TaskId, std::uint64_t> creators(RuntimeAccess& access)
    {
      std::map<TaskId, std::uint64_t> ids;
      for (const auto& task : runtime_tasks(access))
      {
        ids.emplace(task._definition._id, task._definition._registration._revision);
      }
      return ids;
    }

    void check_registration(const RegistrationMetadata& metadata, const DefinitionIndex& index,
                             const mapped::RecordEnvelope& envelope,
                             const std::map<TaskId, std::uint64_t>& actors)
    {
      const auto actor = actors.find(metadata._createdBy);
      if (metadata._revision != index._revision || envelope._registryRevision != index._revision ||
          actor == actors.end() || actor->second > metadata._revision)
      {
        runtime_fail(ErrorCode::IoError, "registry.metadata", "Definition, index, envelope, or creator attribution disagrees");
      }
    }
  } // namespace

  void runtime_validate_field_policy(const FieldDescriptor& descriptor, ErrorCode invalidCode)
  {
    std::uint32_t width{};
    if (const auto scalar = scalar_width(descriptor._base))
    {
      width = *scalar;
    }
    else
    {
      switch (descriptor._base)
      {
      case EFieldBase::BASE_CHARS:
        if (descriptor._logical._kind == FieldLogicalKind::FixedAscii)
        {
          width = descriptor._size;
          break;
        }
        runtime_fail(ErrorCode::UnsupportedPolicy, "field.base", "Untagged character arrays have no implemented logical representation");
      case EFieldBase::BASE_NONE:
      case EFieldBase::BASE_DATE:
      case EFieldBase::BASE_TIMESTAMP:
      case EFieldBase::BASE_YEARS:
      case EFieldBase::BASE_MONTHS:
      case EFieldBase::BASE_DAYS:
      case EFieldBase::BASE_HOURS:
      case EFieldBase::BASE_MINS:
      case EFieldBase::BASE_SECS:
      case EFieldBase::BASE_MILLISECS:
      case EFieldBase::BASE_MICROSECS:
      case EFieldBase::BASE_NANOSECS:
      case EFieldBase::BASE_BYTES:
      case EFieldBase::BASE_SYMBOL:
      case EFieldBase::BASE_STRING:
        runtime_fail(ErrorCode::UnsupportedPolicy, "field.base", "Field representation is not implemented by this native profile");
      default:
        runtime_fail(invalidCode, "field.base", "Unknown Field base type");
      }
    }
    if (descriptor._size != width)
    {
      runtime_fail(invalidCode, "field.size", "Declared Field size must match its fixed scalar wire width");
    }
  }

  std::vector<LoadedField> runtime_fields(RuntimeAccess& access)
  {
    const auto actors = creators(access);
    const auto indexes = definition_indexes(access, mapped::SectionKind::FieldIndex);
    std::set<FieldId> ids;
    std::set<std::string> names;
    std::vector<LoadedField> fields;
    for (const auto& index : indexes)
    {
      const auto& envelope = access.record_envelope(index._reference, 6);
      auto definition = runtime_value(mapped::decode_field_definition(
          access.immutable_record(index._reference, 6), index._reference));
      const auto& descriptor = definition._descriptor;
      if (index._key != static_cast<std::uint32_t>(descriptor._id) ||
          !ids.insert(descriptor._id).second || !names.insert(descriptor._name).second ||
          envelope._previous != 0)
      {
        runtime_fail(ErrorCode::IoError, "field.index", "Field identity, name, or immutable definition chain is inconsistent");
      }
      check_registration(definition._registration, index, envelope, actors);
      if (mapped::field_requires_basic_fields(descriptor) &&
          (access._session->_controller->configuration()._requiredFeatures & mapped::basic_fields_feature) == 0)
      {
        runtime_fail(ErrorCode::IoError, "field.feature", "Stored Field requires basic_fields_v1 in its Session profile");
      }
      runtime_validate_field_policy(descriptor, ErrorCode::IoError);
      fields.push_back(LoadedField{std::move(definition), index._reference, index._offset});
    }
    return fields;
  }

  std::vector<LoadedSchema> runtime_schemas(RuntimeAccess& access)
  {
    const auto fields = runtime_fields(access);
    return runtime_schemas(access, fields);
  }

  std::vector<LoadedSchema> runtime_schemas(RuntimeAccess& access, std::span<const LoadedField> fields)
  {
    const auto actors = creators(access);
    std::map<FieldId, const LoadedField*> fieldIds;
    for (const auto& field : fields)
    {
      fieldIds.emplace(field._definition._descriptor._id, &field);
    }
    const auto indexes = definition_indexes(access, mapped::SectionKind::SchemaIndex);
    std::set<std::uint64_t> keys;
    std::map<MessageType, std::string> typeNames;
    std::map<std::string, MessageType> nameTypes;
    std::vector<LoadedSchema> schemas;
    for (const auto& index : indexes)
    {
      const auto& envelope = access.record_envelope(index._reference, 7);
      auto definition = runtime_value(mapped::decode_schema_definition(
          access.immutable_record(index._reference, 7), index._reference));
      const auto& descriptor = definition._descriptor;
      const auto [typeName, newType] = typeNames.emplace(descriptor._schema._type, descriptor._name);
      const auto [nameType, newName] = nameTypes.emplace(descriptor._name, descriptor._schema._type);
      if (index._key != schema_key(descriptor._schema) || !keys.insert(index._key).second ||
          (!newType && typeName->second != descriptor._name) ||
          (!newName && nameType->second != descriptor._schema._type))
      {
        runtime_fail(ErrorCode::IoError, "message.index", "Schema identity or message type/name mapping is inconsistent");
      }
      check_registration(definition._registration, index, envelope, actors);
      for (const auto field : descriptor._fields)
      {
        const auto registered = fieldIds.find(field);
        if (registered == fieldIds.end() ||
            registered->second->_definition._registration._revision > definition._registration._revision)
        {
          runtime_fail(ErrorCode::IoError, "message.fields", "Stored schema references a Field absent at its registration");
        }
      }
      schemas.push_back(LoadedSchema{std::move(definition), index._reference, index._offset});
    }
    // Every immutable version remains indexed. Chain by registration order,
    // independent of allocation slots and caller-chosen numeric version order.
    std::vector<const LoadedSchema*> ordered;
    ordered.reserve(schemas.size());
    for (const auto& schema : schemas)
    {
      ordered.push_back(&schema);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right)
    {
      return left->_reference < right->_reference;
    });
    std::map<MessageType, std::uint64_t> previous;
    for (const auto* schema : ordered)
    {
      const auto type = schema->_definition._descriptor._schema._type;
      if (access.record_envelope(schema->_reference, 7)._previous != previous[type])
      {
        runtime_fail(ErrorCode::IoError, "message.previous", "Schema version chain does not follow its retained message type");
      }
      previous[type] = schema->_reference;
    }
    return schemas;
  }

  LoadedField runtime_field(RuntimeAccess& access, FieldId field)
  {
    if (!valid_field_id(field))
    {
      runtime_fail(ErrorCode::InvalidArgument, "field.id", "Field identity must have the 0xEE prefix");
    }
    auto fields = runtime_fields(access);
    for (auto& value : fields)
    {
      if (value._definition._descriptor._id == field)
      {
        return std::move(value);
      }
    }
    runtime_fail(ErrorCode::NotFound, "field.id", "Field identity is not registered");
  }

  LoadedSchema runtime_schema(RuntimeAccess& access, SchemaId schema)
  {
    if (schema._type == MessageType::Invalid || schema._version == 0)
    {
      runtime_fail(ErrorCode::InvalidArgument, "message.schema", "Message type and schema version must be nonzero");
    }
    auto schemas = runtime_schemas(access);
    for (auto& value : schemas)
    {
      if (value._definition._descriptor._schema == schema)
      {
        return std::move(value);
      }
    }
    runtime_fail(ErrorCode::NotFound, "message.schema", "Schema identity is not registered");
  }
} // namespace angaria::detail

namespace angaria
{
  Result<FieldId> Registry::find_field(std::string_view name) const
  {
    try
    {
      auto access = detail::registry_access(_state, _actor);
      detail::runtime_value(detail::mapped::validate_metadata_text(name, true, true, "field.name"));
      for (const auto& field : detail::runtime_fields(access))
      {
        if (field._definition._descriptor._name == name)
        {
          return field._definition._descriptor._id;
        }
      }
      detail::runtime_fail(ErrorCode::NotFound, "field.name", "Field name is not registered");
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<SchemaId> Registry::find_message(std::string_view name, std::uint32_t version) const
  {
    try
    {
      auto access = detail::registry_access(_state, _actor);
      detail::runtime_value(detail::mapped::validate_metadata_text(name, true, true, "message.name"));
      if (version == 0)
      {
        detail::runtime_fail(ErrorCode::InvalidArgument, "message.version", "Schema version must be nonzero");
      }
      for (const auto& schema : detail::runtime_schemas(access))
      {
        const auto& descriptor = schema._definition._descriptor;
        if (descriptor._name == name && descriptor._schema._version == version)
        {
          return descriptor._schema;
        }
      }
      detail::runtime_fail(ErrorCode::NotFound, "message.name", "Message name/version is not registered");
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<FieldDescriptor> Registry::describe_field(FieldId id) const
  {
    try
    {
      auto access = detail::registry_access(_state, _actor);
      return detail::runtime_field(access, id)._definition._descriptor;
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<MessageDescriptor> Registry::describe_message(SchemaId id) const
  {
    try
    {
      auto access = detail::registry_access(_state, _actor);
      return detail::runtime_schema(access, id)._definition._descriptor;
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<RegistrationMetadata> Registry::metadata(FieldId id) const
  {
    try
    {
      auto access = detail::registry_access(_state, _actor);
      return detail::runtime_field(access, id)._definition._registration;
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }

  Result<RegistrationMetadata> Registry::metadata(SchemaId id) const
  {
    try
    {
      auto access = detail::registry_access(_state, _actor);
      return detail::runtime_schema(access, id)._definition._registration;
    }
    catch (...)
    {
      return detail::runtime_caught_error();
    }
  }
} // namespace angaria
