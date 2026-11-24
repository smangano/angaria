// Portable Field/schema representation tests, independent of a live Registry.
// Golden bodies are authored from docs/mapped-format.md rather than produced by
// the codecs under test. Native Field support and reference existence are runtime
// checks; recognizing wire metadata does not implement its payload encoding.

#include "schema_metadata.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;
  constexpr std::uint64_t record_offset = 8192;

  void put_integer(std::vector<std::byte>& bytes, std::size_t offset,
                   std::uint64_t value, std::size_t width)
  {
    for (std::size_t byte = 0; byte < width; ++byte)
    {
      bytes[offset + byte] = static_cast<std::byte>((value >> (8 * byte)) & 0xff);
    }
  }

  std::uint64_t get_integer(const std::vector<std::byte>& bytes, std::size_t offset,
                            std::size_t width)
  {
    std::uint64_t value{};
    for (std::size_t byte = 0; byte < width; ++byte)
    {
      value |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + byte])} << (8 * byte);
    }
    return value;
  }

  void append_text(std::vector<std::byte>& bytes, const std::string& text)
  {
    for (const auto character : text)
    {
      bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
  }

  template <typename T>
  T require_value(Result<T> result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    return std::get<T>(std::move(result));
  }

  template <typename T>
  void expect_error(const Result<T>& result, ErrorCode code)
  {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    const auto& error = std::get<Error>(result);
    EXPECT_EQ(error._code, code) << error._message;
    EXPECT_FALSE(error._path.empty());
    EXPECT_FALSE(error._message.empty());
  }

  mapped::FieldDefinition field_definition()
  {
    mapped::FieldDefinition result;
    result._descriptor._id = FieldId{0xee000001u};
    result._descriptor._name = "Price";
    result._descriptor._doc = "ticks";
    result._descriptor._base = EFieldBase::BASE_INT64;
    result._descriptor._size = 8;
    result._registration = RegistrationMetadata{TaskId{3}, UtcTimestamp{std::chrono::nanoseconds{-7}}, 9};
    return result;
  }

  mapped::SchemaDefinition schema_definition()
  {
    mapped::SchemaDefinition result;
    result._descriptor._schema = SchemaId{MessageType{65535}, 0xffffffffu};
    result._descriptor._name = "Quote";
    result._descriptor._doc = "v1";
    result._descriptor._fields = {FieldId{0xee000000u}, FieldId{0xeeffffffu}, FieldId{0xee000000u}};
    result._registration = RegistrationMetadata{TaskId{3}, UtcTimestamp{std::chrono::nanoseconds{-9}}, 11};
    return result;
  }

  std::vector<std::byte> golden_field()
  {
    std::vector<std::byte> bytes(88);
    put_integer(bytes, 0, 0xee000001u, 8);
    put_integer(bytes, 8, 3, 2);
    put_integer(bytes, 16, 0xfffffffffffffff9u, 8);
    put_integer(bytes, 24, 9, 8);
    put_integer(bytes, 32, 8344, 8);
    put_integer(bytes, 40, 5, 8);
    put_integer(bytes, 48, 8349, 8);
    put_integer(bytes, 56, 5, 8);
    put_integer(bytes, 64, 7, 2);
    put_integer(bytes, 68, 8, 4);
    append_text(bytes, "Priceticks");
    return bytes;
  }

  std::vector<std::byte> golden_schema()
  {
    std::vector<std::byte> bytes(100);
    put_integer(bytes, 0, 0x0000ffffffffffffu, 8);
    put_integer(bytes, 8, 3, 2);
    put_integer(bytes, 16, 0xfffffffffffffff7u, 8);
    put_integer(bytes, 24, 11, 8);
    put_integer(bytes, 32, 8356, 8);
    put_integer(bytes, 40, 5, 8);
    put_integer(bytes, 48, 8361, 8);
    put_integer(bytes, 56, 2, 8);
    put_integer(bytes, 64, 65535, 2);
    put_integer(bytes, 68, 0xffffffffu, 4);
    put_integer(bytes, 72, 8344, 8);
    put_integer(bytes, 80, 3, 8);
    put_integer(bytes, 88, 0xee000000u, 4);
    put_integer(bytes, 92, 0xeeffffffu, 4);
    put_integer(bytes, 96, 0xee000000u, 4);
    append_text(bytes, "Quotev1");
    return bytes;
  }

  // A Field ID retains all 32 bits, including its 0xEE prefix. The independent
  // body and deliberately displaced host buffer expose accidental native-struct
  // layout assumptions while preserving creator/time/revision and owning strings.
  TEST(SchemaMetadata, FieldMatchesIndependentGoldenBodyAndUnalignedDecode)
  {
    const auto definition = field_definition();
    EXPECT_EQ(require_value(mapped::encode_field_definition(definition, record_offset)), golden_field());
    auto bytes = golden_field();
    bytes.insert(bytes.begin(), std::byte{0xa5});
    const auto decoded = require_value(mapped::decode_field_definition(std::span{bytes}.subspan(1), record_offset));
    EXPECT_EQ(decoded._descriptor._id, definition._descriptor._id);
    EXPECT_EQ(decoded._descriptor._name, "Price");
    EXPECT_EQ(decoded._descriptor._doc, "ticks");
    EXPECT_EQ(decoded._descriptor._base, EFieldBase::BASE_INT64);
    EXPECT_EQ(decoded._descriptor._size, 8u);
    EXPECT_EQ(decoded._registration._createdBy, TaskId{3});
    EXPECT_EQ(decoded._registration._createdAt.time_since_epoch().count(), -7);
    EXPECT_EQ(decoded._registration._revision, 9u);
  }

  // Schemas preserve the exact type/version identity and the full ordered Field
  // list, including repetitions. The array precedes strings in this canonical
  // body, and all slice addresses remain logical file offsets, not host pointers.
  TEST(SchemaMetadata, SchemaMatchesIndependentGoldenBodyAndPreservesFullFieldIds)
  {
    const auto definition = schema_definition();
    EXPECT_EQ(require_value(mapped::encode_schema_definition(definition, record_offset)), golden_schema());
    auto bytes = golden_schema();
    bytes.insert(bytes.begin(), std::byte{0xa5});
    const auto decoded = require_value(mapped::decode_schema_definition(std::span{bytes}.subspan(1), record_offset));
    EXPECT_EQ(decoded._descriptor._schema, definition._descriptor._schema);
    EXPECT_EQ(decoded._descriptor._fields, definition._descriptor._fields);
    EXPECT_EQ(decoded._descriptor._name, "Quote");
    EXPECT_EQ(decoded._descriptor._doc, "v1");
    EXPECT_EQ(decoded._registration._createdBy, TaskId{3});
    EXPECT_EQ(decoded._registration._createdAt.time_since_epoch().count(), -9);
    EXPECT_EQ(decoded._registration._revision, 11u);
  }

  // Empty control schemas use a zero array offset/count, and optional empty docs
  // use a zero string slice. Only short zero allocation padding may follow the
  // canonical data; padding never becomes an undocumented extension field.
  TEST(SchemaMetadata, EmptyArraysAndDocumentationUseZeroSlicesAndBoundedPadding)
  {
    auto definition = schema_definition();
    definition._descriptor._fields.clear();
    definition._descriptor._doc.clear();
    auto body = require_value(mapped::encode_schema_definition(definition, record_offset));
    EXPECT_EQ(get_integer(body, 72, 8), 0u);
    EXPECT_EQ(get_integer(body, 80, 8), 0u);
    EXPECT_EQ(get_integer(body, 48, 8), 0u);
    EXPECT_EQ(get_integer(body, 56, 8), 0u);
    body.resize(body.size() + 63);
    EXPECT_TRUE(require_value(mapped::decode_schema_definition(body, record_offset))._descriptor._fields.empty());
    body.push_back(std::byte{});
    expect_error(mapped::decode_schema_definition(body, record_offset), ErrorCode::IoError);

    auto field = field_definition();
    field._descriptor._doc.clear();
    body = require_value(mapped::encode_field_definition(field, record_offset));
    EXPECT_EQ(get_integer(body, 48, 8), 0u);
    body.push_back(std::byte{1});
    expect_error(mapped::decode_field_definition(body, record_offset), ErrorCode::IoError);
  }

  // Portable metadata recognizes every assigned base code without enabling its
  // native payload policy. In particular, a future string Field can be inspected
  // with its declared u32 size even though the current native Registry rejects it.
  TEST(SchemaMetadata, PortableFieldBaseCodesAndDeclaredSizesRemainExplicit)
  {
    const std::array bases{
        EFieldBase::BASE_NONE, EFieldBase::BASE_CHAR, EFieldBase::BASE_BYTE,
        EFieldBase::BASE_INT16, EFieldBase::BASE_UINT16, EFieldBase::BASE_INT32,
        EFieldBase::BASE_UINT32, EFieldBase::BASE_INT64, EFieldBase::BASE_UINT64,
        EFieldBase::BASE_FLOAT32, EFieldBase::BASE_FLOAT64, EFieldBase::BASE_DATE,
        EFieldBase::BASE_TIMESTAMP, EFieldBase::BASE_YEARS, EFieldBase::BASE_MONTHS,
        EFieldBase::BASE_DAYS, EFieldBase::BASE_HOURS, EFieldBase::BASE_MINS,
        EFieldBase::BASE_SECS, EFieldBase::BASE_MILLISECS, EFieldBase::BASE_MICROSECS,
        EFieldBase::BASE_NANOSECS, EFieldBase::BASE_CHARS, EFieldBase::BASE_BYTES,
        EFieldBase::BASE_SYMBOL, EFieldBase::BASE_STRING};
    for (std::size_t code = 0; code < bases.size(); ++code)
    {
      auto definition = field_definition();
      definition._descriptor._base = bases[code];
      definition._descriptor._size = std::numeric_limits<std::uint32_t>::max();
      const auto body = require_value(mapped::encode_field_definition(definition, record_offset));
      EXPECT_EQ(get_integer(body, 64, 2), code);
      const auto decoded = require_value(mapped::decode_field_definition(body, record_offset));
      EXPECT_EQ(decoded._descriptor._base, bases[code]);
      EXPECT_EQ(decoded._descriptor._size, std::numeric_limits<std::uint32_t>::max());
    }
  }

  // Reject malformed input before producing bytes. The low 24 zero value is a
  // valid Field identity once prefixed by 0xEE, but an unprefixed, wrong-prefix,
  // zero-type/version, or unknown-enum descriptor has no valid wire identity.
  TEST(SchemaMetadata, DescriptorValidationRejectsInvalidIdentitiesAndEnums)
  {
    for (const auto id : {0u, 1u, 0xedffffffu, 0xef000001u})
    {
      auto field = field_definition();
      field._descriptor._id = static_cast<FieldId>(id);
      expect_error(mapped::validate_field_descriptor(field._descriptor), ErrorCode::InvalidArgument);
      expect_error(mapped::encode_field_definition(field, record_offset), ErrorCode::InvalidArgument);
      auto schema = schema_definition();
      schema._descriptor._fields[0] = static_cast<FieldId>(id);
      expect_error(mapped::validate_message_descriptor(schema._descriptor), ErrorCode::InvalidArgument);
    }
    auto field = field_definition();
    field._descriptor._id = FieldId{0xee000000u};
    require_value(mapped::validate_field_descriptor(field._descriptor));
    field._descriptor._base = static_cast<EFieldBase>(99);
    expect_error(mapped::validate_field_descriptor(field._descriptor), ErrorCode::InvalidArgument);
    for (const auto schema : {SchemaId{MessageType::Invalid, 1}, SchemaId{MessageType{1}, 0}})
    {
      auto definition = schema_definition();
      definition._descriptor._schema = schema;
      expect_error(mapped::encode_schema_definition(definition, record_offset), ErrorCode::InvalidArgument);
    }
  }

  // Registered definitions require attribution and a nonzero revision. Native
  // Task existence is checked separately; the body codec validates only encoded
  // identity ranges and retains the full signed nanosecond timestamp range.
  TEST(SchemaMetadata, RegistrationRequiresCreatorAndRevisionAndPreservesSignedTimes)
  {
    for (const auto timestamp : {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()})
    {
      auto field = field_definition();
      field._registration = RegistrationMetadata{TaskId{65535}, UtcTimestamp{std::chrono::nanoseconds{timestamp}},
                                                  std::numeric_limits<std::uint64_t>::max()};
      const auto read = require_value(mapped::decode_field_definition(
          require_value(mapped::encode_field_definition(field, record_offset)), record_offset));
      EXPECT_EQ(read._registration._createdAt, field._registration._createdAt);
      EXPECT_EQ(read._registration._createdBy, field._registration._createdBy);
      EXPECT_EQ(read._registration._revision, field._registration._revision);
    }
    for (const auto creator : {false, true})
    {
      auto field = field_definition();
      auto schema = schema_definition();
      if (creator)
      {
        field._registration._createdBy = TaskId::Invalid;
        schema._registration._createdBy = TaskId::Invalid;
      }
      else
      {
        field._registration._revision = 0;
        schema._registration._revision = 0;
      }
      expect_error(mapped::encode_field_definition(field, record_offset), ErrorCode::InvalidArgument);
      expect_error(mapped::encode_schema_definition(schema, record_offset), ErrorCode::InvalidArgument);
    }
  }

  // Damaged fixed bodies cannot supply partial definitions or invented recovery
  // flags/attributes. Stored keys must fit the full Field namespace and common
  // attribution fields; unknown base codes are malformed metadata, not policies.
  TEST(SchemaMetadata, FieldRejectsTruncationReservedAttributesAndMalformedKeys)
  {
    for (const auto length : {0u, 63u, 87u, 97u})
    {
      auto body = golden_field();
      body.resize(length);
      expect_error(mapped::decode_field_definition(body, record_offset), ErrorCode::IoError);
    }
    for (const auto offset : {10u, 11u, 15u, 66u, 67u, 72u, 80u})
    {
      auto body = golden_field();
      body[offset] = std::byte{1};
      expect_error(mapped::decode_field_definition(body, record_offset), ErrorCode::IoError);
    }
    for (const auto key : {std::uint64_t{0}, std::uint64_t{0xed000001u}, std::uint64_t{0x1ee000001u}})
    {
      auto body = golden_field();
      put_integer(body, 0, key, 8);
      expect_error(mapped::decode_field_definition(body, record_offset), ErrorCode::IoError);
    }
    for (const auto offset : {8u, 24u})
    {
      auto body = golden_field();
      put_integer(body, offset, 0, offset == 8 ? 2 : 8);
      expect_error(mapped::decode_field_definition(body, record_offset), ErrorCode::IoError);
    }
    auto body = golden_field();
    put_integer(body, 64, 26, 2);
    expect_error(mapped::decode_field_definition(body, record_offset), ErrorCode::IoError);
  }

  // The common schema key must agree with its explicit type/version pair.
  // Corrupt each identity/reserved field or truncate the body so a decoder cannot
  // accept one interpretation while Registry indexes use another.
  TEST(SchemaMetadata, SchemaRejectsTruncationReservedAndInconsistentIdentity)
  {
    for (const auto length : {0u, 63u, 87u, 106u})
    {
      auto body = golden_schema();
      body.resize(length);
      expect_error(mapped::decode_schema_definition(body, record_offset), ErrorCode::IoError);
    }
    for (const auto offset : {10u, 15u, 66u, 67u})
    {
      auto body = golden_schema();
      body[offset] = std::byte{1};
      expect_error(mapped::decode_schema_definition(body, record_offset), ErrorCode::IoError);
    }
    for (const auto offset : {0u, 8u, 24u, 64u, 68u})
    {
      auto body = golden_schema();
      put_integer(body, offset, 0, offset == 64 || offset == 8 ? 2 : offset == 68 ? 4 : 8);
      expect_error(mapped::decode_schema_definition(body, record_offset), ErrorCode::IoError);
    }
  }

  // Every variable slice belongs to one canonical body region. Overlapping
  // strings, forged addresses into another record, and wrapped lengths are
  // rejected before a caller can follow them as references.
  TEST(SchemaMetadata, StringSlicesMustBeCanonicalOwnedAndBounded)
  {
    for (const auto reference : {std::uint64_t{0}, std::uint64_t{4096}, std::uint64_t{8345},
                                 std::numeric_limits<std::uint64_t>::max()})
    {
      auto body = golden_field();
      put_integer(body, 32, reference, 8);
      expect_error(mapped::decode_field_definition(body, record_offset), ErrorCode::IoError);
    }
    auto body = golden_field();
    put_integer(body, 40, std::numeric_limits<std::uint64_t>::max(), 8);
    expect_error(mapped::decode_field_definition(body, record_offset), ErrorCode::IoError);
    body = golden_field();
    put_integer(body, 48, 8344, 8);
    expect_error(mapped::decode_field_definition(body, record_offset), ErrorCode::IoError);
    body = golden_field();
    put_integer(body, 56, 0, 8);
    expect_error(mapped::decode_field_definition(body, record_offset), ErrorCode::IoError);
    body = golden_schema();
    put_integer(body, 32, 8344, 8);
    expect_error(mapped::decode_schema_definition(body, record_offset), ErrorCode::IoError);
    body = golden_schema();
    put_integer(body, 48, 8362, 8);
    expect_error(mapped::decode_schema_definition(body, record_offset), ErrorCode::IoError);
  }

  // Schema counts and offsets are untrusted u64 values. Validate the full array
  // range before reserving memory or reading elements, then reject any element
  // whose full 32-bit identity would be lost or interpreted in another namespace.
  TEST(SchemaMetadata, FieldArraysRejectOverflowForeignReferencesAndInvalidElements)
  {
    for (const auto count : {std::uint64_t{100}, std::numeric_limits<std::uint64_t>::max()})
    {
      auto body = golden_schema();
      put_integer(body, 80, count, 8);
      expect_error(mapped::decode_schema_definition(body, record_offset), ErrorCode::IoError);
    }
    for (const auto reference : {std::uint64_t{0}, std::uint64_t{4096}, std::uint64_t{8348},
                                 std::numeric_limits<std::uint64_t>::max()})
    {
      auto body = golden_schema();
      put_integer(body, 72, reference, 8);
      expect_error(mapped::decode_schema_definition(body, record_offset), ErrorCode::IoError);
    }
    auto body = golden_schema();
    put_integer(body, 80, 0, 8);
    expect_error(mapped::decode_schema_definition(body, record_offset), ErrorCode::IoError);
    body = golden_schema();
    put_integer(body, 92, 0x00000001u, 4);
    expect_error(mapped::decode_schema_definition(body, record_offset), ErrorCode::IoError);
  }

  // A Field list is bounded by its containing allocation, not an accidental
  // 16-bit element count. More than 65,535 references and repeated IDs round-trip
  // without truncation, deduplication, or consumption of additional Field identities.
  TEST(SchemaMetadata, LargeFieldListsHaveNoUndocumentedSixteenBitLimit)
  {
    auto definition = schema_definition();
    definition._descriptor._fields.assign(65536, FieldId{0xeeffffffu});
    const auto body = require_value(mapped::encode_schema_definition(definition, record_offset));
    EXPECT_EQ(get_integer(body, 80, 8), 65536u);
    EXPECT_EQ(get_integer(body, 88, 4), 0xeeffffffu);
    const auto decoded = require_value(mapped::decode_schema_definition(body, record_offset));
    EXPECT_EQ(decoded._descriptor._fields, definition._descriptor._fields);
  }

  // Names require valid nonempty UTF-8 without NUL; optional documentation is
  // length delimited and preserves embedded NUL. Shared text validation gives
  // descriptor preflight and stored-body decoding the same syntax rules.
  TEST(SchemaMetadata, TextValidationPreservesLengthDelimitedDocumentation)
  {
    for (const auto& name : {std::string{}, std::string{"a\0b", 3}, std::string{"\xc0\xaf", 2}})
    {
      auto field = field_definition();
      field._descriptor._name = name;
      expect_error(mapped::validate_field_descriptor(field._descriptor), ErrorCode::InvalidArgument);
      auto schema = schema_definition();
      schema._descriptor._name = name;
      expect_error(mapped::validate_message_descriptor(schema._descriptor), ErrorCode::InvalidArgument);
    }
    auto field = field_definition();
    field._descriptor._name = "\xf4\x8f\xbf\xbf";
    field._descriptor._doc = std::string{"a\0b", 3};
    const auto decoded = require_value(mapped::decode_field_definition(
        require_value(mapped::encode_field_definition(field, record_offset)), record_offset));
    EXPECT_EQ(decoded._descriptor._name, field._descriptor._name);
    EXPECT_EQ(decoded._descriptor._doc, field._descriptor._doc);
    auto body = golden_field();
    body[88] = std::byte{0xff};
    expect_error(mapped::decode_field_definition(body, record_offset), ErrorCode::IoError);
    body = golden_schema();
    body[106] = std::byte{0xff};
    expect_error(mapped::decode_schema_definition(body, record_offset), ErrorCode::IoError);
  }

  // Logical placement is checked before constructing absolute slices. The
  // portable format requires eight-byte record alignment; live allocation uses
  // the stronger 64-byte alignment enforced by its journal.
  TEST(SchemaMetadata, RecordPlacementRejectsMisalignmentAndSignedOffsetOverflow)
  {
    for (const auto offset : {std::uint64_t{0}, std::uint64_t{4095}, std::uint64_t{8193},
                              std::uint64_t{0x7fffffffffffffc0}, std::numeric_limits<std::uint64_t>::max()})
    {
      expect_error(mapped::encode_field_definition(field_definition(), offset), ErrorCode::InvalidArgument);
      expect_error(mapped::decode_field_definition(golden_field(), offset), ErrorCode::IoError);
      expect_error(mapped::encode_schema_definition(schema_definition(), offset), ErrorCode::InvalidArgument);
      expect_error(mapped::decode_schema_definition(golden_schema(), offset), ErrorCode::IoError);
    }
    EXPECT_EQ(require_value(mapped::decode_field_definition(
                  require_value(mapped::encode_field_definition(field_definition(), 8200)), 8200))._descriptor._id,
              field_definition()._descriptor._id);
  }
} // namespace
