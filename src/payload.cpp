#include "angaria/payload.h"

#include "angaria/detail/byte_operations.h"
#include "angaria/detail/checked_arithmetic.h"

#include "angaria/registry.h"
#include "runtime_state.h"
#include "schema_metadata.h"

#include <algorithm>
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
    using angaria::detail::read_unsigned_le;
    using angaria::detail::signed_from_bits;
    using angaria::detail::write_unsigned_le;

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

    template <typename Value>
    bool integer_matches(EFieldBase base, const Value& value)
    {
      switch (base)
      {
      case EFieldBase::BASE_INT8:
        return std::holds_alternative<std::int8_t>(value);
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
      default:
        return false;
      }
    }

    bool scalar_matches(EFieldBase base, const PayloadValue& value)
    {
      switch (base)
      {
      case EFieldBase::BASE_CHAR:
        return std::holds_alternative<char>(value);
      case EFieldBase::BASE_FLOAT32:
        return std::holds_alternative<float>(value);
      case EFieldBase::BASE_FLOAT64:
        return std::holds_alternative<double>(value);
      default:
        return integer_matches(base, value);
      }
    }

    EnumerationNumber enumeration_number(const IntegerValue& value)
    {
      return std::visit([](auto integer) -> EnumerationNumber
      {
        if constexpr (std::is_signed_v<decltype(integer)>)
        {
          return static_cast<std::int64_t>(integer);
        }
        else
        {
          return static_cast<std::uint64_t>(integer);
        }
      }, value);
    }

    bool declared_enumerator(const FieldDescriptor& field, const IntegerValue& value)
    {
      const auto number = enumeration_number(value);
      const auto& entries = field._logical._enumerators;
      const auto entry = std::lower_bound(entries.begin(), entries.end(), number,
          [](const EnumeratorDescriptor& candidate, const EnumerationNumber& selected)
          {
            return candidate._value < selected;
          });
      return entry != entries.end() && entry->_value == number;
    }

    [[noreturn]] void invalid_value(const FieldDescriptor& field, std::size_t index, const char* message)
    {
      detail::runtime_fail(ErrorCode::InvalidArgument, "payload.values[" + std::to_string(index) + "]",
                           "Field " + field._name + ": " + message);
    }

    void validate_value(const FieldDescriptor& field, const PayloadValue& value, std::size_t index)
    {
      switch (field._logical._kind)
      {
      case FieldLogicalKind::None:
        if (!scalar_matches(field._base, value))
        {
          invalid_value(field, index, "Value alternative must exactly match the scalar storage base");
        }
        return;
      case FieldLogicalKind::Boolean:
        if (!std::holds_alternative<bool>(value))
        {
          invalid_value(field, index, "A Boolean requires an explicit bool value");
        }
        return;
      case FieldLogicalKind::Bits:
      {
        const auto* bits = std::get_if<BitFieldValue>(&value);
        if (!bits)
        {
          invalid_value(field, index, "A bit field requires BitFieldValue");
        }
        if (field._size < 8 && bits->_value >= (std::uint64_t{1} << (field._size * 8)))
        {
          invalid_value(field, index, "Bit-field value exceeds its declared width");
        }
        return;
      }
      case FieldLogicalKind::Enumeration:
      {
        const auto* selected = std::get_if<EnumValue>(&value);
        if (!selected || !integer_matches(field._base, selected->_value))
        {
          invalid_value(field, index, "An enum requires EnumValue with the exact underlying integer alternative");
        }
        if (!declared_enumerator(field, selected->_value))
        {
          invalid_value(field, index, "Enum number has no registered label");
        }
        return;
      }
      case FieldLogicalKind::FixedAscii:
      {
        const auto* text = std::get_if<FixedAsciiValue>(&value);
        if (!text || text->_text.size() != field._size)
        {
          invalid_value(field, index, "Fixed ASCII requires exactly the declared number of bytes");
        }
        for (const auto byte : text->_text)
        {
          if (static_cast<unsigned char>(byte) > 0x7f)
          {
            invalid_value(field, index, "Fixed ASCII contains a byte outside 00..7f");
          }
        }
        return;
      }
      }
      detail::runtime_fail(ErrorCode::InvalidState, "payload.codec", "Codec contains an unknown logical Field kind");
    }

    template <typename Signed>
    Signed read_signed(std::span<const std::byte> input, std::size_t offset)
    {
      using Unsigned = std::make_unsigned_t<Signed>;
      const auto bits = read_unsigned_le<Unsigned>(input, offset);
      return signed_from_bits<Signed>(bits);
    }

    template <typename Float>
    void write_float(std::span<std::byte> output, std::size_t offset, Float value)
    {
      using Bits = std::conditional_t<std::is_same_v<Float, float>, std::uint32_t, std::uint64_t>;
      if constexpr (sizeof(Float) == sizeof(Bits) && std::numeric_limits<Float>::is_iec559)
      {
        write_unsigned_le(output, offset, std::bit_cast<Bits>(value));
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
        return std::bit_cast<Float>(read_unsigned_le<Bits>(input, offset));
      }
      else
      {
        check_float_representation<Float>();
        return Float{};
      }
    }

    void write_scalar(std::span<std::byte> output, std::size_t offset, const PayloadValue& value)
    {
      std::visit([&](const auto& scalar)
      {
        using Type = std::remove_cvref_t<decltype(scalar)>;
        if constexpr (std::is_same_v<Type, char>)
        {
          write_unsigned_le(output, offset, std::bit_cast<std::uint8_t>(scalar));
        }
        else if constexpr (std::is_integral_v<Type> && !std::is_same_v<Type, bool>)
        {
          write_unsigned_le(output, offset, static_cast<std::make_unsigned_t<Type>>(scalar));
        }
        else if constexpr (std::is_floating_point_v<Type>)
        {
          write_float(output, offset, scalar);
        }
        else
        {
          detail::runtime_fail(ErrorCode::InvalidState, "payload.codec", "Logical value reached the scalar encoder");
        }
      }, value);
    }

    PayloadValue read_scalar(EFieldBase base, std::span<const std::byte> input, std::size_t offset)
    {
      switch (base)
      {
      case EFieldBase::BASE_CHAR:
        return ScalarValue{std::in_place_type<char>, std::bit_cast<char>(read_unsigned_le<std::uint8_t>(input, offset))};
      case EFieldBase::BASE_BYTE:
        return ScalarValue{std::in_place_type<std::uint8_t>, read_unsigned_le<std::uint8_t>(input, offset)};
      case EFieldBase::BASE_INT8:
        return ScalarValue{std::in_place_type<std::int8_t>, read_signed<std::int8_t>(input, offset)};
      case EFieldBase::BASE_INT16:
        return ScalarValue{std::in_place_type<std::int16_t>, read_signed<std::int16_t>(input, offset)};
      case EFieldBase::BASE_UINT16:
        return ScalarValue{std::in_place_type<std::uint16_t>, read_unsigned_le<std::uint16_t>(input, offset)};
      case EFieldBase::BASE_INT32:
        return ScalarValue{std::in_place_type<std::int32_t>, read_signed<std::int32_t>(input, offset)};
      case EFieldBase::BASE_UINT32:
        return ScalarValue{std::in_place_type<std::uint32_t>, read_unsigned_le<std::uint32_t>(input, offset)};
      case EFieldBase::BASE_INT64:
        return ScalarValue{std::in_place_type<std::int64_t>, read_signed<std::int64_t>(input, offset)};
      case EFieldBase::BASE_UINT64:
        return ScalarValue{std::in_place_type<std::uint64_t>, read_unsigned_le<std::uint64_t>(input, offset)};
      case EFieldBase::BASE_FLOAT32:
        return ScalarValue{std::in_place_type<float>, read_float<float>(input, offset)};
      case EFieldBase::BASE_FLOAT64:
        return ScalarValue{std::in_place_type<double>, read_float<double>(input, offset)};
      default:
        detail::runtime_fail(ErrorCode::InvalidState, "payload.codec", "Codec contains an unsupported Field base");
      }
    }

    void write_field(std::span<std::byte> output, std::size_t offset,
                       const FieldDescriptor& field, const PayloadValue& value)
    {
      switch (field._logical._kind)
      {
      case FieldLogicalKind::None:
        write_scalar(output, offset, value);
        return;
      case FieldLogicalKind::Boolean:
        output[offset] = std::get<bool>(value) ? std::byte{1} : std::byte{};
        return;
      case FieldLogicalKind::Bits:
      {
        const auto bits = std::get<BitFieldValue>(value)._value;
        write_unsigned_le(output, offset, bits, field._size);
        return;
      }
      case FieldLogicalKind::Enumeration:
        std::visit([&](auto integer)
        {
          using Unsigned = std::make_unsigned_t<decltype(integer)>;
          write_unsigned_le(output, offset, static_cast<Unsigned>(integer));
        }, std::get<EnumValue>(value)._value);
        return;
      case FieldLogicalKind::FixedAscii:
      {
        const auto& text = std::get<FixedAsciiValue>(value)._text;
        for (std::size_t byte = 0; byte < text.size(); ++byte)
        {
          output[offset + byte] = static_cast<std::byte>(static_cast<unsigned char>(text[byte]));
        }
        return;
      }
      }
      detail::runtime_fail(ErrorCode::InvalidState, "payload.codec", "Codec contains an unknown logical Field kind");
    }

    IntegerValue read_enum_integer(EFieldBase base, std::span<const std::byte> input, std::size_t offset)
    {
      switch (base)
      {
      case EFieldBase::BASE_INT8:
        return read_signed<std::int8_t>(input, offset);
      case EFieldBase::BASE_BYTE:
        return read_unsigned_le<std::uint8_t>(input, offset);
      case EFieldBase::BASE_INT16:
        return read_signed<std::int16_t>(input, offset);
      case EFieldBase::BASE_UINT16:
        return read_unsigned_le<std::uint16_t>(input, offset);
      case EFieldBase::BASE_INT32:
        return read_signed<std::int32_t>(input, offset);
      case EFieldBase::BASE_UINT32:
        return read_unsigned_le<std::uint32_t>(input, offset);
      case EFieldBase::BASE_INT64:
        return read_signed<std::int64_t>(input, offset);
      case EFieldBase::BASE_UINT64:
        return read_unsigned_le<std::uint64_t>(input, offset);
      default:
        detail::runtime_fail(ErrorCode::InvalidState, "payload.codec", "Codec contains invalid enum storage");
      }
    }

    PayloadValue read_field(const FieldDescriptor& field, std::span<const std::byte> input,
                             std::size_t offset, std::size_t index)
    {
      switch (field._logical._kind)
      {
      case FieldLogicalKind::None:
        return read_scalar(field._base, input, offset);
      case FieldLogicalKind::Boolean:
      {
        const auto byte = std::to_integer<unsigned>(input[offset]);
        if (byte > 1)
        {
          invalid_value(field, index, "Boolean wire byte must be 00 or 01");
        }
        return byte == 1;
      }
      case FieldLogicalKind::Bits:
      {
        return BitFieldValue{read_unsigned_le(input, offset, field._size)};
      }
      case FieldLogicalKind::Enumeration:
      {
        auto number = read_enum_integer(field._base, input, offset);
        if (!declared_enumerator(field, number))
        {
          invalid_value(field, index, "Enum wire number has no registered label");
        }
        return EnumValue{std::move(number)};
      }
      case FieldLogicalKind::FixedAscii:
      {
        for (std::size_t byte = 0; byte < field._size; ++byte)
        {
          if (std::to_integer<unsigned>(input[offset + byte]) > 0x7f)
          {
            invalid_value(field, index, "Fixed ASCII wire byte is outside 00..7f");
          }
        }
        return FixedAsciiValue{std::string{reinterpret_cast<const char*>(input.data() + offset), field._size}};
      }
      }
      detail::runtime_fail(ErrorCode::InvalidState, "payload.codec", "Codec contains an unknown logical Field kind");
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
        const auto next = detail::checked_add(size, field->_size);
        if (!next || *next > maxPayloadBytes)
        {
          detail::runtime_fail(ErrorCode::CapacityExceeded, "payload.capacity", "Schema payload exceeds its configured byte limit");
        }
        size = *next;
      }
      if (!detail::fits_size_t(size) || size > std::vector<std::byte>{}.max_size() ||
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
        ordered.push_back(detail::runtime_value(
            detail::mapped::canonicalize_field_descriptor(*referenced.at(id))));
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

  Result<std::vector<std::byte>> BinaryPayloadCodec::encode(std::span<const PayloadValue> values) const
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
        validate_value(_fields[index], values[index], index);
      }
      std::vector<std::byte> result(static_cast<std::size_t>(_payloadSize));
      std::size_t offset{};
      for (std::size_t index = 0; index < values.size(); ++index)
      {
        write_field(result, offset, _fields[index], values[index]);
        offset += _fields[index]._size;
      }
      return result;
    }
    catch (...)
    {
      return payload_error();
    }
  }

  Result<std::vector<std::byte>> BinaryPayloadCodec::encode(std::initializer_list<PayloadValue> values) const
  {
    return encode(std::span<const PayloadValue>{values.begin(), values.size()});
  }

  OperationResult BinaryPayloadCodec::encode_into(std::span<const PayloadValue> values,
                                                  std::span<std::byte> output) const
  {
    try
    {
      check_valid();
      if (output.size() != _payloadSize)
      {
        detail::runtime_fail(ErrorCode::InvalidArgument, "payload.output_size",
                             "Output byte count must exactly match the schema");
      }
      // Complete validation and temporary allocation before touching caller
      // storage. No failing later Field can leave an earlier prefix written.
      const auto encoded = detail::runtime_value(encode(values));
      std::copy(encoded.begin(), encoded.end(), output.begin());
      return std::monostate{};
    }
    catch (...)
    {
      return payload_error();
    }
  }

  Result<std::vector<PayloadValue>> BinaryPayloadCodec::decode(std::span<const std::byte> payload) const
  {
    try
    {
      check_valid();
      if (payload.size() != _payloadSize)
      {
        detail::runtime_fail(ErrorCode::InvalidArgument, "payload.size", "Payload byte count must exactly match the schema");
      }
      std::vector<PayloadValue> result;
      result.reserve(_fields.size());
      std::size_t offset{};
      for (std::size_t index = 0; index < _fields.size(); ++index)
      {
        const auto& field = _fields[index];
        result.push_back(read_field(field, payload, offset, index));
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
