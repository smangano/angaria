#include "angaria/payload.h"

#include "angaria/registry.h"
#include "runtime_state.h"
#include "schema_metadata.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace angaria
{
  namespace
  {
    Error payload_error()
    {
      try
      {
        throw;
      }
      catch (const std::bad_alloc&)
      {
        return Error{ErrorCode::CapacityExceeded, "Insufficient memory for payload values or metadata", "payload.memory"};
      }
      catch (const std::length_error&)
      {
        return Error{ErrorCode::CapacityExceeded, "Payload values or metadata exceed container capacity", "payload.memory"};
      }
      catch (...)
      {
        return detail::runtime_caught_error();
      }
    }

    template <typename Float>
    void check_float_representation()
    {
      constexpr auto width = std::is_same_v<Float, float> ? 4u : 8u;
      if constexpr (sizeof(Float) != width || !std::numeric_limits<Float>::is_iec559)
      {
        detail::runtime_fail(ErrorCode::UnsupportedPolicy, "payload.representation",
                             "Binary floating-point payloads require IEEE 754 binary32/binary64");
      }
    }

    bool matches(EFieldBase base, const ScalarValue& value)
    {
      switch (base)
      {
      case EFieldBase::BASE_CHAR:
        return std::holds_alternative<char>(value);
      case EFieldBase::BASE_BYTE:
        return std::holds_alternative<std::uint8_t>(value);
      case EFieldBase::BASE_INT16:
        return std::holds_alternative<std::int16_t>(value);
      case EFieldBase::BASE_UINT16:
        return std::holds_alternative<std::uint16_t>(value);
      case EFieldBase::BASE_INT32:
        return std::holds_alternative<std::int32_t>(value);
      case EFieldBase::BASE_UINT32:
        return std::holds_alternative<std::uint32_t>(value);
      case EFieldBase::BASE_INT64:
        return std::holds_alternative<std::int64_t>(value);
      case EFieldBase::BASE_UINT64:
        return std::holds_alternative<std::uint64_t>(value);
      case EFieldBase::BASE_FLOAT32:
        return std::holds_alternative<float>(value);
      case EFieldBase::BASE_FLOAT64:
        return std::holds_alternative<double>(value);
      default:
        return false;
      }
    }

    template <typename Unsigned>
    void write_integer(std::span<std::byte> output, std::size_t offset, Unsigned value)
    {
      static_assert(std::is_unsigned_v<Unsigned>);
      for (std::size_t byte = 0; byte < sizeof(Unsigned); ++byte)
      {
        output[offset + byte] = static_cast<std::byte>(value & 0xffu);
        if (byte + 1 != sizeof(Unsigned))
        {
          value = static_cast<Unsigned>(value >> 8);
        }
      }
    }

    template <typename Unsigned>
    Unsigned read_integer(std::span<const std::byte> input, std::size_t offset)
    {
      static_assert(std::is_unsigned_v<Unsigned>);
      std::uint64_t result{};
      for (std::size_t byte = 0; byte < sizeof(Unsigned); ++byte)
      {
        result |= std::uint64_t{std::to_integer<std::uint8_t>(input[offset + byte])} << (byte * 8);
      }
      return static_cast<Unsigned>(result);
    }

    template <typename Signed>
    Signed read_signed(std::span<const std::byte> input, std::size_t offset)
    {
      using Unsigned = std::make_unsigned_t<Signed>;
      const auto bits = read_integer<Unsigned>(input, offset);
      if (bits <= static_cast<Unsigned>(std::numeric_limits<Signed>::max()))
      {
        return static_cast<Signed>(bits);
      }
      // The wire representation is two's complement. This arithmetic avoids
      // converting an out-of-range unsigned integer to a signed native value.
      const auto complement = static_cast<Unsigned>(~bits);
      return static_cast<Signed>(-static_cast<Signed>(complement) - 1);
    }

    template <typename Float>
    void write_float(std::span<std::byte> output, std::size_t offset, Float value)
    {
      using Bits = std::conditional_t<std::is_same_v<Float, float>, std::uint32_t, std::uint64_t>;
      if constexpr (sizeof(Float) == sizeof(Bits) && std::numeric_limits<Float>::is_iec559)
      {
        write_integer(output, offset, std::bit_cast<Bits>(value));
      }
      else
      {
        check_float_representation<Float>();
      }
    }

    template <typename Float>
    Float read_float(std::span<const std::byte> input, std::size_t offset)
    {
      using Bits = std::conditional_t<std::is_same_v<Float, float>, std::uint32_t, std::uint64_t>;
      if constexpr (sizeof(Float) == sizeof(Bits) && std::numeric_limits<Float>::is_iec559)
      {
        return std::bit_cast<Float>(read_integer<Bits>(input, offset));
      }
      else
      {
        check_float_representation<Float>();
        return Float{};
      }
    }

    void write_scalar(std::span<std::byte> output, std::size_t offset, const ScalarValue& value)
    {
      std::visit([&](auto scalar)
      {
        using Type = decltype(scalar);
        if constexpr (std::is_same_v<Type, char>)
        {
          write_integer(output, offset, std::bit_cast<std::uint8_t>(scalar));
        }
        else if constexpr (std::is_integral_v<Type>)
        {
          write_integer(output, offset, static_cast<std::make_unsigned_t<Type>>(scalar));
        }
        else
        {
          write_float(output, offset, scalar);
        }
      }, value);
    }

    ScalarValue read_scalar(EFieldBase base, std::span<const std::byte> input, std::size_t offset)
    {
      switch (base)
      {
      case EFieldBase::BASE_CHAR:
        return ScalarValue{std::in_place_type<char>, std::bit_cast<char>(read_integer<std::uint8_t>(input, offset))};
      case EFieldBase::BASE_BYTE:
        return ScalarValue{std::in_place_type<std::uint8_t>, read_integer<std::uint8_t>(input, offset)};
      case EFieldBase::BASE_INT16:
        return ScalarValue{std::in_place_type<std::int16_t>, read_signed<std::int16_t>(input, offset)};
      case EFieldBase::BASE_UINT16:
        return ScalarValue{std::in_place_type<std::uint16_t>, read_integer<std::uint16_t>(input, offset)};
      case EFieldBase::BASE_INT32:
        return ScalarValue{std::in_place_type<std::int32_t>, read_signed<std::int32_t>(input, offset)};
      case EFieldBase::BASE_UINT32:
        return ScalarValue{std::in_place_type<std::uint32_t>, read_integer<std::uint32_t>(input, offset)};
      case EFieldBase::BASE_INT64:
        return ScalarValue{std::in_place_type<std::int64_t>, read_signed<std::int64_t>(input, offset)};
      case EFieldBase::BASE_UINT64:
        return ScalarValue{std::in_place_type<std::uint64_t>, read_integer<std::uint64_t>(input, offset)};
      case EFieldBase::BASE_FLOAT32:
        return ScalarValue{std::in_place_type<float>, read_float<float>(input, offset)};
      case EFieldBase::BASE_FLOAT64:
        return ScalarValue{std::in_place_type<double>, read_float<double>(input, offset)};
      default:
        detail::runtime_fail(ErrorCode::InvalidState, "payload.codec", "Codec contains an unsupported Field base");
      }
    }
  } // namespace

  BinaryPayloadCodec::BinaryPayloadCodec(SchemaId schema, std::vector<FieldDescriptor> fields,
                                           std::uint64_t payloadSize)
      : _schema(schema), _fields(std::move(fields)), _payloadSize(payloadSize)
  {
  }

  BinaryPayloadCodec::BinaryPayloadCodec(BinaryPayloadCodec&& other) noexcept
      : _schema(std::exchange(other._schema, {})), _fields(std::move(other._fields)),
        _payloadSize(std::exchange(other._payloadSize, 0))
  {
    other._fields.clear();
  }

  BinaryPayloadCodec& BinaryPayloadCodec::operator=(const BinaryPayloadCodec& other)
  {
    if (this != &other)
    {
      // Descriptor strings can allocate while copying. Commit only after the
      // complete schema, ordered metadata, and byte count have been copied.
      BinaryPayloadCodec copy{other};
      *this = std::move(copy);
    }
    return *this;
  }

  BinaryPayloadCodec& BinaryPayloadCodec::operator=(BinaryPayloadCodec&& other) noexcept
  {
    if (this != &other)
    {
      _schema = std::exchange(other._schema, {});
      _fields = std::move(other._fields);
      _payloadSize = std::exchange(other._payloadSize, 0);
      other._fields.clear();
    }
    return *this;
  }

  Result<BinaryPayloadCodec> BinaryPayloadCodec::from_registry(const Registry& registry, SchemaId schema,
                                                               std::uint64_t maxPayloadBytes)
  {
    try
    {
      const auto snapshot = detail::runtime_value(registry.snapshot());
      return from_snapshot(snapshot, schema, maxPayloadBytes);
    }
    catch (...)
    {
      return payload_error();
    }
  }

  Result<BinaryPayloadCodec> BinaryPayloadCodec::from_snapshot(const RegistrySnapshot& snapshot, SchemaId schema,
                                                               std::uint64_t maxPayloadBytes)
  {
    try
    {
      if (schema._type == MessageType::Invalid || schema._version == 0)
      {
        detail::runtime_fail(ErrorCode::InvalidArgument, "message.schema", "Message type and version must be nonzero");
      }
      const MessageDescriptor* selected{};
      for (const auto& message : snapshot._messages)
      {
        if (message._descriptor._schema == schema)
        {
          if (selected)
          {
            detail::runtime_fail(ErrorCode::InvalidArgument, "message.schema", "Snapshot has duplicate definitions for the exact schema");
          }
          selected = &message._descriptor;
        }
      }
      if (!selected)
      {
        detail::runtime_fail(ErrorCode::NotFound, "message.schema", "Exact message schema is not present in the snapshot");
      }
      detail::runtime_value(detail::mapped::validate_message_descriptor(*selected));
      for (const auto& message : snapshot._messages)
      {
        const auto& candidate = message._descriptor;
        if ((candidate._schema._type == schema._type) != (candidate._name == selected->_name))
        {
          detail::runtime_fail(ErrorCode::InvalidArgument, "message.schema", "Message type and name disagree within the selected schema family");
        }
      }

      std::map<FieldId, const FieldDescriptor*> referenced;
      for (const auto field : selected->_fields)
      {
        referenced.try_emplace(field, nullptr);
      }
      std::map<std::string_view, FieldId> names;
      for (const auto& field : snapshot._fields)
      {
        const auto& descriptor = field._descriptor;
        const auto needed = referenced.find(descriptor._id);
        if (needed == referenced.end())
        {
          continue;
        }
        if (needed->second)
        {
          detail::runtime_fail(ErrorCode::InvalidArgument, "message.fields", "Snapshot has duplicate definitions for a referenced Field ID");
        }
        detail::runtime_value(detail::mapped::validate_field_descriptor(descriptor));
        detail::runtime_validate_field_policy(descriptor, ErrorCode::InvalidArgument);
        if (!names.emplace(descriptor._name, descriptor._id).second)
        {
          detail::runtime_fail(ErrorCode::InvalidArgument, "field.name", "Referenced Field names must identify unique definitions");
        }
        if (descriptor._base == EFieldBase::BASE_FLOAT32)
        {
          check_float_representation<float>();
        }
        else if (descriptor._base == EFieldBase::BASE_FLOAT64)
        {
          check_float_representation<double>();
        }
        needed->second = &descriptor;
      }

      std::uint64_t size{};
      for (const auto id : selected->_fields)
      {
        const auto* field = referenced.at(id);
        if (!field)
        {
          detail::runtime_fail(ErrorCode::NotFound, "message.fields", "A referenced Field is absent from the snapshot");
        }
        if (field->_size > maxPayloadBytes - size ||
            field->_size > std::numeric_limits<std::uint64_t>::max() - size)
        {
          detail::runtime_fail(ErrorCode::CapacityExceeded, "payload.capacity", "Schema payload exceeds its configured byte limit");
        }
        size += field->_size;
      }
      if (size > std::numeric_limits<std::size_t>::max() || size > std::vector<std::byte>{}.max_size() ||
          selected->_fields.size() > std::vector<FieldDescriptor>{}.max_size())
      {
        detail::runtime_fail(ErrorCode::CapacityExceeded, "payload.capacity", "Schema payload or layout exceeds host container capacity");
      }
      // Validate the whole byte budget before duplicating the ordered metadata,
      // particularly when one registered Field appears many times in a schema.
      std::vector<FieldDescriptor> ordered;
      ordered.reserve(selected->_fields.size());
      for (const auto id : selected->_fields)
      {
        ordered.push_back(*referenced.at(id));
      }
      return BinaryPayloadCodec{schema, std::move(ordered), size};
    }
    catch (...)
    {
      return payload_error();
    }
  }

  SchemaId BinaryPayloadCodec::schema() const
  {
    return _schema;
  }

  std::uint64_t BinaryPayloadCodec::payload_size() const
  {
    return _payloadSize;
  }

  std::span<const FieldDescriptor> BinaryPayloadCodec::fields() const
  {
    return _fields;
  }

  void BinaryPayloadCodec::check_valid() const
  {
    if (_schema._type == MessageType::Invalid || _schema._version == 0)
    {
      detail::runtime_fail(ErrorCode::InvalidState, "payload.codec", "Codec has been moved from");
    }
  }

  Result<std::vector<std::byte>> BinaryPayloadCodec::encode(std::span<const ScalarValue> values) const
  {
    try
    {
      check_valid();
      if (values.size() != _fields.size())
      {
        detail::runtime_fail(ErrorCode::InvalidArgument, "payload.values", "Value count must equal the schema's ordered Field count");
      }
      for (std::size_t index = 0; index < values.size(); ++index)
      {
        if (!matches(_fields[index]._base, values[index]))
        {
          detail::runtime_fail(ErrorCode::InvalidArgument, "payload.values[" + std::to_string(index) + "]",
                               "Value alternative must exactly match the declared scalar Field base");
        }
      }
      std::vector<std::byte> result(static_cast<std::size_t>(_payloadSize));
      std::size_t offset{};
      for (std::size_t index = 0; index < values.size(); ++index)
      {
        write_scalar(result, offset, values[index]);
        offset += _fields[index]._size;
      }
      return result;
    }
    catch (...)
    {
      return payload_error();
    }
  }

  Result<std::vector<ScalarValue>> BinaryPayloadCodec::decode(std::span<const std::byte> payload) const
  {
    try
    {
      check_valid();
      if (payload.size() != _payloadSize)
      {
        detail::runtime_fail(ErrorCode::InvalidArgument, "payload.size", "Payload byte count must exactly match the schema");
      }
      std::vector<ScalarValue> result;
      result.reserve(_fields.size());
      std::size_t offset{};
      for (const auto& field : _fields)
      {
        result.push_back(read_scalar(field._base, payload, offset));
        offset += field._size;
      }
      return result;
    }
    catch (...)
    {
      return payload_error();
    }
  }
} // namespace angaria
