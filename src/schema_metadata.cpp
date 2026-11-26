#include "schema_metadata.h"

#include "session_metadata.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <limits>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace angaria::detail::mapped
{
  namespace
  {
    constexpr std::size_t fixed_bytes = 88;

    struct CodecFailure
    {
      Error _error;
    };

    [[noreturn]] void fail(ErrorCode code, std::string_view path, std::string_view message)
    {
      throw CodecFailure{Error{code, std::string{message}, std::string{path}}};
    }

    void require(bool condition, ErrorCode code, std::string_view path, std::string_view message)
    {
      if (!condition)
      {
        fail(code, path, message);
      }
    }

    void take(OperationResult result)
    {
      if (auto* error = std::get_if<Error>(&result))
      {
        throw CodecFailure{std::move(*error)};
      }
    }

    template <typename T>
    T read_integer(std::span<const std::byte> bytes, std::size_t offset)
    {
      static_assert(std::is_unsigned_v<T> && sizeof(T) <= 8);
      std::uint64_t value{};
      for (std::size_t byte = 0; byte < sizeof(T); ++byte)
      {
        value |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + byte])} << (8 * byte);
      }
      return static_cast<T>(value);
    }

    template <typename T>
    void write_integer(std::span<std::byte> bytes, std::size_t offset, T value)
    {
      static_assert(std::is_unsigned_v<T> && sizeof(T) <= 8);
      for (std::size_t byte = 0; byte < sizeof(T); ++byte)
      {
        bytes[offset + byte] = static_cast<std::byte>((std::uint64_t{value} >> (8 * byte)) & 0xff);
      }
    }

    bool zero(std::span<const std::byte> bytes)
    {
      return std::all_of(bytes.begin(), bytes.end(), [](std::byte value)
      {
        return value == std::byte{};
      });
    }

    bool field_id(std::uint32_t value)
    {
      return (value & 0xff000000u) == 0xee000000u;
    }

    std::uint16_t base_code(EFieldBase base, ErrorCode code)
    {
      switch (base)
      {
      case EFieldBase::BASE_NONE: return 0;
      case EFieldBase::BASE_CHAR: return 1;
      case EFieldBase::BASE_BYTE: return 2;
      case EFieldBase::BASE_INT16: return 3;
      case EFieldBase::BASE_UINT16: return 4;
      case EFieldBase::BASE_INT32: return 5;
      case EFieldBase::BASE_UINT32: return 6;
      case EFieldBase::BASE_INT64: return 7;
      case EFieldBase::BASE_UINT64: return 8;
      case EFieldBase::BASE_FLOAT32: return 9;
      case EFieldBase::BASE_FLOAT64: return 10;
      case EFieldBase::BASE_DATE: return 11;
      case EFieldBase::BASE_TIMESTAMP: return 12;
      case EFieldBase::BASE_YEARS: return 13;
      case EFieldBase::BASE_MONTHS: return 14;
      case EFieldBase::BASE_DAYS: return 15;
      case EFieldBase::BASE_HOURS: return 16;
      case EFieldBase::BASE_MINS: return 17;
      case EFieldBase::BASE_SECS: return 18;
      case EFieldBase::BASE_MILLISECS: return 19;
      case EFieldBase::BASE_MICROSECS: return 20;
      case EFieldBase::BASE_NANOSECS: return 21;
      case EFieldBase::BASE_CHARS: return 22;
      case EFieldBase::BASE_BYTES: return 23;
      case EFieldBase::BASE_SYMBOL: return 24;
      case EFieldBase::BASE_STRING: return 25;
      case EFieldBase::BASE_INT8: return 26;
      }
      fail(code, "field.base", "Unknown Field base type");
    }

    EFieldBase decode_base(std::uint16_t code)
    {
      constexpr std::array values{
          EFieldBase::BASE_NONE, EFieldBase::BASE_CHAR, EFieldBase::BASE_BYTE,
          EFieldBase::BASE_INT16, EFieldBase::BASE_UINT16, EFieldBase::BASE_INT32,
          EFieldBase::BASE_UINT32, EFieldBase::BASE_INT64, EFieldBase::BASE_UINT64,
          EFieldBase::BASE_FLOAT32, EFieldBase::BASE_FLOAT64, EFieldBase::BASE_DATE,
          EFieldBase::BASE_TIMESTAMP, EFieldBase::BASE_YEARS, EFieldBase::BASE_MONTHS,
          EFieldBase::BASE_DAYS, EFieldBase::BASE_HOURS, EFieldBase::BASE_MINS,
          EFieldBase::BASE_SECS, EFieldBase::BASE_MILLISECS, EFieldBase::BASE_MICROSECS,
          EFieldBase::BASE_NANOSECS, EFieldBase::BASE_CHARS, EFieldBase::BASE_BYTES,
          EFieldBase::BASE_SYMBOL, EFieldBase::BASE_STRING, EFieldBase::BASE_INT8};
      require(code < values.size(), ErrorCode::IoError, "field.base", "Unknown stored Field base code");
      return values[code];
    }

    struct IntegerShape
    {
      bool _signed{};
      unsigned _bytes{};
    };

    std::optional<IntegerShape> integer_shape(EFieldBase base)
    {
      switch (base)
      {
      case EFieldBase::BASE_INT8: return IntegerShape{true, 1};
      case EFieldBase::BASE_BYTE: return IntegerShape{false, 1};
      case EFieldBase::BASE_INT16: return IntegerShape{true, 2};
      case EFieldBase::BASE_UINT16: return IntegerShape{false, 2};
      case EFieldBase::BASE_INT32: return IntegerShape{true, 4};
      case EFieldBase::BASE_UINT32: return IntegerShape{false, 4};
      case EFieldBase::BASE_INT64: return IntegerShape{true, 8};
      case EFieldBase::BASE_UINT64: return IntegerShape{false, 8};
      default: return std::nullopt;
      }
    }

    void check_number(const EnumerationNumber& number, IntegerShape shape, ErrorCode code,
                       std::string_view path)
    {
      if (shape._signed)
      {
        const auto* value = std::get_if<std::int64_t>(&number);
        require(value != nullptr, code, path, "Enumerator value must match the signed storage base");
        const auto minimum = shape._bytes == 8 ? std::numeric_limits<std::int64_t>::min() :
            -(std::int64_t{1} << (shape._bytes * 8 - 1));
        const auto maximum = shape._bytes == 8 ? std::numeric_limits<std::int64_t>::max() :
            (std::int64_t{1} << (shape._bytes * 8 - 1)) - 1;
        require(*value >= minimum && *value <= maximum, code, path,
                "Enumerator value exceeds the signed storage width");
      }
      else
      {
        const auto* value = std::get_if<std::uint64_t>(&number);
        require(value != nullptr, code, path, "Enumerator value must match the unsigned storage base");
        const auto maximum = shape._bytes == 8 ? std::numeric_limits<std::uint64_t>::max() :
            (std::uint64_t{1} << (shape._bytes * 8)) - 1;
        require(*value <= maximum, code, path, "Enumerator value exceeds the unsigned storage width");
      }
    }

    std::uint16_t logical_code(FieldLogicalKind kind, ErrorCode code)
    {
      switch (kind)
      {
      case FieldLogicalKind::None: return 0;
      case FieldLogicalKind::Boolean: return 1;
      case FieldLogicalKind::Bits: return 2;
      case FieldLogicalKind::Enumeration: return 3;
      case FieldLogicalKind::FixedAscii: return 4;
      }
      fail(code, "field.logical.kind", "Unknown logical Field kind");
    }

    void check_logical(const FieldDescriptor& descriptor, ErrorCode code)
    {
      const auto& logical = descriptor._logical;
      static_cast<void>(logical_code(logical._kind, code));
      if (logical._kind != FieldLogicalKind::Enumeration)
      {
        require(logical._enumerators.empty(), code, "field.enumerators",
                "Only an enumeration Field can contain enumerator entries");
      }
      switch (logical._kind)
      {
      case FieldLogicalKind::None:
        if (descriptor._base == EFieldBase::BASE_INT8)
        {
          require(descriptor._size == 1, code, "field.size", "An int8 Field occupies exactly one byte");
        }
        return;
      case FieldLogicalKind::Boolean:
        require(descriptor._base == EFieldBase::BASE_BYTE && descriptor._size == 1,
                code, "field.logical", "A Boolean requires byte storage and size one");
        return;
      case FieldLogicalKind::Bits:
      {
        const auto shape = integer_shape(descriptor._base);
        require(shape && !shape->_signed && descriptor._size == shape->_bytes,
                code, "field.logical", "A bit Field requires unsigned 8/16/32/64-bit storage of the exact width");
        return;
      }
      case FieldLogicalKind::FixedAscii:
        require(descriptor._base == EFieldBase::BASE_CHARS && descriptor._size != 0,
                code, "field.logical", "A fixed ASCII Field requires chars storage and a positive byte size");
        return;
      case FieldLogicalKind::Enumeration:
        break;
      }
      const auto shape = integer_shape(descriptor._base);
      require(shape && descriptor._size == shape->_bytes, code, "field.logical",
              "An enumeration requires integer storage of the exact width");
      require(!logical._enumerators.empty(), code, "field.enumerators", "An enumeration requires at least one entry");
      std::set<std::string_view> names;
      std::set<EnumerationNumber> numbers;
      for (std::size_t index = 0; index < logical._enumerators.size(); ++index)
      {
        const auto& entry = logical._enumerators[index];
        const auto path = "field.enumerators[" + std::to_string(index) + "]";
        const auto namePath = path + ".name";
        take(validate_metadata_text(entry._name, true, true, namePath.c_str(), code));
        check_number(entry._value, *shape, code, path + ".value");
        require(names.insert(entry._name).second, code, namePath, "Enumerator labels must be unique");
        require(numbers.insert(entry._value).second, code, path + ".value", "Enumerator numeric values must be unique");
      }
    }

    void check_field(const FieldDescriptor& descriptor, ErrorCode code)
    {
      require(field_id(static_cast<std::uint32_t>(descriptor._id)), code, "field.id",
              "Field identity must have the 0xEE high byte");
      take(validate_metadata_text(descriptor._name, true, true, "field.name", code));
      take(validate_metadata_text(descriptor._doc, false, false, "field.doc", code));
      static_cast<void>(base_code(descriptor._base, code));
      check_logical(descriptor, code);
    }

    std::uint64_t checked_sum(std::uint64_t left, std::uint64_t right, ErrorCode code)
    {
      const auto result = checked_add(left, right);
      require(result && *result <= max_log_bytes, code, "field.definition", "Field definition length overflows");
      return *result;
    }

    std::uint64_t attributes_bytes(const FieldDescriptor& descriptor, ErrorCode code)
    {
      if (descriptor._logical._kind == FieldLogicalKind::None)
      {
        return 0;
      }
      if (descriptor._logical._kind != FieldLogicalKind::Enumeration)
      {
        return 8;
      }
      const auto entryBytes = checked_multiply(descriptor._logical._enumerators.size(), 24);
      require(entryBytes.has_value(), code, "field.enumerators", "Enumerator entry array length overflows");
      auto bytes = checked_sum(16, *entryBytes, code);
      for (const auto& entry : descriptor._logical._enumerators)
      {
        bytes = checked_sum(bytes, entry._name.size(), code);
      }
      return bytes;
    }

    std::uint64_t field_body_bytes(const FieldDescriptor& descriptor, ErrorCode code)
    {
      auto bytes = checked_sum(fixed_bytes, attributes_bytes(descriptor, code), code);
      bytes = checked_sum(bytes, descriptor._name.size(), code);
      return checked_sum(bytes, descriptor._doc.size(), code);
    }

    void check_schema(const MessageDescriptor& descriptor, ErrorCode code)
    {
      require(descriptor._schema._type != MessageType::Invalid && descriptor._schema._version != 0,
              code, "message.schema", "Message type and schema version must be nonzero");
      take(validate_metadata_text(descriptor._name, true, true, "message.name", code));
      take(validate_metadata_text(descriptor._doc, false, false, "message.doc", code));
      for (const auto id : descriptor._fields)
      {
        require(field_id(static_cast<std::uint32_t>(id)), code, "message.fields",
                "Schema Field identities must retain the 0xEE high byte");
      }
    }

    void check_registration(const RegistrationMetadata& registration, ErrorCode code)
    {
      require(registration._createdBy != TaskId::Invalid && registration._revision != 0,
              code, "definition.registration", "Definition creator and revision must be nonzero");
    }

    std::uint64_t body_begin(std::uint64_t offset, std::uint64_t length, ErrorCode code)
    {
      require(offset >= superblock_bytes && offset % record_alignment == 0 &&
                  contains_range(max_log_bytes, offset, 64),
              code, "definition.offset", "Invalid definition record offset");
      const auto begin = offset + 64;
      require(contains_range(max_log_bytes, begin, length), code, "definition.offset",
              "Definition body exceeds format bounds");
      return begin;
    }

    std::size_t host_size(std::uint64_t value, ErrorCode code)
    {
      require(value <= std::numeric_limits<std::size_t>::max() &&
                  value <= static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()),
              code, "definition.length", "Definition length exceeds host bounds");
      return static_cast<std::size_t>(value);
    }

    std::uint64_t number_bits(const EnumerationNumber& number)
    {
      return std::visit([](auto value)
      {
        return static_cast<std::uint64_t>(value);
      }, number);
    }

    EnumerationNumber decode_number(std::uint64_t bits, IntegerShape shape, ErrorCode code,
                                      std::string_view path)
    {
      EnumerationNumber number;
      if (shape._signed)
      {
        const auto value = bits <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ?
            static_cast<std::int64_t>(bits) :
            -std::int64_t{1} - static_cast<std::int64_t>(std::numeric_limits<std::uint64_t>::max() - bits);
        number = value;
      }
      else
      {
        number = bits;
      }
      check_number(number, shape, code, path);
      return number;
    }

    void write_attributes(std::span<std::byte> bytes, const FieldDescriptor& descriptor,
                            std::uint64_t begin, std::uint64_t attributeBytes)
    {
      if (attributeBytes == 0)
      {
        return;
      }
      write_integer<std::uint16_t>(bytes, 66, 1);
      write_integer<std::uint64_t>(bytes, 72, begin + fixed_bytes);
      write_integer<std::uint64_t>(bytes, 80, attributeBytes);
      write_integer<std::uint16_t>(bytes, fixed_bytes, 1);
      write_integer<std::uint16_t>(bytes, fixed_bytes + 2,
          logical_code(descriptor._logical._kind, ErrorCode::InvalidArgument));
      if (descriptor._logical._kind != FieldLogicalKind::Enumeration)
      {
        return;
      }
      const auto& entries = descriptor._logical._enumerators;
      std::vector<const EnumeratorDescriptor*> ordered;
      ordered.reserve(entries.size());
      for (const auto& entry : entries)
      {
        ordered.push_back(&entry);
      }
      std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right)
      {
        return left->_value < right->_value;
      });
      write_integer<std::uint64_t>(bytes, fixed_bytes + 8, entries.size());
      auto cursor = fixed_bytes + 16 + entries.size() * 24;
      for (std::size_t index = 0; index < ordered.size(); ++index)
      {
        const auto& entry = *ordered[index];
        const auto entryOffset = fixed_bytes + 16 + index * 24;
        write_integer<std::uint64_t>(bytes, entryOffset, number_bits(entry._value));
        write_integer<std::uint64_t>(bytes, entryOffset + 8, begin + cursor);
        write_integer<std::uint64_t>(bytes, entryOffset + 16, entry._name.size());
        for (const auto character : entry._name)
        {
          bytes[cursor++] = static_cast<std::byte>(static_cast<unsigned char>(character));
        }
      }
    }

    std::size_t read_attributes(std::span<const std::byte> bytes, FieldDescriptor& descriptor,
                                 std::uint64_t begin)
    {
      const auto flags = read_integer<std::uint16_t>(bytes, 66);
      const auto reference = read_integer<std::uint64_t>(bytes, 72);
      const auto length = read_integer<std::uint64_t>(bytes, 80);
      if (flags == 0)
      {
        require(reference == 0 && length == 0, ErrorCode::IoError, "field.attributes",
                "An ordinary scalar Field requires an empty attributes slice");
        return fixed_bytes;
      }
      require(flags == 1, ErrorCode::IoError, "field.flags", "Unknown required Field flags");
      require(reference == begin + fixed_bytes && length >= 8 && length <= bytes.size() - fixed_bytes,
              ErrorCode::IoError, "field.attributes", "Logical attributes exceed their canonical owning body");
      const auto attributeEnd = fixed_bytes + host_size(length, ErrorCode::IoError);
      require(read_integer<std::uint16_t>(bytes, fixed_bytes) == 1,
              ErrorCode::IoError, "field.attributes.version", "Unknown required logical attribute encoding");
      require(zero(bytes.subspan(fixed_bytes + 4, 4)), ErrorCode::IoError, "field.attributes.reserved",
              "Logical attribute reserved bytes must be zero");
      switch (read_integer<std::uint16_t>(bytes, fixed_bytes + 2))
      {
      case 1: descriptor._logical._kind = FieldLogicalKind::Boolean; break;
      case 2: descriptor._logical._kind = FieldLogicalKind::Bits; break;
      case 3: descriptor._logical._kind = FieldLogicalKind::Enumeration; break;
      case 4: descriptor._logical._kind = FieldLogicalKind::FixedAscii; break;
      default: fail(ErrorCode::IoError, "field.logical.kind", "Unknown required logical Field kind");
      }
      if (descriptor._logical._kind != FieldLogicalKind::Enumeration)
      {
        require(length == 8, ErrorCode::IoError, "field.attributes", "Unexpected logical attribute trailing bytes");
        return attributeEnd;
      }
      require(length >= 16, ErrorCode::IoError, "field.enumerators", "Truncated enumeration header");
      const auto shape = integer_shape(descriptor._base);
      require(shape && descriptor._size == shape->_bytes, ErrorCode::IoError, "field.logical",
              "An enumeration requires integer storage of the exact width");
      const auto count = read_integer<std::uint64_t>(bytes, fixed_bytes + 8);
      const auto entryBytes = checked_multiply(count, 24);
      require(count != 0 && entryBytes && *entryBytes <= length - 16,
              ErrorCode::IoError, "field.enumerators", "Enumeration entry array exceeds its attributes");
      auto cursor = fixed_bytes + 16 + host_size(*entryBytes, ErrorCode::IoError);
      std::set<std::string_view> names;
      std::optional<EnumerationNumber> previous;
      // Validate the complete borrowed block before reserving an owning list or
      // copying labels. Entry counts cannot direct reads outside this record.
      for (std::size_t index = 0; index < count; ++index)
      {
        const auto entryOffset = fixed_bytes + 16 + index * 24;
        const auto path = "field.enumerators[" + std::to_string(index) + "]";
        const auto number = decode_number(read_integer<std::uint64_t>(bytes, entryOffset),
                                          *shape, ErrorCode::IoError, path + ".value");
        require(!previous || *previous < number, ErrorCode::IoError, path + ".value",
                "Stored enumerators must have distinct values in ascending numeric order");
        previous = number;
        const auto labelReference = read_integer<std::uint64_t>(bytes, entryOffset + 8);
        const auto labelLength = read_integer<std::uint64_t>(bytes, entryOffset + 16);
        require(labelLength != 0 && labelReference == begin + cursor && labelLength <= attributeEnd - cursor,
                ErrorCode::IoError, path + ".name", "Enumerator label is outside its canonical attributes block");
        const auto label = std::string_view{reinterpret_cast<const char*>(bytes.data() + cursor),
                                            static_cast<std::size_t>(labelLength)};
        const auto namePath = path + ".name";
        take(validate_metadata_text(label, true, true, namePath.c_str(), ErrorCode::IoError));
        require(names.insert(label).second, ErrorCode::IoError, namePath, "Enumerator labels must be unique");
        cursor += static_cast<std::size_t>(labelLength);
      }
      require(cursor == attributeEnd, ErrorCode::IoError, "field.attributes",
              "Unexpected enumeration attribute trailing bytes");
      auto& entries = descriptor._logical._enumerators;
      entries.reserve(host_size(count, ErrorCode::IoError));
      for (std::size_t index = 0; index < count; ++index)
      {
        const auto entryOffset = fixed_bytes + 16 + index * 24;
        const auto labelOffset = read_integer<std::uint64_t>(bytes, entryOffset + 8) - begin;
        const auto labelLength = read_integer<std::uint64_t>(bytes, entryOffset + 16);
        entries.push_back(EnumeratorDescriptor{
            std::string{reinterpret_cast<const char*>(bytes.data() + static_cast<std::size_t>(labelOffset)),
                          static_cast<std::size_t>(labelLength)},
            decode_number(read_integer<std::uint64_t>(bytes, entryOffset), *shape, ErrorCode::IoError,
                          "field.enumerators")});
      }
      return attributeEnd;
    }

    std::vector<std::byte> encode_text(std::uint64_t prefixBytes, std::string_view name,
                                       std::string_view doc, std::uint64_t offset)
    {
      const auto withName = checked_add(prefixBytes, name.size());
      const auto total = withName ? checked_add(*withName, doc.size()) : std::nullopt;
      require(total.has_value(), ErrorCode::InvalidArgument, "definition.length", "Definition length overflows");
      const auto length = host_size(*total, ErrorCode::InvalidArgument);
      const auto begin = body_begin(offset, *total, ErrorCode::InvalidArgument);
      std::vector<std::byte> bytes(length);
      auto cursor = host_size(prefixBytes, ErrorCode::InvalidArgument);
      const std::array<std::string_view, 2> strings{name, doc};
      for (std::size_t index = 0; index < strings.size(); ++index)
      {
        if (strings[index].empty())
        {
          continue;
        }
        write_integer<std::uint64_t>(bytes, 32 + index * 16, begin + cursor);
        write_integer<std::uint64_t>(bytes, 40 + index * 16, strings[index].size());
        for (const auto character : strings[index])
        {
          bytes[cursor++] = static_cast<std::byte>(static_cast<unsigned char>(character));
        }
      }
      return bytes;
    }

    std::array<std::string, 2> decode_text(std::span<const std::byte> bytes, std::size_t cursor,
                                            std::uint64_t begin, bool schema)
    {
      std::array<std::string_view, 2> strings;
      for (std::size_t index = 0; index < strings.size(); ++index)
      {
        const auto reference = read_integer<std::uint64_t>(bytes, 32 + index * 16);
        const auto length = read_integer<std::uint64_t>(bytes, 40 + index * 16);
        if (length == 0)
        {
          require(reference == 0, ErrorCode::IoError, "definition.strings", "Empty string slice has a nonzero offset");
          continue;
        }
        require(reference == begin + cursor && length <= bytes.size() - cursor,
                ErrorCode::IoError, "definition.strings", "String slice is outside its canonical owning body");
        strings[index] = std::string_view{reinterpret_cast<const char*>(bytes.data() + cursor),
                                          static_cast<std::size_t>(length)};
        cursor += static_cast<std::size_t>(length);
      }
      require(bytes.size() - cursor < section_alignment && zero(bytes.subspan(cursor)),
              ErrorCode::IoError, "definition.padding", "Unexpected definition allocation padding");
      take(validate_metadata_text(strings[0], true, true, schema ? "message.name" : "field.name", ErrorCode::IoError));
      take(validate_metadata_text(strings[1], false, false, schema ? "message.doc" : "field.doc", ErrorCode::IoError));
      return {std::string{strings[0]}, std::string{strings[1]}};
    }

    void write_common(std::span<std::byte> bytes, std::uint64_t key, const RegistrationMetadata& registration)
    {
      write_integer<std::uint64_t>(bytes, 0, key);
      write_integer<std::uint16_t>(bytes, 8, static_cast<std::uint16_t>(registration._createdBy));
      write_integer<std::uint64_t>(bytes, 16,
          static_cast<std::uint64_t>(registration._createdAt.time_since_epoch().count()));
      write_integer<std::uint64_t>(bytes, 24, registration._revision);
    }

    RegistrationMetadata read_common(std::span<const std::byte> bytes)
    {
      require(zero(bytes.subspan(10, 6)), ErrorCode::IoError, "definition.reserved",
              "Definition state/reserved bytes must be zero");
      RegistrationMetadata registration;
      registration._createdBy = static_cast<TaskId>(read_integer<std::uint16_t>(bytes, 8));
      registration._createdAt = UtcTimestamp{std::chrono::nanoseconds{
          std::bit_cast<std::int64_t>(read_integer<std::uint64_t>(bytes, 16))}};
      registration._revision = read_integer<std::uint64_t>(bytes, 24);
      check_registration(registration, ErrorCode::IoError);
      return registration;
    }
  } // namespace

  OperationResult validate_field_descriptor(const FieldDescriptor& descriptor)
  {
    try
    {
      check_field(descriptor, ErrorCode::InvalidArgument);
      static_cast<void>(host_size(field_body_bytes(descriptor, ErrorCode::InvalidArgument), ErrorCode::InvalidArgument));
      return std::monostate{};
    }
    catch (const CodecFailure& failure)
    {
      return failure._error;
    }
    catch (const std::bad_alloc&)
    {
      return Error{ErrorCode::CapacityExceeded, "Insufficient memory to validate Field metadata", "field.memory"};
    }
  }

  Result<FieldDescriptor> canonicalize_field_descriptor(const FieldDescriptor& descriptor)
  {
    try
    {
      check_field(descriptor, ErrorCode::InvalidArgument);
      static_cast<void>(host_size(field_body_bytes(descriptor, ErrorCode::InvalidArgument), ErrorCode::InvalidArgument));
      FieldDescriptor normalized = descriptor;
      auto& entries = normalized._logical._enumerators;
      std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right)
      {
        return left._value < right._value;
      });
      return normalized;
    }
    catch (const CodecFailure& failure)
    {
      return failure._error;
    }
    catch (const std::bad_alloc&)
    {
      return Error{ErrorCode::CapacityExceeded, "Insufficient memory to own Field metadata", "field.memory"};
    }
    catch (const std::length_error&)
    {
      return Error{ErrorCode::CapacityExceeded, "Field metadata exceeds a native container limit", "field.memory"};
    }
  }

  Result<std::uint64_t> field_definition_body_bytes(const FieldDescriptor& descriptor)
  {
    try
    {
      check_field(descriptor, ErrorCode::InvalidArgument);
      const auto bytes = field_body_bytes(descriptor, ErrorCode::InvalidArgument);
      static_cast<void>(host_size(bytes, ErrorCode::InvalidArgument));
      return bytes;
    }
    catch (const CodecFailure& failure)
    {
      return failure._error;
    }
    catch (const std::bad_alloc&)
    {
      return Error{ErrorCode::CapacityExceeded, "Insufficient memory to validate Field metadata", "field.memory"};
    }
  }

  bool field_requires_basic_fields(const FieldDescriptor& descriptor)
  {
    return descriptor._base == EFieldBase::BASE_INT8 || descriptor._logical._kind != FieldLogicalKind::None;
  }

  OperationResult validate_message_descriptor(const MessageDescriptor& descriptor)
  {
    try
    {
      check_schema(descriptor, ErrorCode::InvalidArgument);
      return std::monostate{};
    }
    catch (const CodecFailure& failure)
    {
      return failure._error;
    }
  }

  Result<std::vector<std::byte>> encode_field_definition(const FieldDefinition& definition,
                                                        std::uint64_t recordOffset)
  {
    try
    {
      check_field(definition._descriptor, ErrorCode::InvalidArgument);
      check_registration(definition._registration, ErrorCode::InvalidArgument);
      const auto bodyBytes = field_body_bytes(definition._descriptor, ErrorCode::InvalidArgument);
      static_cast<void>(host_size(bodyBytes, ErrorCode::InvalidArgument));
      const auto begin = body_begin(recordOffset, bodyBytes, ErrorCode::InvalidArgument);
      const auto attributeBytes = attributes_bytes(definition._descriptor, ErrorCode::InvalidArgument);
      auto bytes = encode_text(fixed_bytes + attributeBytes,
          definition._descriptor._name, definition._descriptor._doc, recordOffset);
      write_common(bytes, static_cast<std::uint32_t>(definition._descriptor._id), definition._registration);
      write_integer<std::uint16_t>(bytes, 64, base_code(definition._descriptor._base, ErrorCode::InvalidArgument));
      write_integer<std::uint32_t>(bytes, 68, definition._descriptor._size);
      write_attributes(bytes, definition._descriptor, begin, attributeBytes);
      return bytes;
    }
    catch (const CodecFailure& failure)
    {
      return failure._error;
    }
    catch (const std::bad_alloc&)
    {
      return Error{ErrorCode::CapacityExceeded, "Insufficient memory to encode Field metadata", "field.memory"};
    }
    catch (const std::length_error&)
    {
      return Error{ErrorCode::CapacityExceeded, "Field metadata exceeds a native container limit", "field.memory"};
    }
  }

  Result<FieldDefinition> decode_field_definition(std::span<const std::byte> immutableBody,
                                                  std::uint64_t recordOffset)
  {
    try
    {
      require(immutableBody.size() >= fixed_bytes, ErrorCode::IoError, "field.definition", "Truncated Field body");
      const auto begin = body_begin(recordOffset, immutableBody.size(), ErrorCode::IoError);
      const auto key = read_integer<std::uint64_t>(immutableBody, 0);
      require(key <= std::numeric_limits<std::uint32_t>::max() && field_id(static_cast<std::uint32_t>(key)),
              ErrorCode::IoError, "field.id", "Stored Field identity is not a full 0xEE-prefixed u32");
      FieldDefinition definition;
      definition._registration = read_common(immutableBody);
      definition._descriptor._id = static_cast<FieldId>(static_cast<std::uint32_t>(key));
      definition._descriptor._base = decode_base(read_integer<std::uint16_t>(immutableBody, 64));
      definition._descriptor._size = read_integer<std::uint32_t>(immutableBody, 68);
      const auto cursor = read_attributes(immutableBody, definition._descriptor, begin);
      auto strings = decode_text(immutableBody, cursor, begin, false);
      definition._descriptor._name = std::move(strings[0]);
      definition._descriptor._doc = std::move(strings[1]);
      check_field(definition._descriptor, ErrorCode::IoError);
      return definition;
    }
    catch (const CodecFailure& failure)
    {
      return failure._error;
    }
    catch (const std::bad_alloc&)
    {
      return Error{ErrorCode::CapacityExceeded, "Insufficient memory to decode Field metadata", "field.memory"};
    }
    catch (const std::length_error&)
    {
      return Error{ErrorCode::CapacityExceeded, "Field metadata exceeds a native container limit", "field.memory"};
    }
  }

  Result<std::vector<std::byte>> encode_schema_definition(const SchemaDefinition& definition,
                                                         std::uint64_t recordOffset)
  {
    try
    {
      check_schema(definition._descriptor, ErrorCode::InvalidArgument);
      check_registration(definition._registration, ErrorCode::InvalidArgument);
      const auto arrayBytes = checked_multiply(definition._descriptor._fields.size(), 4);
      const auto prefix = arrayBytes ? checked_add(fixed_bytes, *arrayBytes) : std::nullopt;
      require(prefix.has_value(), ErrorCode::InvalidArgument, "message.fields", "Schema Field array length overflows");
      auto bytes = encode_text(*prefix, definition._descriptor._name, definition._descriptor._doc, recordOffset);
      const auto schema = definition._descriptor._schema;
      write_common(bytes, (std::uint64_t{static_cast<std::uint16_t>(schema._type)} << 32) | schema._version,
                    definition._registration);
      write_integer<std::uint16_t>(bytes, 64, static_cast<std::uint16_t>(schema._type));
      write_integer<std::uint32_t>(bytes, 68, schema._version);
      if (!definition._descriptor._fields.empty())
      {
        write_integer<std::uint64_t>(bytes, 72, recordOffset + 64 + fixed_bytes);
        write_integer<std::uint64_t>(bytes, 80, definition._descriptor._fields.size());
        for (std::size_t index = 0; index < definition._descriptor._fields.size(); ++index)
        {
          write_integer<std::uint32_t>(bytes, fixed_bytes + index * 4,
              static_cast<std::uint32_t>(definition._descriptor._fields[index]));
        }
      }
      return bytes;
    }
    catch (const CodecFailure& failure)
    {
      return failure._error;
    }
  }

  Result<SchemaDefinition> decode_schema_definition(std::span<const std::byte> immutableBody,
                                                    std::uint64_t recordOffset)
  {
    try
    {
      require(immutableBody.size() >= fixed_bytes, ErrorCode::IoError, "message.definition", "Truncated Schema body");
      const auto begin = body_begin(recordOffset, immutableBody.size(), ErrorCode::IoError);
      require(zero(immutableBody.subspan(66, 2)), ErrorCode::IoError, "message.definition", "Reserved Schema bytes are nonzero");
      SchemaDefinition definition;
      definition._registration = read_common(immutableBody);
      auto& descriptor = definition._descriptor;
      descriptor._schema._type = static_cast<MessageType>(read_integer<std::uint16_t>(immutableBody, 64));
      descriptor._schema._version = read_integer<std::uint32_t>(immutableBody, 68);
      const auto expectedKey = (std::uint64_t{static_cast<std::uint16_t>(descriptor._schema._type)} << 32) |
                               descriptor._schema._version;
      require(descriptor._schema._type != MessageType::Invalid && descriptor._schema._version != 0 &&
                  read_integer<std::uint64_t>(immutableBody, 0) == expectedKey,
              ErrorCode::IoError, "message.schema", "Stored Schema key and type/version disagree");
      const auto count = read_integer<std::uint64_t>(immutableBody, 80);
      const auto arrayBytes = checked_multiply(count, 4);
      require(arrayBytes && *arrayBytes <= immutableBody.size() - fixed_bytes,
              ErrorCode::IoError, "message.fields", "Schema Field array exceeds its owning body");
      const auto reference = read_integer<std::uint64_t>(immutableBody, 72);
      require(reference == (count == 0 ? 0 : begin + fixed_bytes), ErrorCode::IoError,
              "message.fields", "Schema Field array is outside its canonical owning body");
      const auto cursor = fixed_bytes + host_size(*arrayBytes, ErrorCode::IoError);
      for (std::size_t index = 0; index < count; ++index)
      {
        require(field_id(read_integer<std::uint32_t>(immutableBody, fixed_bytes + index * 4)),
                ErrorCode::IoError, "message.fields", "Stored Schema Field identity loses its 0xEE prefix");
      }
      auto strings = decode_text(immutableBody, cursor, begin, true);
      descriptor._name = std::move(strings[0]);
      descriptor._doc = std::move(strings[1]);
      descriptor._fields.resize(host_size(count, ErrorCode::IoError));
      for (std::size_t index = 0; index < descriptor._fields.size(); ++index)
      {
        descriptor._fields[index] = static_cast<FieldId>(read_integer<std::uint32_t>(immutableBody, fixed_bytes + index * 4));
      }
      return definition;
    }
    catch (const CodecFailure& failure)
    {
      return failure._error;
    }
  }
} // namespace angaria::detail::mapped
