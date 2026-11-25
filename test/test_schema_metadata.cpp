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
    put_integer(body, 64, 27, 2);
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

  mapped::FieldDefinition enum_definition()
  {
    auto definition = field_definition();
    auto& field = definition._descriptor;
    field._id = FieldId{0xee000002u};
    field._name = "Range";
    field._doc.clear();
    field._base = EFieldBase::BASE_INT8;
    field._size = 1;
    field._logical._kind = FieldLogicalKind::Enumeration;
    // Deliberately unsorted input: canonical numeric order is Low, then Max.
    field._logical._enumerators = {{"Max", std::int64_t{127}}, {"Low", std::int64_t{-128}}};
    return definition;
  }

  std::vector<std::byte> golden_enum()
  {
    // These offsets are authored from the format-2.1 contract: 88 fixed bytes,
    // an eight-byte attribute header, u64 count, two 24-byte entries, six label
    // bytes, then the five-byte Field name. No C++ object layout is involved.
    std::vector<std::byte> bytes(152);
    put_integer(bytes, 0, 0xee000002u, 8);
    put_integer(bytes, 8, 3, 2);
    put_integer(bytes, 16, 0xfffffffffffffff9u, 8);
    put_integer(bytes, 24, 9, 8);
    put_integer(bytes, 32, 8414, 8);
    put_integer(bytes, 40, 5, 8);
    put_integer(bytes, 64, 26, 2);
    put_integer(bytes, 66, 1, 2);
    put_integer(bytes, 68, 1, 4);
    put_integer(bytes, 72, 8344, 8);
    put_integer(bytes, 80, 70, 8);
    put_integer(bytes, 88, 1, 2);
    put_integer(bytes, 90, 3, 2);
    put_integer(bytes, 96, 2, 8);
    put_integer(bytes, 104, 0xffffffffffffff80u, 8);
    put_integer(bytes, 112, 8408, 8);
    put_integer(bytes, 120, 3, 8);
    put_integer(bytes, 128, 127, 8);
    put_integer(bytes, 136, 8411, 8);
    put_integer(bytes, 144, 3, 8);
    append_text(bytes, "LowMaxRange");
    return bytes;
  }

  // An owning enum definition preserves numeric signedness and all labels.
  // Declaration order is not semantic: encoding and canonicalization put the
  // explicit signed values in ascending order without modifying the input.
  TEST(LogicalMetadata, SignedEnumMatchesIndependentGoldenBytesAndOwnsCanonicalLabels)
  {
    auto definition = enum_definition();
    EXPECT_EQ(require_value(mapped::encode_field_definition(definition, record_offset)), golden_enum());
    EXPECT_EQ(require_value(mapped::field_definition_body_bytes(definition._descriptor)), 163u);
    const auto canonical = require_value(mapped::canonicalize_field_descriptor(definition._descriptor));
    EXPECT_EQ(canonical._logical._enumerators.front()._name, "Low");
    EXPECT_EQ(definition._descriptor._logical._enumerators.front()._name, "Max");
    const auto decoded = require_value(mapped::decode_field_definition(golden_enum(), record_offset));
    EXPECT_EQ(decoded._descriptor._logical, canonical._logical);
    EXPECT_EQ(decoded._descriptor._name, "Range");
    EXPECT_TRUE(mapped::field_requires_basic_fields(decoded._descriptor));
    definition._descriptor._logical._enumerators.back()._name = "Changed";
    EXPECT_EQ(canonical._logical._enumerators.front()._name, "Low");
    EXPECT_EQ(decoded._descriptor._logical._enumerators.front()._name, "Low");
  }

  // The metadata numeric slot is a full u64, never a signed TOML-sized number.
  // Under uint64 storage, all-one bits are UINT64_MAX rather than minus one.
  TEST(LogicalMetadata, UnsignedEnumPreservesTheFullUint64Range)
  {
    auto definition = enum_definition();
    auto& field = definition._descriptor;
    field._base = EFieldBase::BASE_UINT64;
    field._size = 8;
    field._logical._enumerators = {{"Max", std::numeric_limits<std::uint64_t>::max()}, {"Low", std::uint64_t{0}}};
    auto expected = golden_enum();
    put_integer(expected, 64, 8, 2);
    put_integer(expected, 68, 8, 4);
    put_integer(expected, 104, 0, 8);
    put_integer(expected, 128, std::numeric_limits<std::uint64_t>::max(), 8);
    EXPECT_EQ(require_value(mapped::encode_field_definition(definition, record_offset)), expected);
    const auto read = require_value(mapped::decode_field_definition(expected, record_offset));
    EXPECT_EQ(std::get<std::uint64_t>(read._descriptor._logical._enumerators.front()._value), 0u);
    EXPECT_EQ(std::get<std::uint64_t>(read._descriptor._logical._enumerators.back()._value),
              std::numeric_limits<std::uint64_t>::max());
  }

  // Boolean, bit-field, and ASCII definitions use the same eight-byte tagged
  // header. Their storage base and declared size supply all width information;
  // even UINT32_MAX ASCII size describes metadata without allocating a payload.
  TEST(LogicalMetadata, SimpleLogicalKindsHaveExactEightByteAttributes)
  {
    struct Shape
    {
      FieldLogicalKind _kind;
      EFieldBase _base;
      std::uint32_t _size;
      std::uint16_t _code;
    };
    const std::array cases{
        Shape{FieldLogicalKind::Boolean, EFieldBase::BASE_BYTE, 1, 1},
        Shape{FieldLogicalKind::Bits, EFieldBase::BASE_BYTE, 1, 2},
        Shape{FieldLogicalKind::Bits, EFieldBase::BASE_UINT16, 2, 2},
        Shape{FieldLogicalKind::Bits, EFieldBase::BASE_UINT32, 4, 2},
        Shape{FieldLogicalKind::Bits, EFieldBase::BASE_UINT64, 8, 2},
        Shape{FieldLogicalKind::FixedAscii, EFieldBase::BASE_CHARS, 1, 4},
        Shape{FieldLogicalKind::FixedAscii, EFieldBase::BASE_CHARS,
              std::numeric_limits<std::uint32_t>::max(), 4}};
    for (const auto& shape : cases)
    {
      auto definition = field_definition();
      auto& field = definition._descriptor;
      field._base = shape._base;
      field._size = shape._size;
      field._logical._kind = shape._kind;
      const auto bytes = require_value(mapped::encode_field_definition(definition, record_offset));
      EXPECT_EQ(bytes.size(), 106u);
      EXPECT_EQ(get_integer(bytes, 66, 2), 1u);
      EXPECT_EQ(get_integer(bytes, 72, 8), 8344u);
      EXPECT_EQ(get_integer(bytes, 80, 8), 8u);
      EXPECT_EQ(get_integer(bytes, 88, 2), 1u);
      EXPECT_EQ(get_integer(bytes, 90, 2), shape._code);
      EXPECT_EQ(get_integer(bytes, 92, 4), 0u);
      EXPECT_EQ(get_integer(bytes, 32, 8), 8352u);
      const auto read = require_value(mapped::decode_field_definition(bytes, record_offset));
      EXPECT_EQ(read._descriptor._logical, field._logical);
      EXPECT_EQ(read._descriptor._base, field._base);
      EXPECT_EQ(read._descriptor._size, field._size);
    }
    auto ordinary = field_definition();
    ordinary._descriptor._name = "TradingEnabled";
    ordinary._descriptor._base = EFieldBase::BASE_BYTE;
    ordinary._descriptor._size = 1;
    EXPECT_FALSE(mapped::field_requires_basic_fields(ordinary._descriptor));
    const auto read = require_value(mapped::decode_field_definition(
        require_value(mapped::encode_field_definition(ordinary, record_offset)), record_offset));
    EXPECT_EQ(read._descriptor._logical._kind, FieldLogicalKind::None);
  }

  // The newly assigned signed-byte code is appended after the old codes. Its
  // scalar body still has no attributes, while its one-byte width is mandatory.
  TEST(LogicalMetadata, Int8AppendsCodeWithoutChangingTheScalarBodyLayout)
  {
    auto definition = field_definition();
    definition._descriptor._base = EFieldBase::BASE_INT8;
    definition._descriptor._size = 1;
    auto expected = golden_field();
    put_integer(expected, 64, 26, 2);
    put_integer(expected, 68, 1, 4);
    EXPECT_EQ(require_value(mapped::encode_field_definition(definition, record_offset)), expected);
    EXPECT_EQ(require_value(mapped::decode_field_definition(expected, record_offset))._descriptor._base,
              EFieldBase::BASE_INT8);
    EXPECT_TRUE(mapped::field_requires_basic_fields(definition._descriptor));
    for (const auto size : {0u, 2u, std::numeric_limits<std::uint32_t>::max()})
    {
      definition._descriptor._size = size;
      expect_error(mapped::validate_field_descriptor(definition._descriptor), ErrorCode::InvalidArgument);
      auto damaged = expected;
      put_integer(damaged, 68, size, 4);
      expect_error(mapped::decode_field_definition(damaged, record_offset), ErrorCode::IoError);
    }
  }

  // Representation metadata must agree with its storage base and width. An
  // enum table cannot be smuggled into a Boolean, ASCII, bits, or ordinary scalar
  // descriptor; unknown logical kinds are explicit validation failures.
  TEST(LogicalMetadata, DescriptorKindsRejectInconsistentBasesSizesAndExtraEntries)
  {
    auto definition = enum_definition();
    auto field = definition._descriptor;
    for (const auto kind : {FieldLogicalKind::None, FieldLogicalKind::Boolean,
                             FieldLogicalKind::Bits, FieldLogicalKind::FixedAscii})
    {
      field._logical._kind = kind;
      expect_error(mapped::validate_field_descriptor(field), ErrorCode::InvalidArgument);
    }
    field._logical._enumerators.clear();
    for (const auto kind : {FieldLogicalKind::Boolean, FieldLogicalKind::Bits,
                             FieldLogicalKind::FixedAscii, FieldLogicalKind::Enumeration,
                             static_cast<FieldLogicalKind>(99)})
    {
      field._logical._kind = kind;
      expect_error(mapped::validate_field_descriptor(field), ErrorCode::InvalidArgument);
    }
    field._logical._kind = FieldLogicalKind::Bits;
    field._base = EFieldBase::BASE_UINT16;
    field._size = 1;
    expect_error(mapped::validate_field_descriptor(field), ErrorCode::InvalidArgument);
    field._logical._kind = FieldLogicalKind::FixedAscii;
    field._base = EFieldBase::BASE_CHARS;
    field._size = 0;
    expect_error(mapped::validate_field_descriptor(field), ErrorCode::InvalidArgument);
    field = definition._descriptor;
    field._base = EFieldBase::BASE_FLOAT64;
    field._size = 8;
    expect_error(mapped::validate_field_descriptor(field), ErrorCode::InvalidArgument);
  }

  // Enumeration metadata uses int64 or uint64 according to the declared base.
  // A narrow base still imposes narrow bounds before any persisted conversion.
  TEST(LogicalMetadata, EnumeratorNumbersEnforceSignednessAndEveryStorageWidth)
  {
    struct Shape
    {
      EFieldBase _base;
      unsigned _size;
      bool _signed;
    };
    const std::array cases{
        Shape{EFieldBase::BASE_INT8, 1, true}, Shape{EFieldBase::BASE_BYTE, 1, false},
        Shape{EFieldBase::BASE_INT16, 2, true}, Shape{EFieldBase::BASE_UINT16, 2, false},
        Shape{EFieldBase::BASE_INT32, 4, true}, Shape{EFieldBase::BASE_UINT32, 4, false},
        Shape{EFieldBase::BASE_INT64, 8, true}, Shape{EFieldBase::BASE_UINT64, 8, false}};
    for (const auto shape : cases)
    {
      auto field = enum_definition()._descriptor;
      field._base = shape._base;
      field._size = shape._size;
      if (shape._signed)
      {
        const auto minimum = shape._size == 8 ? std::numeric_limits<std::int64_t>::min() :
            -(std::int64_t{1} << (shape._size * 8 - 1));
        const auto maximum = shape._size == 8 ? std::numeric_limits<std::int64_t>::max() :
            (std::int64_t{1} << (shape._size * 8 - 1)) - 1;
        field._logical._enumerators = {{"Minimum", minimum}, {"Maximum", maximum}};
        require_value(mapped::validate_field_descriptor(field));
        field._logical._enumerators.front()._value = std::uint64_t{0};
        expect_error(mapped::validate_field_descriptor(field), ErrorCode::InvalidArgument);
        if (shape._size != 8)
        {
          field._logical._enumerators.front()._value = minimum - 1;
          expect_error(mapped::validate_field_descriptor(field), ErrorCode::InvalidArgument);
          field._logical._enumerators.front()._value = maximum + 1;
          expect_error(mapped::validate_field_descriptor(field), ErrorCode::InvalidArgument);
        }
      }
      else
      {
        const auto maximum = shape._size == 8 ? std::numeric_limits<std::uint64_t>::max() :
            (std::uint64_t{1} << (shape._size * 8)) - 1;
        field._logical._enumerators = {{"Minimum", std::uint64_t{0}}, {"Maximum", maximum}};
        require_value(mapped::validate_field_descriptor(field));
        field._logical._enumerators.front()._value = std::int64_t{0};
        expect_error(mapped::validate_field_descriptor(field), ErrorCode::InvalidArgument);
        if (shape._size != 8)
        {
          field._logical._enumerators.front()._value = maximum + 1;
          expect_error(mapped::validate_field_descriptor(field), ErrorCode::InvalidArgument);
        }
      }
    }
  }

  // Labels and numbers each identify exactly one entry. Metadata text rules
  // reject empty labels, embedded NUL, and malformed UTF-8 before ownership.
  TEST(LogicalMetadata, EnumeratorLabelsAndValuesMustBeValidAndUnique)
  {
    for (const auto& label : {std::string{}, std::string{"a\0b", 3}, std::string{"\xc0\xaf", 2}, std::string{"Max"}})
    {
      auto field = enum_definition()._descriptor;
      field._logical._enumerators.back()._name = label;
      expect_error(mapped::canonicalize_field_descriptor(field), ErrorCode::InvalidArgument);
    }
    auto field = enum_definition()._descriptor;
    field._logical._enumerators.back()._value = std::int64_t{127};
    expect_error(mapped::field_definition_body_bytes(field), ErrorCode::InvalidArgument);
    field._logical._enumerators.clear();
    expect_error(mapped::validate_field_descriptor(field), ErrorCode::InvalidArgument);
  }

  // A required flag and an owned attribute block are one unit. Unknown flags,
  // versions/kinds, nonzero reserved bytes, truncated arrays, and forged slices
  // cannot change a Field's interpretation or escape its immutable record.
  TEST(LogicalMetadata, StoredAttributesRejectMalformedHeadersAndOwnership)
  {
    for (const auto flag : {0u, 2u, 3u, 0x8000u})
    {
      auto bytes = golden_enum();
      put_integer(bytes, 66, flag, 2);
      expect_error(mapped::decode_field_definition(bytes, record_offset), ErrorCode::IoError);
    }
    for (const auto offset : {72u, 80u, 88u, 90u})
    {
      auto bytes = golden_enum();
      put_integer(bytes, offset, 0, offset < 88 ? 8 : 2);
      expect_error(mapped::decode_field_definition(bytes, record_offset), ErrorCode::IoError);
    }
    for (const auto offset : {92u, 93u, 95u})
    {
      auto bytes = golden_enum();
      bytes[offset] = std::byte{1};
      expect_error(mapped::decode_field_definition(bytes, record_offset), ErrorCode::IoError);
    }
    for (const auto count : {std::uint64_t{0}, std::uint64_t{3}, std::numeric_limits<std::uint64_t>::max()})
    {
      auto bytes = golden_enum();
      put_integer(bytes, 96, count, 8);
      expect_error(mapped::decode_field_definition(bytes, record_offset), ErrorCode::IoError);
    }
    for (const auto length : {std::uint64_t{7}, std::uint64_t{69}, std::uint64_t{71},
                               std::numeric_limits<std::uint64_t>::max()})
    {
      auto bytes = golden_enum();
      put_integer(bytes, 80, length, 8);
      expect_error(mapped::decode_field_definition(bytes, record_offset), ErrorCode::IoError);
    }
    for (const auto offset : {112u, 120u, 136u, 144u})
    {
      auto bytes = golden_enum();
      put_integer(bytes, offset, std::numeric_limits<std::uint64_t>::max(), 8);
      expect_error(mapped::decode_field_definition(bytes, record_offset), ErrorCode::IoError);
    }
  }

  // The stored signed number must already be canonical sign extension. Merely
  // masking the low byte would misread 0x80 as -128 or accept extra high bits.
  // Ordered unique numbers and labels are checked before returning a descriptor.
  TEST(LogicalMetadata, StoredEnumsRejectNoncanonicalNumbersOrderingAndLabels)
  {
    for (const auto value : {std::uint64_t{128}, std::uint64_t{0xff}, std::uint64_t{0x180},
                              std::uint64_t{127}})
    {
      auto bytes = golden_enum();
      put_integer(bytes, 104, value, 8);
      expect_error(mapped::decode_field_definition(bytes, record_offset), ErrorCode::IoError);
    }
    auto bytes = golden_enum();
    put_integer(bytes, 104, 127, 8);
    put_integer(bytes, 128, 0xffffffffffffff80u, 8);
    expect_error(mapped::decode_field_definition(bytes, record_offset), ErrorCode::IoError);
    bytes = golden_enum();
    std::copy_n(bytes.begin() + 152, 3, bytes.begin() + 155);
    expect_error(mapped::decode_field_definition(bytes, record_offset), ErrorCode::IoError);
    for (const auto invalid : {std::byte{0}, std::byte{0xff}})
    {
      bytes = golden_enum();
      bytes[152] = invalid;
      expect_error(mapped::decode_field_definition(bytes, record_offset), ErrorCode::IoError);
    }
    bytes = golden_enum();
    put_integer(bytes, 64, 2, 2);
    put_integer(bytes, 104, 0x100, 8);
    expect_error(mapped::decode_field_definition(bytes, record_offset), ErrorCode::IoError);
  }

  // The count is u64 rather than an implicit 16-bit ceiling. A large owned
  // definition remains bounded by actual record bytes and returns every entry
  // canonically, even when declarations arrive in reverse numeric order.
  TEST(LogicalMetadata, LargeEnumeratorTablesPreserveMoreThan65535Entries)
  {
    auto definition = enum_definition();
    auto& field = definition._descriptor;
    field._base = EFieldBase::BASE_UINT32;
    field._size = 4;
    auto& entries = field._logical._enumerators;
    entries.clear();
    for (std::uint64_t value = 65536; value != 0; --value)
    {
      entries.push_back(EnumeratorDescriptor{"V" + std::to_string(value - 1), value - 1});
    }
    const auto bytes = require_value(mapped::encode_field_definition(definition, record_offset));
    EXPECT_EQ(get_integer(bytes, 96, 8), 65536u);
    const auto read = require_value(mapped::decode_field_definition(bytes, record_offset));
    ASSERT_EQ(read._descriptor._logical._enumerators.size(), 65536u);
    EXPECT_EQ(read._descriptor._logical._enumerators.front()._name, "V0");
    EXPECT_EQ(read._descriptor._logical._enumerators.back()._name, "V65535");
    EXPECT_EQ(require_value(mapped::field_definition_body_bytes(field)), bytes.size());
  }
} // namespace
