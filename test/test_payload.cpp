// Binary payloads use the exact registered schema and explicit byte widths.
// The metadata-only fixtures below are owning public Snapshot values, not raw
// native-session edits. Golden bytes are written independently of the codec.
#include "angaria/payload.h"
#include "angaria/runtime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace fs = std::filesystem;

  const SchemaId sample_schema{static_cast<MessageType>(42), 1};

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
  void expect_error(const Result<T>& result, ErrorCode code, std::string_view path = {})
  {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    const auto& error = std::get<Error>(result);
    EXPECT_EQ(error._code, code) << error._path << ": " << error._message;
    EXPECT_FALSE(error._message.empty());
    EXPECT_FALSE(error._path.empty());
    if (!path.empty())
    {
      EXPECT_NE(error._path.find(path), std::string::npos) << error._path;
    }
  }

  std::vector<std::byte> octets(std::initializer_list<unsigned> values)
  {
    std::vector<std::byte> result;
    for (const auto value : values)
    {
      if (value > 255)
      {
        throw std::runtime_error("Invalid hand-authored golden byte");
      }
      result.push_back(static_cast<std::byte>(value));
    }
    return result;
  }

  FieldDescriptor field(EFieldBase base = EFieldBase::BASE_INT64, std::uint32_t size = 8,
                          std::uint32_t id = 0xee000001, std::string name = "PriceTicks")
  {
    FieldDescriptor descriptor;
    descriptor._id = static_cast<FieldId>(id);
    descriptor._name = std::move(name);
    descriptor._base = base;
    descriptor._size = size;
    descriptor._doc = "A fixed-width value";
    return descriptor;
  }

  RegistrySnapshot snapshot(std::vector<FieldDescriptor> fields, std::vector<FieldId> order)
  {
    RegistrySnapshot result;
    result._revision = 1;
    result._currentRevision = 1;
    const RegistrationMetadata metadata{TaskId{1}, UtcTimestamp{}, 1};
    TaskDescriptor setup;
    setup._name = "Setup";
    result._tasks.push_back(TaskSnapshot{TaskId{1}, setup, metadata});
    for (auto& descriptor : fields)
    {
      result._fields.push_back(FieldSnapshot{std::move(descriptor), metadata});
    }
    MessageDescriptor message;
    message._schema = sample_schema;
    message._name = "Sample";
    message._fields = std::move(order);
    message._doc = "Ordered scalar payload";
    result._messages.push_back(MessageSnapshot{std::move(message), metadata});
    return result;
  }

  RegistrySnapshot scalar_snapshot(EFieldBase base = EFieldBase::BASE_INT64, std::uint32_t size = 8)
  {
    const auto descriptor = field(base, size);
    return snapshot({descriptor}, {descriptor._id});
  }

  BinaryPayloadCodec codec(const RegistrySnapshot& definitions, std::uint64_t limit = 1024)
  {
    return require_value(BinaryPayloadCodec::from_snapshot(definitions, sample_schema, limit));
  }

  FieldDescriptor logical_field(FieldLogicalKind kind, EFieldBase base, std::uint32_t size,
                                   std::uint32_t id = 0xee000001, std::string name = "LogicalValue")
  {
    auto descriptor = field(base, size, id, std::move(name));
    descriptor._logical._kind = kind;
    return descriptor;
  }

  FieldDescriptor enum_field(EFieldBase base, std::uint32_t size,
                              std::vector<EnumeratorDescriptor> entries,
                              std::uint32_t id = 0xee000001, std::string name = "Side")
  {
    auto descriptor = logical_field(FieldLogicalKind::Enumeration, base, size, id, std::move(name));
    descriptor._logical._enumerators = std::move(entries);
    return descriptor;
  }

  RegistrySnapshot one_field_snapshot(FieldDescriptor descriptor)
  {
    const auto id = descriptor._id;
    return snapshot({std::move(descriptor)}, {id});
  }

  // The mixture deliberately places wide values after narrow ones. The 42
  // payload bytes have no C++ struct padding, Field IDs, or schema header.
  TEST(BinaryPayloadTest, EncodesAndDecodesAnIndependentGoldenPackedPayload)
  {
    const std::vector<FieldDescriptor> fields{
        field(EFieldBase::BASE_CHAR, 1, 0xee000000, "Character"),
        field(EFieldBase::BASE_BYTE, 1, 0xee000001, "Byte"),
        field(EFieldBase::BASE_INT16, 2, 0xee000002, "Signed16"),
        field(EFieldBase::BASE_UINT16, 2, 0xee000003, "Unsigned16"),
        field(EFieldBase::BASE_INT32, 4, 0xee000004, "Signed32"),
        field(EFieldBase::BASE_UINT32, 4, 0xee000005, "Unsigned32"),
        field(EFieldBase::BASE_INT64, 8, 0xee000006, "Signed64"),
        field(EFieldBase::BASE_UINT64, 8, 0xee000007, "Unsigned64"),
        field(EFieldBase::BASE_FLOAT32, 4, 0xee000008, "Float32"),
        field(EFieldBase::BASE_FLOAT64, 8, 0xeeffffff, "Float64")};
    std::vector<FieldId> order;
    for (const auto& descriptor : fields)
    {
      order.push_back(descriptor._id);
    }
    const auto encoder = codec(snapshot(fields, order));
    const std::vector<ScalarValue> values{
        char{'Z'}, std::uint8_t{0xa5}, std::int16_t{-2}, std::uint16_t{0xabcd},
        std::numeric_limits<std::int32_t>::min(), std::uint32_t{0x89abcdef},
        std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::uint64_t>::max(),
        1.0F, -2.5};
    const auto golden = octets({
        0x5a, 0xa5, 0xfe, 0xff, 0xcd, 0xab,
        0x00, 0x00, 0x00, 0x80, 0xef, 0xcd, 0xab, 0x89,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x00, 0x00, 0x80, 0x3f,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xc0});
    EXPECT_EQ(encoder.schema(), sample_schema);
    EXPECT_EQ(encoder.payload_size(), 42u);
    EXPECT_EQ(encoder.fields().size(), 10u);
    EXPECT_EQ(require_value(encoder.encode(values)), golden);
    // Decode the independent fixture, rather than only the encoder's output.
    EXPECT_EQ(require_value(encoder.decode(golden)), values);
  }

  TEST(BinaryPayloadTest, PreservesSignedExtremaAndUnsignedHighestBits)
  {
    struct IntegerCase
    {
      EFieldBase _base;
      std::uint32_t _size;
      ScalarValue _value;
      std::vector<std::byte> _golden;
    };
    const std::vector<IntegerCase> cases{
        {EFieldBase::BASE_INT16, 2, std::numeric_limits<std::int16_t>::min(), octets({0x00, 0x80})},
        {EFieldBase::BASE_INT16, 2, std::numeric_limits<std::int16_t>::max(), octets({0xff, 0x7f})},
        {EFieldBase::BASE_INT16, 2, std::int16_t{-1}, octets({0xff, 0xff})},
        {EFieldBase::BASE_INT32, 4, std::numeric_limits<std::int32_t>::min(), octets({0, 0, 0, 0x80})},
        {EFieldBase::BASE_INT32, 4, std::numeric_limits<std::int32_t>::max(), octets({0xff, 0xff, 0xff, 0x7f})},
        {EFieldBase::BASE_INT32, 4, std::int32_t{-1}, octets({0xff, 0xff, 0xff, 0xff})},
        {EFieldBase::BASE_INT64, 8, std::numeric_limits<std::int64_t>::min(), octets({0, 0, 0, 0, 0, 0, 0, 0x80})},
        {EFieldBase::BASE_INT64, 8, std::numeric_limits<std::int64_t>::max(), octets({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f})},
        {EFieldBase::BASE_INT64, 8, std::int64_t{-1}, octets({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff})},
        {EFieldBase::BASE_BYTE, 1, std::numeric_limits<std::uint8_t>::max(), octets({0xff})},
        {EFieldBase::BASE_UINT16, 2, std::numeric_limits<std::uint16_t>::max(), octets({0xff, 0xff})},
        {EFieldBase::BASE_UINT32, 4, std::numeric_limits<std::uint32_t>::max(), octets({0xff, 0xff, 0xff, 0xff})},
        {EFieldBase::BASE_UINT64, 8, std::numeric_limits<std::uint64_t>::max(), octets({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff})}};
    for (const auto& test : cases)
    {
      SCOPED_TRACE(static_cast<unsigned>(test._base));
      const auto encoder = codec(scalar_snapshot(test._base, test._size));
      const std::array values{test._value};
      EXPECT_EQ(require_value(encoder.encode(values)), test._golden);
      EXPECT_EQ(require_value(encoder.decode(test._golden)), (std::vector<ScalarValue>{test._value}));
    }
  }

  TEST(BinaryPayloadTest, TreatsCharAndByteAsDistinctRawOctetTypes)
  {
    const auto character = codec(scalar_snapshot(EFieldBase::BASE_CHAR, 1));
    const auto byte = codec(scalar_snapshot(EFieldBase::BASE_BYTE, 1));
    for (const auto bits : {std::uint8_t{0}, std::uint8_t{0x80}, std::uint8_t{0xff}})
    {
      const auto golden = octets({bits});
      const std::array<ScalarValue, 1> characters{std::bit_cast<char>(bits)};
      const std::array<ScalarValue, 1> bytes{bits};
      EXPECT_EQ(require_value(character.encode(characters)), golden);
      EXPECT_EQ(require_value(byte.encode(bytes)), golden);
      EXPECT_EQ(std::bit_cast<std::uint8_t>(std::get<char>(require_value(character.decode(golden))[0])), bits);
      EXPECT_EQ(std::get<std::uint8_t>(require_value(byte.decode(golden))[0]), bits);
      // A value's numeric range does not make it the other declared type.
      expect_error(character.encode(bytes), ErrorCode::InvalidArgument, "payload.values[0]");
      expect_error(byte.encode(characters), ErrorCode::InvalidArgument, "payload.values[0]");
    }
  }

  TEST(BinaryPayloadTest, PreservesIeeeFloatingPointBitsIncludingNanPayloads)
  {
    struct FloatCase
    {
      std::uint32_t _bits;
      std::vector<std::byte> _golden;
    };
    const std::vector<FloatCase> floats{
        {0x80000000u, octets({0, 0, 0, 0x80})},
        {0x7f800000u, octets({0, 0, 0x80, 0x7f})},
        {0xff800000u, octets({0, 0, 0x80, 0xff})},
        {0x7fc12345u, octets({0x45, 0x23, 0xc1, 0x7f})},
        {0x00000001u, octets({1, 0, 0, 0})},
        {0x7f7fffffu, octets({0xff, 0xff, 0x7f, 0x7f})}};
    const auto float_codec = codec(scalar_snapshot(EFieldBase::BASE_FLOAT32, 4));
    for (const auto& test : floats)
    {
      const std::array<ScalarValue, 1> values{std::bit_cast<float>(test._bits)};
      EXPECT_EQ(require_value(float_codec.encode(values)), test._golden);
      const auto decoded = require_value(float_codec.decode(test._golden));
      EXPECT_EQ(std::bit_cast<std::uint32_t>(std::get<float>(decoded[0])), test._bits);
    }
    struct DoubleCase
    {
      std::uint64_t _bits;
      std::vector<std::byte> _golden;
    };
    const std::vector<DoubleCase> doubles{
        {0x8000000000000000ull, octets({0, 0, 0, 0, 0, 0, 0, 0x80})},
        {0x7ff0000000000000ull, octets({0, 0, 0, 0, 0, 0, 0xf0, 0x7f})},
        {0xfff0000000000000ull, octets({0, 0, 0, 0, 0, 0, 0xf0, 0xff})},
        {0x7ff8123456789abcull, octets({0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12, 0xf8, 0x7f})},
        {0x0000000000000001ull, octets({1, 0, 0, 0, 0, 0, 0, 0})},
        {0x7fefffffffffffffull, octets({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xef, 0x7f})}};
    const auto double_codec = codec(scalar_snapshot(EFieldBase::BASE_FLOAT64, 8));
    for (const auto& test : doubles)
    {
      const std::array<ScalarValue, 1> values{std::bit_cast<double>(test._bits)};
      EXPECT_EQ(require_value(double_codec.encode(values)), test._golden);
      const auto decoded = require_value(double_codec.decode(test._golden));
      EXPECT_EQ(std::bit_cast<std::uint64_t>(std::get<double>(decoded[0])), test._bits);
    }
  }

  TEST(BinaryPayloadTest, UsesSchemaOrderAndRepeatedOccurrencesWithoutAlignmentPadding)
  {
    const auto price = field(EFieldBase::BASE_INT16, 2, 0xee000001, "Price");
    const auto quantity = field(EFieldBase::BASE_BYTE, 1, 0xee000002, "Quantity");
    // Catalog order differs from schema order, and Price occurs twice.
    const auto encoder = codec(snapshot({quantity, price}, {price._id, quantity._id, price._id}));
    const std::array<ScalarValue, 3> values{std::int16_t{0x1234}, std::uint8_t{0x7f}, std::int16_t{-2}};
    EXPECT_EQ(encoder.payload_size(), 5u);
    ASSERT_EQ(encoder.fields().size(), 3u);
    EXPECT_EQ(encoder.fields()[0]._id, price._id);
    EXPECT_EQ(encoder.fields()[1]._id, quantity._id);
    EXPECT_EQ(encoder.fields()[2]._id, price._id);
    EXPECT_EQ(require_value(encoder.encode(values)), octets({0x34, 0x12, 0x7f, 0xfe, 0xff}));
  }

  TEST(BinaryPayloadTest, PreservesFieldCountsBeyondSixteenBits)
  {
    const auto byte = field(EFieldBase::BASE_BYTE, 1);
    constexpr std::size_t count = 65536;
    const auto encoder = codec(snapshot({byte}, std::vector<FieldId>(count, byte._id)), count);
    EXPECT_EQ(encoder.fields().size(), count);
    EXPECT_EQ(encoder.payload_size(), count);
    const std::vector<ScalarValue> values(count, std::uint8_t{0xa5});
    const std::vector<std::byte> expected(count, std::byte{0xa5});
    EXPECT_EQ(require_value(encoder.encode(values)), expected);
    EXPECT_EQ(require_value(encoder.decode(expected)), values);
  }

  TEST(BinaryPayloadTest, EmptyControlSchemasNeedNoPayloadCapacity)
  {
    const auto encoder = codec(snapshot({}, {}), 0);
    EXPECT_EQ(encoder.payload_size(), 0u);
    EXPECT_TRUE(encoder.fields().empty());
    EXPECT_TRUE(require_value(encoder.encode({})).empty());
    EXPECT_TRUE(require_value(encoder.decode({})).empty());
    const std::array<ScalarValue, 1> value{std::uint8_t{1}};
    expect_error(encoder.encode(value), ErrorCode::InvalidArgument);
    expect_error(encoder.decode(octets({1})), ErrorCode::InvalidArgument, "payload.size");
  }

  TEST(BinaryPayloadTest, ChecksTheCompleteRepeatedFieldByteBudgetBeforeAllocation)
  {
    const auto price = field();
    const auto definitions = snapshot({price}, {price._id, price._id, price._id});
    expect_error(BinaryPayloadCodec::from_snapshot(definitions, sample_schema, 23),
                 ErrorCode::CapacityExceeded, "payload.capacity");
    expect_error(BinaryPayloadCodec::from_snapshot(definitions, sample_schema, 0),
                 ErrorCode::CapacityExceeded, "payload.capacity");
    EXPECT_EQ(codec(definitions, 24).payload_size(), 24u);
    // The limit is a bound, not a request to allocate that many bytes.
    EXPECT_EQ(codec(definitions, std::numeric_limits<std::uint64_t>::max()).payload_size(), 24u);
  }

  TEST(BinaryPayloadTest, RequiresAnExactNonzeroSchemaIdentity)
  {
    auto definitions = scalar_snapshot();
    auto version_two = definitions._messages.front();
    version_two._descriptor._schema._version = 2;
    definitions._messages.push_back(version_two);
    expect_error(BinaryPayloadCodec::from_snapshot(definitions, {MessageType::Invalid, 1}, 1024),
                 ErrorCode::InvalidArgument, "message.schema");
    expect_error(BinaryPayloadCodec::from_snapshot(definitions, {sample_schema._type, 0}, 1024),
                 ErrorCode::InvalidArgument, "message.schema");
    expect_error(BinaryPayloadCodec::from_snapshot(definitions, {sample_schema._type, 3}, 1024),
                 ErrorCode::NotFound, "message.schema");
    expect_error(BinaryPayloadCodec::from_snapshot(definitions, {static_cast<MessageType>(43), 1}, 1024),
                 ErrorCode::NotFound, "message.schema");
    definitions._messages.clear();
    expect_error(BinaryPayloadCodec::from_snapshot(definitions, sample_schema, 1024),
                 ErrorCode::NotFound, "message.schema");
  }

  // A caller may construct Snapshot values itself, so selected metadata must
  // be checked rather than presumed to have come from a validated live Session.
  TEST(BinaryPayloadTest, RejectsMalformedSelectedSchemasAndAmbiguousFamilies)
  {
    const auto valid = scalar_snapshot();
    for (const auto& bad_name : {std::string{}, std::string{"bad\0name", 8}, std::string(1, static_cast<char>(0xff))})
    {
      auto definitions = valid;
      definitions._messages[0]._descriptor._name = bad_name;
      expect_error(BinaryPayloadCodec::from_snapshot(definitions, sample_schema, 1024), ErrorCode::InvalidArgument);
    }
    auto duplicate = valid;
    duplicate._messages.push_back(duplicate._messages.front());
    expect_error(BinaryPayloadCodec::from_snapshot(duplicate, sample_schema, 1024), ErrorCode::InvalidArgument);
    auto renamed = valid;
    auto other_name = renamed._messages.front();
    other_name._descriptor._schema._version = 2;
    other_name._descriptor._name = "DifferentName";
    renamed._messages.push_back(other_name);
    expect_error(BinaryPayloadCodec::from_snapshot(renamed, sample_schema, 1024), ErrorCode::InvalidArgument);
    auto retyped = valid;
    auto other_type = retyped._messages.front();
    other_type._descriptor._schema._type = static_cast<MessageType>(43);
    retyped._messages.push_back(other_type);
    expect_error(BinaryPayloadCodec::from_snapshot(retyped, sample_schema, 1024), ErrorCode::InvalidArgument);
    auto bad_reference = valid;
    bad_reference._messages[0]._descriptor._fields[0] = static_cast<FieldId>(0xef000001);
    expect_error(BinaryPayloadCodec::from_snapshot(bad_reference, sample_schema, 1024), ErrorCode::InvalidArgument);
  }

  TEST(BinaryPayloadTest, RejectsMissingAndAmbiguousReferencedFieldDefinitions)
  {
    const auto valid = scalar_snapshot();
    auto missing = valid;
    missing._fields.clear();
    expect_error(BinaryPayloadCodec::from_snapshot(missing, sample_schema, 1024), ErrorCode::NotFound, "message.fields");
    auto duplicate = valid;
    duplicate._fields.push_back(duplicate._fields.front());
    expect_error(BinaryPayloadCodec::from_snapshot(duplicate, sample_schema, 1024), ErrorCode::InvalidArgument);
    auto renamed_duplicate = valid;
    auto renamed = renamed_duplicate._fields.front();
    renamed._descriptor._name = "AnotherName";
    renamed_duplicate._fields.push_back(renamed);
    expect_error(BinaryPayloadCodec::from_snapshot(renamed_duplicate, sample_schema, 1024), ErrorCode::InvalidArgument);
    auto same_name = valid;
    auto alias = same_name._fields.front();
    alias._descriptor._id = static_cast<FieldId>(0xee000002);
    same_name._fields.push_back(alias);
    same_name._messages[0]._descriptor._fields.push_back(alias._descriptor._id);
    expect_error(BinaryPayloadCodec::from_snapshot(same_name, sample_schema, 1024), ErrorCode::InvalidArgument);
  }

  TEST(BinaryPayloadTest, RejectsBadReferencedFieldMetadataAndScalarWidths)
  {
    const auto valid = scalar_snapshot();
    for (const auto size : {0u, 1u, 7u, 9u, std::numeric_limits<std::uint32_t>::max()})
    {
      auto definitions = valid;
      definitions._fields[0]._descriptor._size = size;
      expect_error(BinaryPayloadCodec::from_snapshot(definitions, sample_schema, 1024), ErrorCode::InvalidArgument);
    }
    for (const auto& bad_name : {std::string{}, std::string{"bad\0name", 8}, std::string(1, static_cast<char>(0xff))})
    {
      auto definitions = valid;
      definitions._fields[0]._descriptor._name = bad_name;
      expect_error(BinaryPayloadCodec::from_snapshot(definitions, sample_schema, 1024), ErrorCode::InvalidArgument);
    }
    auto bad_doc = valid;
    bad_doc._fields[0]._descriptor._doc = std::string(1, static_cast<char>(0xff));
    expect_error(BinaryPayloadCodec::from_snapshot(bad_doc, sample_schema, 1024), ErrorCode::InvalidArgument);
  }

  TEST(BinaryPayloadTest, DistinguishesRecognizedFutureRepresentationsFromUnknownBases)
  {
    for (const auto base : {EFieldBase::BASE_DATE, EFieldBase::BASE_TIMESTAMP, EFieldBase::BASE_SECS,
                           EFieldBase::BASE_CHARS, EFieldBase::BASE_BYTES, EFieldBase::BASE_SYMBOL, EFieldBase::BASE_STRING})
    {
      SCOPED_TRACE(static_cast<unsigned>(base));
      expect_error(BinaryPayloadCodec::from_snapshot(scalar_snapshot(base, 0), sample_schema, 1024),
                   ErrorCode::UnsupportedPolicy);
    }
    expect_error(BinaryPayloadCodec::from_snapshot(scalar_snapshot(static_cast<EFieldBase>(0xffff), 8), sample_schema, 1024),
                 ErrorCode::InvalidArgument);
  }

  TEST(BinaryPayloadTest, EncodingRequiresExactValueCountAndVariantAlternatives)
  {
    const auto encoder = codec(scalar_snapshot());
    expect_error(encoder.encode({}), ErrorCode::InvalidArgument);
    const std::array<ScalarValue, 2> extra{std::int64_t{1}, std::int64_t{2}};
    expect_error(encoder.encode(extra), ErrorCode::InvalidArgument);
    const std::vector<ScalarValue> wrong_types{
        char{1}, std::uint8_t{1}, std::int16_t{1}, std::uint16_t{1}, std::int32_t{1},
        std::uint32_t{1}, std::uint64_t{1}, 1.0F, 1.0};
    for (const auto& value : wrong_types)
    {
      const std::array values{value};
      expect_error(encoder.encode(values), ErrorCode::InvalidArgument, "payload.values[0]");
    }
    const std::array<ScalarValue, 1> correct{std::int64_t{1}};
    EXPECT_EQ(require_value(encoder.encode(correct)), octets({1, 0, 0, 0, 0, 0, 0, 0}));
  }

  TEST(BinaryPayloadTest, DecodingRejectsBothTruncationAndTrailingBytes)
  {
    const auto encoder = codec(scalar_snapshot());
    for (const auto length : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{9}, std::size_t{16}})
    {
      const std::vector<std::byte> bytes(length);
      expect_error(encoder.decode(bytes), ErrorCode::InvalidArgument, "payload.size");
    }
    EXPECT_EQ(require_value(encoder.decode(octets({0, 0, 0, 0, 0, 0, 0, 0}))),
              (std::vector<ScalarValue>{std::int64_t{0}}));
  }

  TEST(BinaryPayloadTest, OwnsResolvedDefinitionsAndMaintainsValidMoveState)
  {
    auto definitions = scalar_snapshot();
    // A codec needs its selected family and referenced Fields, not unrelated
    // data a caller may have put in this process-local catalog value.
    definitions._fields.push_back(FieldSnapshot{field(static_cast<EFieldBase>(0xffff), 0, 0xee000010, "Unrelated"), {}});
    MessageDescriptor unrelated;
    unrelated._schema = {static_cast<MessageType>(99), 0};
    unrelated._name = "Unrelated";
    definitions._messages.push_back(MessageSnapshot{unrelated, {}});
    auto original = codec(definitions);
    definitions._fields[0]._descriptor._name = "ChangedAfterConstruction";
    definitions._fields[0]._descriptor._size = 1;
    definitions._messages[0]._descriptor._fields.clear();
    auto moved = std::move(original);
    const std::array<ScalarValue, 1> values{std::int64_t{5}};
    EXPECT_EQ(moved.fields()[0]._name, "PriceTicks");
    EXPECT_EQ(moved.payload_size(), 8u);
    EXPECT_EQ(require_value(moved.encode(values)), octets({5, 0, 0, 0, 0, 0, 0, 0}));
    expect_error(original.encode(values), ErrorCode::InvalidState, "payload.codec");
    expect_error(original.decode(octets({5, 0, 0, 0, 0, 0, 0, 0})), ErrorCode::InvalidState, "payload.codec");

    const auto byte = field(EFieldBase::BASE_BYTE, 1, 0xee000002, "Byte");
    const auto short_value = field(EFieldBase::BASE_UINT16, 2, 0xee000003, "Short");
    auto alternative = snapshot({byte, short_value}, {short_value._id, byte._id});
    alternative._messages[0]._descriptor._schema._version = 2;
    auto assigned = require_value(BinaryPayloadCodec::from_snapshot(
        alternative, {sample_schema._type, 2}, 3));
    EXPECT_EQ(assigned.payload_size(), 3u);
    // Copy assignment must switch the identity, Field layout, and byte count
    // together, leaving both the copied source and a self-assigned target usable.
    assigned = moved;
    EXPECT_EQ(assigned.schema(), sample_schema);
    EXPECT_EQ(assigned.payload_size(), 8u);
    ASSERT_EQ(assigned.fields().size(), 1u);
    EXPECT_EQ(assigned.fields()[0]._base, EFieldBase::BASE_INT64);
    auto* self = &assigned;
    assigned = *self;
    EXPECT_EQ(require_value(assigned.encode(values)), octets({5, 0, 0, 0, 0, 0, 0, 0}));
    EXPECT_EQ(require_value(moved.encode(values)), octets({5, 0, 0, 0, 0, 0, 0, 0}));
  }

  // Logical Fields carry meaning in addition to an integer storage width.
  // These fixtures remain public owning snapshots. Their bytes are hand-written
  // rather than obtained from the encoder under test.
  TEST(LogicalPayloadTest, EncodesAnIndependentMixedLogicalFixtureWithoutPadding)
  {
    const auto enabled = logical_field(FieldLogicalKind::Boolean, EFieldBase::BASE_BYTE, 1,
                                        0xee000001, "Enabled");
    const auto flags = logical_field(FieldLogicalKind::Bits, EFieldBase::BASE_UINT16, 2,
                                      0xee000002, "Flags");
    const auto side = enum_field(EFieldBase::BASE_INT8, 1,
        {{"Buy", std::int64_t{1}}, {"Sell", std::int64_t{-1}}}, 0xee000003);
    const auto code = logical_field(FieldLogicalKind::FixedAscii, EFieldBase::BASE_CHARS, 4,
                                     0xee000004, "Code");
    const auto offset = field(EFieldBase::BASE_INT8, 1, 0xee000005, "Offset");
    const auto definitions = snapshot({offset, code, side, flags, enabled},
        {enabled._id, flags._id, side._id, code._id, offset._id});
    const auto encoder = codec(definitions, 9);
    const std::vector<PayloadValue> values{
        true, BitFieldValue{0xa501}, EnumValue{std::int8_t{-1}},
        FixedAsciiValue{std::string{"AB\0 ", 4}}, std::numeric_limits<std::int8_t>::min()};
    const auto golden = octets({1, 1, 0xa5, 0xff, 'A', 'B', 0, ' ', 0x80});
    EXPECT_EQ(encoder.payload_size(), 9u);
    EXPECT_EQ(require_value(encoder.encode(values)), golden);
    EXPECT_EQ(require_value(encoder.decode(golden)), values);
    EXPECT_EQ(encoder.fields()[2]._logical._enumerators.front()._name, "Sell");
    expect_error(BinaryPayloadCodec::from_snapshot(definitions, sample_schema, 8),
                 ErrorCode::CapacityExceeded, "payload.capacity");
  }

  TEST(LogicalPayloadTest, BooleanAndOrdinaryByteRemainDistinctEvenWhenTheirNamesSuggestMeaning)
  {
    const auto boolean = logical_field(FieldLogicalKind::Boolean, EFieldBase::BASE_BYTE, 1);
    const auto encoder = codec(one_field_snapshot(boolean));
    EXPECT_EQ(require_value(encoder.encode({false})), octets({0}));
    EXPECT_EQ(require_value(encoder.encode({true})), octets({1}));
    EXPECT_EQ(require_value(encoder.decode(octets({0}))), (std::vector<PayloadValue>{false}));
    EXPECT_EQ(require_value(encoder.decode(octets({1}))), (std::vector<PayloadValue>{true}));
    for (const auto raw : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{2}})
    {
      expect_error(encoder.encode({raw}), ErrorCode::InvalidArgument, "payload.values[0]");
    }
    for (const auto raw : {2u, 0x80u, 0xffu})
    {
      expect_error(encoder.decode(octets({raw})), ErrorCode::InvalidArgument, "payload.values[0]");
    }
    const auto ordinary = codec(one_field_snapshot(field(EFieldBase::BASE_BYTE, 1,
        0xee000001, "TradingEnabled")));
    EXPECT_EQ(require_value(ordinary.encode({std::uint8_t{0xff}})), octets({0xff}));
    expect_error(ordinary.encode({true}), ErrorCode::InvalidArgument);
  }

  TEST(LogicalPayloadTest, BitFieldsUseEachDeclaredWidthAndRejectValuesOutsideIt)
  {
    struct BitCase
    {
      EFieldBase _base;
      std::uint32_t _size;
      std::uint64_t _value;
      std::vector<std::byte> _golden;
    };
    const std::vector<BitCase> cases{
        {EFieldBase::BASE_BYTE, 1, 0x80, octets({0x80})},
        {EFieldBase::BASE_UINT16, 2, 0x8001, octets({1, 0x80})},
        {EFieldBase::BASE_UINT32, 4, 0x80000001, octets({1, 0, 0, 0x80})},
        {EFieldBase::BASE_UINT64, 8, 0x8000000000000001ull, octets({1, 0, 0, 0, 0, 0, 0, 0x80})}};
    for (const auto& test : cases)
    {
      SCOPED_TRACE(test._size);
      const auto encoder = codec(one_field_snapshot(logical_field(FieldLogicalKind::Bits, test._base, test._size)));
      EXPECT_EQ(require_value(encoder.encode({BitFieldValue{test._value}})), test._golden);
      EXPECT_EQ(require_value(encoder.decode(test._golden)),
                (std::vector<PayloadValue>{BitFieldValue{test._value}}));
      expect_error(encoder.encode({std::uint64_t{test._value}}), ErrorCode::InvalidArgument);
      if (test._size < 8)
      {
        expect_error(encoder.encode({BitFieldValue{std::uint64_t{1} << (test._size * 8)}}),
                     ErrorCode::InvalidArgument);
      }
      else
      {
        EXPECT_EQ(require_value(encoder.encode({BitFieldValue{std::numeric_limits<std::uint64_t>::max()}})),
                  octets({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}));
      }
    }
  }

  TEST(LogicalPayloadTest, SignedEightBitScalarsHaveIndependentTwosComplementBytes)
  {
    const auto encoder = codec(scalar_snapshot(EFieldBase::BASE_INT8, 1));
    const std::array<std::int8_t, 4> values{
        std::numeric_limits<std::int8_t>::min(), std::int8_t{-1}, std::int8_t{0},
        std::numeric_limits<std::int8_t>::max()};
    const std::array<unsigned, 4> bytes{0x80, 0xff, 0, 0x7f};
    for (std::size_t index = 0; index < values.size(); ++index)
    {
      EXPECT_EQ(require_value(encoder.encode({values[index]})), octets({bytes[index]}));
      EXPECT_EQ(require_value(encoder.decode(octets({bytes[index]}))),
                (std::vector<PayloadValue>{values[index]}));
    }
    expect_error(encoder.encode({std::uint8_t{1}}), ErrorCode::InvalidArgument);
    expect_error(encoder.encode({char{1}}), ErrorCode::InvalidArgument);
    expect_error(encoder.encode({std::int16_t{1}}), ErrorCode::InvalidArgument);
  }

  TEST(LogicalPayloadTest, EnumStoragePreservesEveryIntegerWidthAndItsExactVariant)
  {
    struct EnumCase
    {
      EFieldBase _base;
      std::uint32_t _size;
      IntegerValue _value;
      EnumerationNumber _metadata;
      std::vector<std::byte> _golden;
    };
    const std::vector<EnumCase> cases{
        {EFieldBase::BASE_INT8, 1, std::int8_t{-128}, std::int64_t{-128}, octets({0x80})},
        {EFieldBase::BASE_BYTE, 1, std::uint8_t{255}, std::uint64_t{255}, octets({0xff})},
        {EFieldBase::BASE_INT16, 2, std::int16_t{-32768}, std::int64_t{-32768}, octets({0, 0x80})},
        {EFieldBase::BASE_UINT16, 2, std::uint16_t{65535}, std::uint64_t{65535}, octets({0xff, 0xff})},
        {EFieldBase::BASE_INT32, 4, std::numeric_limits<std::int32_t>::min(),
         std::int64_t{-2147483648ll}, octets({0, 0, 0, 0x80})},
        {EFieldBase::BASE_UINT32, 4, std::uint32_t{0x80000000u},
         std::uint64_t{0x80000000u}, octets({0, 0, 0, 0x80})},
        {EFieldBase::BASE_INT64, 8, std::numeric_limits<std::int64_t>::min(),
         std::numeric_limits<std::int64_t>::min(), octets({0, 0, 0, 0, 0, 0, 0, 0x80})},
        {EFieldBase::BASE_UINT64, 8, std::numeric_limits<std::uint64_t>::max(),
         std::numeric_limits<std::uint64_t>::max(), octets({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff})}};
    for (const auto& test : cases)
    {
      SCOPED_TRACE(static_cast<unsigned>(test._base));
      const auto encoder = codec(one_field_snapshot(enum_field(test._base, test._size, {{"Boundary", test._metadata}})));
      const std::vector<PayloadValue> values{EnumValue{test._value}};
      EXPECT_EQ(require_value(encoder.encode(values)), test._golden);
      EXPECT_EQ(require_value(encoder.decode(test._golden)), values);
    }
  }

  TEST(LogicalPayloadTest, ClosedEnumsRejectUnnamedValuesAndWrongUnderlyingTypes)
  {
    const auto encoder = codec(one_field_snapshot(enum_field(EFieldBase::BASE_BYTE, 1,
        {{"Sell", std::uint64_t{2}}, {"Buy", std::uint64_t{1}}})));
    EXPECT_EQ(require_value(encoder.encode({EnumValue{std::uint8_t{1}}})), octets({1}));
    expect_error(encoder.encode({std::uint8_t{1}}), ErrorCode::InvalidArgument);
    expect_error(encoder.encode({EnumValue{std::uint16_t{1}}}), ErrorCode::InvalidArgument);
    expect_error(encoder.encode({EnumValue{std::int8_t{1}}}), ErrorCode::InvalidArgument);
    expect_error(encoder.encode({EnumValue{std::uint8_t{3}}}), ErrorCode::InvalidArgument);
    for (const auto unknown : {0u, 3u, 0xffu})
    {
      expect_error(encoder.decode(octets({unknown})), ErrorCode::InvalidArgument, "payload.values[0]");
    }
    ASSERT_EQ(encoder.fields()[0]._logical._enumerators.size(), 2u);
    EXPECT_EQ(encoder.fields()[0]._logical._enumerators[0]._name, "Buy");
    EXPECT_EQ(encoder.fields()[0]._logical._enumerators[1]._name, "Sell");
  }

  TEST(LogicalPayloadTest, FixedAsciiPreservesControlBytesAndCallerPaddingWithoutCoercion)
  {
    const auto encoder = codec(one_field_snapshot(logical_field(FieldLogicalKind::FixedAscii,
        EFieldBase::BASE_CHARS, 4)));
    const FixedAsciiValue text{std::string{'A', '\n', '\0', '\x7f'}};
    const auto golden = octets({'A', 10, 0, 0x7f});
    EXPECT_EQ(require_value(encoder.encode({text})), golden);
    EXPECT_EQ(require_value(encoder.decode(golden)), (std::vector<PayloadValue>{text}));
    EXPECT_EQ(require_value(encoder.encode({FixedAsciiValue{"AB  "}})), octets({'A', 'B', ' ', ' '}));
    for (const auto& invalid : {std::string{}, std::string{"AB"}, std::string{"ABCDE"},
                                std::string{'A', 'B', '\0', static_cast<char>(0x80)},
                                std::string{"\xc3\xa9" "AB", 4}})
    {
      expect_error(encoder.encode({FixedAsciiValue{invalid}}), ErrorCode::InvalidArgument);
    }
    expect_error(encoder.decode(octets({'A', 'B', 0x80, 0})), ErrorCode::InvalidArgument);
    expect_error(encoder.decode(octets({0xc3, 0xa9, 'A', 'B'})), ErrorCode::InvalidArgument);
    expect_error(encoder.encode({char{'A'}}), ErrorCode::InvalidArgument);
  }

  TEST(LogicalPayloadTest, BoundedConstructionLeavesCallerBytesUnchangedOnEveryInputFailure)
  {
    const auto enabled = logical_field(FieldLogicalKind::Boolean, EFieldBase::BASE_BYTE, 1,
        0xee000001, "Enabled");
    const auto code = logical_field(FieldLogicalKind::FixedAscii, EFieldBase::BASE_CHARS, 2,
        0xee000002, "Code");
    const auto encoder = codec(snapshot({enabled, code}, {enabled._id, code._id}), 3);
    std::array<std::byte, 3> output{std::byte{0xa5}, std::byte{0xa5}, std::byte{0xa5}};
    const auto original = output;
    const std::array<PayloadValue, 2> invalid_late{true, FixedAsciiValue{"A"}};
    expect_error(encoder.encode_into(invalid_late, output), ErrorCode::InvalidArgument);
    EXPECT_EQ(output, original);
    const std::array<PayloadValue, 2> invalid_early{std::uint8_t{1}, FixedAsciiValue{"AB"}};
    expect_error(encoder.encode_into(invalid_early, output), ErrorCode::InvalidArgument);
    EXPECT_EQ(output, original);
    const std::array<PayloadValue, 2> correct{true, FixedAsciiValue{"AB"}};
    expect_error(encoder.encode_into(correct, std::span<std::byte>{output}.first(2)),
                 ErrorCode::InvalidArgument, "payload.output_size");
    EXPECT_EQ(output, original);
    require_value(encoder.encode_into(correct, output));
    EXPECT_EQ((std::vector<std::byte>{output.begin(), output.end()}), octets({1, 'A', 'B'}));
    // Initializer-list construction also validates the complete declared bound.
    EXPECT_EQ(require_value(encoder.encode({false, FixedAsciiValue{"CD"}})), octets({0, 'C', 'D'}));
    const auto empty = codec(snapshot({}, {}), 0);
    require_value(empty.encode_into({}, {}));
  }

  TEST(LogicalPayloadTest, RepeatedLogicalFieldsKeepOrderAndExactByteBudgets)
  {
    const auto value = logical_field(FieldLogicalKind::Boolean, EFieldBase::BASE_BYTE, 1);
    const auto definitions = snapshot({value}, {value._id, value._id, value._id});
    const auto encoder = codec(definitions, 3);
    EXPECT_EQ(require_value(encoder.encode({true, false, true})), octets({1, 0, 1}));
    EXPECT_EQ(require_value(encoder.decode(octets({1, 0, 1}))),
              (std::vector<PayloadValue>{true, false, true}));
    expect_error(BinaryPayloadCodec::from_snapshot(definitions, sample_schema, 2), ErrorCode::CapacityExceeded);
    auto huge_ascii = logical_field(FieldLogicalKind::FixedAscii, EFieldBase::BASE_CHARS,
        std::numeric_limits<std::uint32_t>::max());
    expect_error(BinaryPayloadCodec::from_snapshot(one_field_snapshot(huge_ascii), sample_schema, 16),
                 ErrorCode::CapacityExceeded, "payload.capacity");
  }

  TEST(LogicalPayloadTest, SnapshotCompilationRejectsInvalidLogicalDefinitions)
  {
    std::vector<FieldDescriptor> invalid{
        logical_field(FieldLogicalKind::Boolean, EFieldBase::BASE_BYTE, 2),
        logical_field(FieldLogicalKind::Bits, EFieldBase::BASE_UINT32, 3),
        logical_field(FieldLogicalKind::Bits, EFieldBase::BASE_INT16, 2),
        logical_field(FieldLogicalKind::FixedAscii, EFieldBase::BASE_CHARS, 0),
        logical_field(static_cast<FieldLogicalKind>(0xffff), EFieldBase::BASE_BYTE, 1),
        enum_field(EFieldBase::BASE_BYTE, 1, {}),
        enum_field(EFieldBase::BASE_BYTE, 1, {{"Buy", std::uint64_t{1}}, {"Alias", std::uint64_t{1}}}),
        enum_field(EFieldBase::BASE_BYTE, 1, {{"Buy", std::uint64_t{1}}, {"Buy", std::uint64_t{2}}}),
        enum_field(EFieldBase::BASE_BYTE, 1, {{"Negative", std::int64_t{-1}}})};
    for (const auto& descriptor : invalid)
    {
      expect_error(BinaryPayloadCodec::from_snapshot(one_field_snapshot(descriptor), sample_schema, 1024),
                   ErrorCode::InvalidArgument);
    }
  }

  TEST(LogicalPayloadTest, CompiledEnumMetadataOwnsItsLabelsAcrossCopiesAndMoves)
  {
    auto definitions = one_field_snapshot(enum_field(EFieldBase::BASE_INT16, 2,
        {{"Positive", std::int64_t{5}}, {"Negative", std::int64_t{-2}}}));
    auto original = codec(definitions);
    definitions._fields[0]._descriptor._logical._enumerators.clear();
    auto copy = original;
    auto moved = std::move(original);
    EXPECT_EQ(copy.fields()[0]._logical._enumerators.front()._name, "Negative");
    EXPECT_EQ(moved.fields()[0]._logical._enumerators.back()._name, "Positive");
    EXPECT_EQ(require_value(copy.encode({EnumValue{std::int16_t{-2}}})), octets({0xfe, 0xff}));
    EXPECT_EQ(require_value(moved.decode(octets({5, 0}))),
              (std::vector<PayloadValue>{EnumValue{std::int16_t{5}}}));
    std::array<std::byte, 2> output{std::byte{0xa5}, std::byte{0xa5}};
    const auto before = output;
    const std::array<PayloadValue, 1> values{EnumValue{std::int16_t{5}}};
    expect_error(original.encode_into(values, output), ErrorCode::InvalidState, "payload.codec");
    EXPECT_EQ(output, before);
  }

  class PayloadRegistryTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-payload-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    CreatedSession create() const
    {
      SessionOptions options;
      options._name = "Payload examples";
      options._directory = _root / "session";
      options._maxLogBytes = 1048576;
      options._limits = CapacityLimits::empty().tasks(2).fields(4).message_schemas(4);
      TaskDescriptor creator;
      creator._name = "Setup";
      return require_value(Session::create(options, creator));
    }

    static SchemaId register_quote(Registry actor)
    {
      const auto price = field();
      require_value(actor.register_field(price));
      MessageDescriptor quote;
      quote._schema = sample_schema;
      quote._name = "Quote";
      quote._fields = {price._id};
      return require_value(actor.register_message(quote));
    }

    fs::path _root;
  };

  TEST_F(PayloadRegistryTest, ExactVersionsRemainUsableAfterSessionClosureWithoutRetainingClaims)
  {
    std::optional<BinaryPayloadCodec> older;
    std::optional<BinaryPayloadCodec> newer;
    TaskId setup;
    {
      auto created = create();
      setup = created._creator.claim()._task;
      const auto version_one = register_quote(created._creator.registry());
      older.emplace(require_value(BinaryPayloadCodec::from_registry(created._creator.registry(), version_one, 1024)));
      const auto quantity = field(EFieldBase::BASE_BYTE, 1, 0xee000002, "Quantity");
      require_value(created._creator.registry().register_field(quantity));
      auto quote = require_value(created._session.registry().describe_message(version_one));
      quote._schema._version = 2;
      quote._fields.push_back(quantity._id);
      const auto version_two = require_value(created._creator.registry().register_message(quote));
      newer.emplace(require_value(BinaryPayloadCodec::from_registry(created._session.registry(), version_two, 1024)));
      EXPECT_EQ(older->schema(), version_one);
      EXPECT_EQ(newer->schema(), version_two);
      EXPECT_EQ(older->payload_size(), 8u);
      EXPECT_EQ(newer->payload_size(), 9u);
    }
    // Both the Session and its actor-bound Registry have ended. The codecs own
    // ordinary values, so another claimant can replace Setup while they exist.
    {
      auto attached = require_value(Session::attach(_root / "session"));
      auto replacement = require_value(attached.attach_task(setup));
      EXPECT_EQ(replacement.claim()._incarnation, 2u);
    }
    const std::array<ScalarValue, 1> first{std::int64_t{12345}};
    const std::array<ScalarValue, 2> second{std::int64_t{12345}, std::uint8_t{7}};
    EXPECT_EQ(require_value(older->encode(first)), octets({0x39, 0x30, 0, 0, 0, 0, 0, 0}));
    EXPECT_EQ(require_value(newer->encode(second)), octets({0x39, 0x30, 0, 0, 0, 0, 0, 0, 7}));
  }

  TEST_F(PayloadRegistryTest, HistoricalSnapshotsResolveOnlySchemasRegisteredAtTheirCut)
  {
    auto created = create();
    const auto version_one = register_quote(created._creator.registry());
    const auto reader = created._session.registry();
    const auto old = require_value(reader.snapshot());
    auto quote = require_value(reader.describe_message(version_one));
    quote._schema._version = 2;
    quote._fields.push_back(quote._fields.front());
    const auto version_two = require_value(created._creator.registry().register_message(quote));
    const auto historical = require_value(reader.snapshot(old._revision));
    const auto old_codec = require_value(BinaryPayloadCodec::from_snapshot(historical, version_one, 1024));
    EXPECT_EQ(old_codec.payload_size(), 8u);
    expect_error(BinaryPayloadCodec::from_snapshot(historical, version_two, 1024), ErrorCode::NotFound, "message.schema");
    const auto new_codec = require_value(BinaryPayloadCodec::from_registry(reader, version_two, 1024));
    EXPECT_EQ(new_codec.payload_size(), 16u);
    expect_error(BinaryPayloadCodec::from_registry(Registry{}, version_one, 1024), ErrorCode::InvalidState);
  }

  TEST_F(PayloadRegistryTest, LogicalDefinitionsRetainExactHistoricalMeaningAfterAllHandlesClose)
  {
    std::optional<BinaryPayloadCodec> older;
    std::optional<BinaryPayloadCodec> newer;
    RegistrySnapshot historical;
    TaskId setup;
    {
      auto created = create();
      setup = created._creator.claim()._task;
      auto actor = created._creator.registry();
      auto side = enum_field(EFieldBase::BASE_BYTE, 1,
          {{"Sell", std::uint64_t{2}}, {"Buy", std::uint64_t{1}}});
      const auto first_field = require_value(actor.register_field(side));
      MessageDescriptor status;
      status._schema = sample_schema;
      status._name = "Status";
      status._fields = {first_field};
      const auto first_schema = require_value(actor.register_message(status));
      historical = require_value(created._session.registry().snapshot());
      older.emplace(require_value(BinaryPayloadCodec::from_registry(actor, first_schema, 1)));

      // Enum declaration order is not meaning. Retrying an equivalent mapping
      // preserves the Registry revision, but adding a value under that identity
      // is a conflict and cannot change the old codec or retained schema.
      std::reverse(side._logical._enumerators.begin(), side._logical._enumerators.end());
      EXPECT_EQ(require_value(actor.register_field(side)), first_field);
      EXPECT_EQ(require_value(created._session.registry().snapshot())._revision, historical._revision);
      side._logical._enumerators.push_back({"Auction", std::uint64_t{3}});
      expect_error(actor.register_field(side), ErrorCode::Conflict);
      EXPECT_EQ(require_value(created._session.registry().snapshot())._revision, historical._revision);

      side._id = static_cast<FieldId>(0xee000002);
      side._name = "SideV2";
      const auto second_field = require_value(actor.register_field(side));
      status._schema._version = 2;
      status._fields = {second_field};
      const auto second_schema = require_value(actor.register_message(status));
      newer.emplace(require_value(BinaryPayloadCodec::from_registry(actor, second_schema, 1)));
    }
    {
      auto attached = require_value(Session::attach(_root / "session"));
      auto replacement = require_value(attached.attach_task(setup));
      EXPECT_EQ(replacement.claim()._incarnation, 2u);
    }
    const auto retained = codec(historical, 1);
    EXPECT_EQ(require_value(retained.encode({EnumValue{std::uint8_t{2}}})), octets({2}));
    expect_error(older->decode(octets({3})), ErrorCode::InvalidArgument);
    EXPECT_EQ(require_value(newer->decode(octets({3}))),
              (std::vector<PayloadValue>{EnumValue{std::uint8_t{3}}}));
    EXPECT_EQ(older->fields()[0]._logical._enumerators[0]._name, "Buy");
    EXPECT_EQ(newer->fields()[0]._logical._enumerators[2]._name, "Auction");
    expect_error(BinaryPayloadCodec::from_snapshot(historical, {sample_schema._type, 2}, 1),
                 ErrorCode::NotFound, "message.schema");
  }
} // namespace
