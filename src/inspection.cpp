#include "angaria/inspection.h"

#include "angaria/detail/checked_arithmetic.h"
#include "angaria/detail/error_support.h"
#include "angaria/detail/text_escaping.h"
#include "angaria/runtime.h"
#include "schema_metadata.h"

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace angaria
{
  namespace
  {
    struct InspectionFailure
    {
      Error _error;
    };

    [[noreturn]] void fail(ErrorCode code, const char* path, const char* message)
    {
      throw InspectionFailure{Error{code, message, path}};
    }

    Error caught_error()
    {
      try
      {
        throw;
      }
      catch (const InspectionFailure& failure)
      {
        return failure._error;
      }
      catch (const std::bad_alloc&)
      {
        return Error{ErrorCode::CapacityExceeded, "Insufficient memory for message inspection", "inspection.memory"};
      }
      catch (const std::length_error&)
      {
        return Error{ErrorCode::CapacityExceeded, "Inspection exceeds a native container limit", "inspection.memory"};
      }
      catch (...)
      {
        return Error{ErrorCode::InvalidState, "Unexpected message inspection failure", "inspection.internal"};
      }
    }

    std::uint64_t add_bytes(std::uint64_t left, std::uint64_t right)
    {
      const auto total = detail::checked_add(left, right);
      if (!total)
      {
        fail(ErrorCode::CapacityExceeded, "inspection.catalog", "Catalog byte count overflows");
      }
      return *total;
    }

    std::uint64_t multiply_bytes(std::uint64_t count, std::uint64_t width)
    {
      const auto total = detail::checked_multiply(count, width);
      if (!total)
      {
        fail(ErrorCode::CapacityExceeded, "inspection.catalog", "Catalog array byte count overflows");
      }
      return *total;
    }

    // This pass borrows the caller's strings/lists. It bounds subsequent deep
    // validation and copies, including labels that may greatly exceed payloads.
    std::uint64_t borrowed_field_bytes(const FieldDescriptor& field)
    {
      auto bytes = add_bytes(add_bytes(88, field._name.size()), field._doc.size());
      if (field._logical._kind == FieldLogicalKind::Enumeration)
      {
        bytes = add_bytes(bytes, add_bytes(16, multiply_bytes(field._logical._enumerators.size(), 24)));
        for (const auto& entry : field._logical._enumerators)
        {
          bytes = add_bytes(bytes, entry._name.size());
        }
      }
      else if (field._logical._kind != FieldLogicalKind::None)
      {
        bytes = add_bytes(bytes, 8);
      }
      return bytes;
    }

    void validate_limits(InspectionLimits limits)
    {
      if (limits._maxRecordBytes < MessageRecordCodec::header_bytes)
      {
        fail(ErrorCode::InvalidArgument, "inspection.limits.record_bytes", "Record budget must include the 80-byte header");
      }
      if (limits._maxTextBytes == 0)
      {
        fail(ErrorCode::InvalidArgument, "inspection.limits.text_bytes", "Rendered text budget must be positive");
      }
    }

    void validate_catalog(const RegistrySnapshot& snapshot, InspectionLimits limits)
    {
      std::uint64_t bytes{};
      for (const auto& field : snapshot._fields)
      {
        bytes = add_bytes(bytes, borrowed_field_bytes(field._descriptor));
        if (bytes > limits._maxCatalogBytes)
        {
          fail(ErrorCode::CapacityExceeded, "inspection.catalog", "Retained Field definitions exceed the catalog byte budget");
        }
      }
      for (const auto& message : snapshot._messages)
      {
        const auto& descriptor = message._descriptor;
        auto body = add_bytes(add_bytes(88, descriptor._name.size()), descriptor._doc.size());
        body = add_bytes(body, multiply_bytes(descriptor._fields.size(), 4));
        bytes = add_bytes(bytes, body);
        if (bytes > limits._maxCatalogBytes)
        {
          fail(ErrorCode::CapacityExceeded, "inspection.catalog", "Retained schemas exceed the catalog byte budget");
        }
      }

      std::map<FieldId, const FieldDescriptor*> fields;
      std::set<std::string_view> fieldNames;
      for (const auto& entry : snapshot._fields)
      {
        const auto& field = entry._descriptor;
        detail::value_or_throw<InspectionFailure>(detail::mapped::validate_field_descriptor(field));
        if (!fields.emplace(field._id, &field).second || !fieldNames.insert(field._name).second)
        {
          fail(ErrorCode::InvalidArgument, "inspection.catalog.fields", "Field IDs and names must uniquely identify definitions");
        }
      }
      std::set<std::pair<MessageType, std::uint32_t>> identities;
      std::map<MessageType, std::string_view> typeNames;
      std::map<std::string_view, MessageType> nameTypes;
      for (const auto& entry : snapshot._messages)
      {
        const auto& message = entry._descriptor;
        detail::value_or_throw<InspectionFailure>(detail::mapped::validate_message_descriptor(message));
        const auto [type, newType] = typeNames.emplace(message._schema._type, message._name);
        const auto [name, newName] = nameTypes.emplace(message._name, message._schema._type);
        if (!identities.emplace(message._schema._type, message._schema._version).second ||
            (!newType && type->second != message._name) ||
            (!newName && name->second != message._schema._type))
        {
          fail(ErrorCode::InvalidArgument, "inspection.catalog.messages", "Schema identities and type/name families must be unambiguous");
        }
        for (const auto field : message._fields)
        {
          if (!fields.contains(field))
          {
            fail(ErrorCode::InvalidArgument, "inspection.catalog.messages.fields", "Schema references a missing Field definition");
          }
        }
      }
    }

    class BoundedText
    {
    public:
      explicit BoundedText(std::uint64_t limit) : _limit(limit)
      {
      }

      void append(std::string_view value)
      {
        if (value.size() > _limit - _text.size())
        {
          fail(ErrorCode::CapacityExceeded, "inspection.text", "Rendered inspection exceeds its text byte budget");
        }
        _text.append(value);
      }

      template <typename T>
      void integer(T value)
      {
        std::array<char, 32> bytes{};
        const auto converted = std::to_chars(bytes.data(), bytes.data() + bytes.size(), value);
        if (converted.ec != std::errc{})
        {
          fail(ErrorCode::InvalidState, "inspection.number", "Integer rendering failed");
        }
        append(std::string_view{bytes.data(), static_cast<std::size_t>(converted.ptr - bytes.data())});
      }

      void hex(std::uint64_t value, std::size_t digits)
      {
        std::array<char, 16> bytes{};
        for (std::size_t index = 0; index < digits; ++index)
        {
          bytes[digits - index - 1] = detail::hex_digit(static_cast<unsigned>((value >> (4 * index)) & 15));
        }
        append(std::string_view{bytes.data(), digits});
      }

      void quoted(std::string_view value)
      {
        append("\"");
        for (const auto raw : value)
        {
          const auto escaped = detail::escape_ascii_byte(static_cast<unsigned char>(raw), true);
          append(std::string_view{escaped._bytes.data(), escaped._size});
        }
        append("\"");
      }

      std::string finish() &&
      {
        return std::move(_text);
      }

    private:
      std::uint64_t _limit;
      std::string _text;
    };

    const char* base_name(EFieldBase base)
    {
      switch (base)
      {
      case EFieldBase::BASE_CHAR: return "char";
      case EFieldBase::BASE_BYTE: return "byte";
      case EFieldBase::BASE_INT8: return "int8";
      case EFieldBase::BASE_INT16: return "int16";
      case EFieldBase::BASE_UINT16: return "uint16";
      case EFieldBase::BASE_INT32: return "int32";
      case EFieldBase::BASE_UINT32: return "uint32";
      case EFieldBase::BASE_INT64: return "int64";
      case EFieldBase::BASE_UINT64: return "uint64";
      case EFieldBase::BASE_FLOAT32: return "float32";
      case EFieldBase::BASE_FLOAT64: return "float64";
      default: fail(ErrorCode::UnsupportedPolicy, "inspection.field", "Field base has no implemented inspection representation");
      }
    }

    void render_type(BoundedText& text, const FieldDescriptor& field)
    {
      switch (field._logical._kind)
      {
      case FieldLogicalKind::None:
        text.append(base_name(field._base));
        return;
      case FieldLogicalKind::Boolean:
        text.append("bool");
        return;
      case FieldLogicalKind::Bits:
        text.append("bits");
        text.integer(std::uint64_t{field._size} * 8);
        return;
      case FieldLogicalKind::Enumeration:
        text.append("enum<");
        text.append(base_name(field._base));
        text.append(">");
        return;
      case FieldLogicalKind::FixedAscii:
        text.append("chars<");
        text.integer(field._size);
        text.append(">");
        return;
      }
      fail(ErrorCode::InvalidArgument, "inspection.field", "Unknown logical Field kind");
    }

    template <typename T>
    void render_float(BoundedText& text, const T& value)
    {
      using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
      const auto bits = std::bit_cast<Bits>(value);
      constexpr unsigned fractionBits = sizeof(T) == 4 ? 23 : 52;
      constexpr Bits exponentMask = sizeof(T) == 4 ? 0xff : 0x7ff;
      constexpr Bits fractionMask = (Bits{1} << fractionBits) - 1;
      constexpr Bits signMask = Bits{1} << (sizeof(T) * 8 - 1);
      const auto exponent = (bits >> fractionBits) & exponentMask;
      // Classify through integer bits. In particular, do not quiet a signaling
      // NaN merely to decide which diagnostic representation to print.
      if (exponent == exponentMask && (bits & fractionMask) != 0)
      {
        text.append("nan(0x");
        text.hex(bits, sizeof(T) * 2);
        text.append(")");
      }
      else if (exponent == exponentMask)
      {
        text.append((bits & signMask) != 0 ? "-inf" : "inf");
      }
      else if (bits == signMask)
      {
        text.append("-0");
      }
      else
      {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(std::numeric_limits<T>::max_digits10) << value;
        text.append(stream.str());
      }
    }

    void render_value(BoundedText& text, const FieldDescriptor& field, const PayloadValue& value)
    {
      std::visit([&](const auto& item)
      {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, bool>)
        {
          text.append(item ? "true" : "false");
        }
        else if constexpr (std::is_same_v<T, char>)
        {
          text.quoted(std::string_view{&item, 1});
        }
        else if constexpr (std::is_same_v<T, BitFieldValue>)
        {
          text.append("0x");
          text.hex(item._value, field._size * 2);
        }
        else if constexpr (std::is_same_v<T, EnumValue>)
        {
          const auto number = std::visit([](auto integer) -> EnumerationNumber
          {
            if constexpr (std::is_signed_v<decltype(integer)>)
            {
              return static_cast<std::int64_t>(integer);
            }
            else
            {
              return static_cast<std::uint64_t>(integer);
            }
          }, item._value);
          const EnumeratorDescriptor* found{};
          for (const auto& entry : field._logical._enumerators)
          {
            if (entry._value == number)
            {
              found = &entry;
              break;
            }
          }
          if (!found)
          {
            fail(ErrorCode::InvalidState, "inspection.enum", "Decoded enum has no registered label");
          }
          text.quoted(found->_name);
          text.append(" (");
          std::visit([&](auto integer)
          {
            text.integer(integer);
          }, number);
          text.append(")");
        }
        else if constexpr (std::is_same_v<T, FixedAsciiValue>)
        {
          text.quoted(item._text);
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
          render_float(text, item);
        }
        else if constexpr (std::is_signed_v<T>)
        {
          text.integer(static_cast<std::int64_t>(item));
        }
        else
        {
          text.integer(static_cast<std::uint64_t>(item));
        }
      }, value);
    }

    std::string render_message(const MessageRecordView& record, const MessageDescriptor& message,
                                 const BinaryPayloadCodec& codec, std::span<const PayloadValue> values,
                                 std::uint64_t textLimit)
    {
      const auto& header = record._header;
      const auto& producer = header._publication._producer;
      BoundedText text{textLimit};
      text.append("message ");
      text.quoted(message._name);
      text.append(" session=");
      text.hex(producer._session._high, 16);
      text.hex(producer._session._low, 16);
      text.append(" type=");
      text.integer(static_cast<std::uint16_t>(header._schema._type));
      text.append(" version=");
      text.integer(header._schema._version);
      text.append(" producer=");
      text.integer(static_cast<std::uint16_t>(producer._task));
      text.append(" incarnation=");
      text.integer(producer._incarnation);
      text.append(" sequence=");
      text.integer(header._publication._sequence);
      text.append(" time_ns=");
      text.integer(header._publicationTime.time_since_epoch().count());
      text.append(" flags=0x");
      text.hex(header._flags, 8);
      text.append("\n");
      const auto fields = codec.fields();
      for (std::size_t index = 0; index < fields.size(); ++index)
      {
        text.append("  [");
        text.integer(index);
        text.append("] ");
        text.quoted(fields[index]._name);
        text.append(" ");
        render_type(text, fields[index]);
        text.append(" = ");
        render_value(text, fields[index], values[index]);
        text.append("\n");
      }
      return std::move(text).finish();
    }
  } // namespace

  MessageInspector::MessageInspector(SessionId session, RegistrySnapshot catalog, InspectionLimits limits)
      : _session(session), _catalog(std::move(catalog)), _limits(limits)
  {
  }

  MessageInspector& MessageInspector::operator=(const MessageInspector& other)
  {
    if (this != &other)
    {
      MessageInspector complete(other);
      *this = std::move(complete);
    }
    return *this;
  }

  MessageInspector::MessageInspector(MessageInspector&& other) noexcept
      : _session(std::exchange(other._session, {})), _catalog(std::move(other._catalog)), _limits(other._limits)
  {
    other._catalog = {};
  }

  MessageInspector& MessageInspector::operator=(MessageInspector&& other) noexcept
  {
    if (this != &other)
    {
      _session = std::exchange(other._session, {});
      _catalog = std::move(other._catalog);
      _limits = other._limits;
      other._catalog = {};
    }
    return *this;
  }

  SessionId MessageInspector::session() const
  {
    return _session;
  }

  Result<MessageInspector> MessageInspector::from_session(const Session& session, InspectionLimits limits)
  {
    try
    {
      validate_limits(limits);
      const auto identity = session.identity();
      if (identity == SessionId{})
      {
        fail(ErrorCode::InvalidState, "inspection.session", "Inspector needs a live Session handle");
      }
      const auto catalog = detail::value_or_throw<InspectionFailure>(session.registry().snapshot());
      return from_snapshot(identity, catalog, limits);
    }
    catch (...)
    {
      return caught_error();
    }
  }

  Result<MessageInspector> MessageInspector::from_snapshot(SessionId schemaSession,
                                                            const RegistrySnapshot& snapshot,
                                                            InspectionLimits limits)
  {
    try
    {
      validate_limits(limits);
      if (schemaSession == SessionId{})
      {
        fail(ErrorCode::InvalidArgument, "inspection.session", "Schema Session identity must be nonzero");
      }
      validate_catalog(snapshot, limits);
      RegistrySnapshot catalog;
      catalog._revision = snapshot._revision;
      catalog._currentRevision = snapshot._currentRevision;
      catalog._fields.reserve(snapshot._fields.size());
      for (const auto& field : snapshot._fields)
      {
        catalog._fields.push_back({detail::value_or_throw<InspectionFailure>(detail::mapped::canonicalize_field_descriptor(field._descriptor)),
                                    field._registration});
      }
      catalog._messages = snapshot._messages;
      return MessageInspector{schemaSession, std::move(catalog), limits};
    }
    catch (...)
    {
      return caught_error();
    }
  }

  Result<std::string> MessageInspector::inspect(std::span<const std::byte> record) const
  {
    try
    {
      if (_session == SessionId{})
      {
        fail(ErrorCode::InvalidState, "inspection.state", "Inspector has been moved from");
      }
      return inspect(detail::value_or_throw<InspectionFailure>(MessageRecordCodec::decode_view(record, _limits._maxRecordBytes)));
    }
    catch (...)
    {
      return caught_error();
    }
  }

  Result<std::string> MessageInspector::inspect(const MessageRecordView& record) const
  {
    try
    {
      if (_session == SessionId{})
      {
        fail(ErrorCode::InvalidState, "inspection.state", "Inspector has been moved from");
      }
      detail::value_or_throw<InspectionFailure>(MessageRecordCodec::encode_header(record._header, _limits._maxRecordBytes));
      if (record._header._payloadBytes != record._payload.size())
      {
        fail(ErrorCode::InvalidArgument, "message_record.payload_size", "Declared payload length differs from the supplied view");
      }
      if (record._header._publication._producer._session != _session)
      {
        fail(ErrorCode::SchemaConflict, "inspection.session", "Record and schema catalog belong to different Sessions");
      }
      if (record._header._format != PayloadFormat::Binary)
      {
        fail(ErrorCode::UnsupportedPolicy, "inspection.format", "Generic value inspection currently supports binary payloads only");
      }
      const MessageDescriptor* selected{};
      for (const auto& entry : _catalog._messages)
      {
        if (entry._descriptor._schema == record._header._schema)
        {
          selected = &entry._descriptor;
          break;
        }
      }
      if (!selected)
      {
        fail(ErrorCode::NotFound, "inspection.schema", "The exact record schema is absent from the retained catalog");
      }
      std::map<FieldId, std::uint64_t> widths;
      for (const auto& field : _catalog._fields)
      {
        widths.emplace(field._descriptor._id, borrowed_field_bytes(field._descriptor));
      }
      std::uint64_t expanded{};
      for (const auto id : selected->_fields)
      {
        expanded = add_bytes(expanded, widths.at(id));
        if (expanded > _limits._maxCatalogBytes)
        {
          fail(ErrorCode::CapacityExceeded, "inspection.layout", "Expanded Field layout exceeds the catalog byte budget");
        }
      }
      const auto codec = detail::value_or_throw<InspectionFailure>(BinaryPayloadCodec::from_snapshot(
          _catalog, selected->_schema, _limits._maxRecordBytes - MessageRecordCodec::header_bytes));
      const auto values = detail::value_or_throw<InspectionFailure>(MessageRecordCodec::decode_binary(record, codec, _session));
      return render_message(record, *selected, codec, values, _limits._maxTextBytes);
    }
    catch (...)
    {
      return caught_error();
    }
  }
} // namespace angaria
