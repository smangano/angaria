// Generic value inspection uses only retained Registry metadata, not application
// C++ message types. Most cases use explicitly fabricated owning public snapshots;
// the final cases separately exercise real native Sessions and claim lifetimes.
#include "result_assertions.h"
#include "angaria/inspection.h"
#include "angaria/runtime.h"
#include "mapped_superblock.h"
#include "native_file.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <locale>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  constexpr SessionId source_session{1, 2};
  constexpr SchemaId quote_schema{static_cast<MessageType>(1), 1};


  using angaria::test_support::expect_error;

  FieldDescriptor field(unsigned ordinal, std::string name, EFieldBase base, std::uint32_t bytes,
                          FieldLogicalKind kind = FieldLogicalKind::None)
  {
    FieldDescriptor result;
    result._id = static_cast<FieldId>(0xee000000u + ordinal);
    result._name = std::move(name);
    result._base = base;
    result._size = bytes;
    result._logical._kind = kind;
    return result;
  }

  RegistrySnapshot catalog(std::vector<FieldDescriptor> fields, std::string name = "Quote")
  {
    RegistrySnapshot result;
    result._revision = result._currentRevision = 1;
    MessageDescriptor message;
    message._name = std::move(name);
    message._schema = quote_schema;
    for (auto& descriptor : fields)
    {
      message._fields.push_back(descriptor._id);
      result._fields.push_back({std::move(descriptor), {}});
    }
    result._messages.push_back({std::move(message), {}});
    return result;
  }

  MessageHeader header(std::size_t payloadBytes, SchemaId schema = quote_schema,
                          SessionId session = source_session)
  {
    MessageHeader result;
    result._schema = schema;
    result._publication = {{session, TaskId{1}, 1}, 1};
    result._publishedAt = UtcTimestamp{std::chrono::nanoseconds{42}};
    result._payloadBytes = payloadBytes;
    return result;
  }

  std::vector<std::byte> encode(const RegistrySnapshot& definitions,
                                  std::initializer_list<PayloadValue> values,
                                  SchemaId schema = quote_schema)
  {
    const auto codec = angaria::test_support::require_value(BinaryPayloadCodec::from_snapshot(definitions, schema, 1048576));
    const auto payload = angaria::test_support::require_value(codec.encode(values));
    return angaria::test_support::require_value(MessageRecordCodec::encode(header(payload.size(), schema), payload, 1048576));
  }

  std::string heading(std::string name = "Quote")
  {
    return "message \"" + name + "\" session=00000000000000010000000000000002 type=1 version=1 "
        "producer=1 incarnation=1 sequence=1 time_ns=42 flags=0x00000000\n";
  }

  TEST(MessageInspection, RendersEveryScalarWithoutApplicationMessageTypes)
  {
    const auto definitions = catalog({
        field(1, "Character", EFieldBase::BASE_CHAR, 1), field(2, "Byte", EFieldBase::BASE_BYTE, 1),
        field(3, "Small", EFieldBase::BASE_INT8, 1), field(4, "Signed16", EFieldBase::BASE_INT16, 2),
        field(5, "Unsigned16", EFieldBase::BASE_UINT16, 2), field(6, "Signed32", EFieldBase::BASE_INT32, 4),
        field(7, "Unsigned32", EFieldBase::BASE_UINT32, 4), field(8, "Signed64", EFieldBase::BASE_INT64, 8),
        field(9, "Unsigned64", EFieldBase::BASE_UINT64, 8), field(10, "Float", EFieldBase::BASE_FLOAT32, 4),
        field(11, "Double", EFieldBase::BASE_FLOAT64, 8)});
    const auto bytes = encode(definitions, {
        static_cast<char>(0x80), std::uint8_t{255}, std::int8_t{-128}, std::int16_t{-32768},
        std::uint16_t{65535}, std::int32_t{-2147483647 - 1}, std::uint32_t{4294967295u},
        std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::uint64_t>::max(), 1.25f, 2.5});
    const auto inspector = angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, definitions));
    EXPECT_EQ(angaria::test_support::require_value(inspector.inspect(bytes)), heading() +
        "  [0] \"Character\" char = \"\\x80\"\n"
        "  [1] \"Byte\" byte = 255\n"
        "  [2] \"Small\" int8 = -128\n"
        "  [3] \"Signed16\" int16 = -32768\n"
        "  [4] \"Unsigned16\" uint16 = 65535\n"
        "  [5] \"Signed32\" int32 = -2147483648\n"
        "  [6] \"Unsigned32\" uint32 = 4294967295\n"
        "  [7] \"Signed64\" int64 = -9223372036854775808\n"
        "  [8] \"Unsigned64\" uint64 = 18446744073709551615\n"
        "  [9] \"Float\" float32 = 1.25\n"
        "  [10] \"Double\" float64 = 2.5\n");
  }

  TEST(MessageInspection, RendersLogicalMeaningAndExplicitAsciiBytes)
  {
    auto side = field(3, "Side", EFieldBase::BASE_BYTE, 1, FieldLogicalKind::Enumeration);
    side._logical._enumerators = {{"Sell", std::uint64_t{2}}, {"Buy", std::uint64_t{1}}};
    const auto definitions = catalog({field(1, "Enabled", EFieldBase::BASE_BYTE, 1, FieldLogicalKind::Boolean),
        field(2, "Flags", EFieldBase::BASE_UINT16, 2, FieldLogicalKind::Bits), side,
        field(4, "Code", EFieldBase::BASE_CHARS, 6, FieldLogicalKind::FixedAscii)});
    const auto bytes = encode(definitions, {true, BitFieldValue{0x8001}, EnumValue{std::uint8_t{1}},
                                            FixedAsciiValue{std::string{"IBM\0 \n", 6}}});
    const auto inspector = angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, definitions));
    EXPECT_EQ(angaria::test_support::require_value(inspector.inspect(bytes)), heading() +
        "  [0] \"Enabled\" bool = true\n"
        "  [1] \"Flags\" bits16 = 0x8001\n"
        "  [2] \"Side\" enum<byte> = \"Buy\" (1)\n"
        "  [3] \"Code\" chars<6> = \"IBM\\x00 \\x0a\"\n");
  }

  // Catalog names are valid UTF-8 but may contain controls. Every metadata byte
  // is escaped under the same rules as values, so names cannot inject lines.
  TEST(MessageInspection, EscapesMetadataLabelsQuotesBackslashesAndNonAscii)
  {
    auto side = field(1, "Side\t\"\\\xc3\xa9", EFieldBase::BASE_INT8, 1, FieldLogicalKind::Enumeration);
    side._logical._enumerators = {{"Buy\n\x7f\xc3\xa9", std::int64_t{-1}}};
    const auto definitions = catalog({side}, "Q\n\xc3\xa9");
    const auto bytes = encode(definitions, {EnumValue{std::int8_t{-1}}});
    const auto inspector = angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, definitions));
    const auto text = angaria::test_support::require_value(inspector.inspect(bytes));
    EXPECT_TRUE(text.starts_with("message \"Q\\x0a\\xc3\\xa9\" session="));
    EXPECT_NE(text.find("\"Side\\x09\\\"\\\\\\xc3\\xa9\" enum<int8> = \"Buy\\x0a\\x7f\\xc3\\xa9\" (-1)\n"), std::string::npos);
    EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), 2);
    EXPECT_TRUE(std::all_of(text.begin(), text.end(), [](unsigned char byte)
    {
      return byte == '\n' || (byte >= 0x20 && byte <= 0x7e);
    }));
  }

  class CommaPunctuation : public std::numpunct<char>
  {
  protected:
    char do_decimal_point() const override
    {
      return ',';
    }
  };

  class RestoreLocale
  {
  public:
    RestoreLocale() : _previous(std::locale())
    {
      std::locale::global(std::locale(_previous, new CommaPunctuation));
    }
    ~RestoreLocale()
    {
      std::locale::global(_previous);
    }
  private:
    std::locale _previous;
  };

  TEST(MessageInspection, FloatRenderingIsLocaleIndependentAndPreservesSpecialBits)
  {
    const auto definitions = catalog({field(1, "Value", EFieldBase::BASE_FLOAT32, 4)});
    const auto inspector = angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, definitions));
    RestoreLocale restore;
    for (const auto& [bits, expected] : std::array<std::pair<std::uint32_t, const char*>, 6>{{
        {0x80000000u, "-0"}, {0x7f800000u, "inf"}, {0xff800000u, "-inf"},
        {0x7fc12345u, "nan(0x7fc12345)"}, {0x7f800001u, "nan(0x7f800001)"}, {0x3fa00000u, "1.25"}}})
    {
      const auto bytes = encode(definitions, {std::bit_cast<float>(bits)});
      EXPECT_EQ(angaria::test_support::require_value(inspector.inspect(bytes)), heading() + "  [0] \"Value\" float32 = " + expected + "\n");
    }
    const auto doubleDefinitions = catalog({field(1, "Value", EFieldBase::BASE_FLOAT64, 8)});
    const auto doubleInspector = angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, doubleDefinitions));
    const auto bytes = encode(doubleDefinitions, {std::bit_cast<double>(std::uint64_t{0xfff0000000000001})});
    EXPECT_EQ(angaria::test_support::require_value(doubleInspector.inspect(bytes)), heading() + "  [0] \"Value\" float64 = nan(0xfff0000000000001)\n");
  }

  TEST(MessageInspection, ExactSchemaSelectionNeverFallsBackToNewestOrAnotherSession)
  {
    auto definitions = catalog({field(1, "Signed", EFieldBase::BASE_INT16, 2),
                                field(2, "Unsigned", EFieldBase::BASE_UINT16, 2)});
    definitions._messages[0]._descriptor._fields.resize(1);
    auto next = definitions._messages[0];
    next._descriptor._schema._version = 2;
    next._descriptor._fields = {definitions._fields[1]._descriptor._id};
    definitions._messages.push_back(next);
    const auto inspector = angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, definitions));
    const auto first = encode(definitions, {std::int16_t{-2}});
    const auto second = encode(definitions, {std::uint16_t{65534}}, next._descriptor._schema);
    EXPECT_NE(angaria::test_support::require_value(inspector.inspect(first)).find("int16 = -2"), std::string::npos);
    EXPECT_NE(angaria::test_support::require_value(inspector.inspect(second)).find("uint16 = 65534"), std::string::npos);
    auto oldDefinitions = definitions;
    oldDefinitions._messages.pop_back();
    const auto oldInspector = angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, oldDefinitions));
    expect_error(oldInspector.inspect(second), ErrorCode::NotFound);
    auto wrong = angaria::test_support::require_value(MessageRecordCodec::decode(first, 1048576));
    wrong._header._publication._producer._session = {3, 4};
    expect_error(inspector.inspect(wrong.view()), ErrorCode::SchemaConflict);
    wrong._header._publication._producer._session = source_session;
    wrong._header._schema._type = MessageType{99};
    expect_error(inspector.inspect(wrong.view()), ErrorCode::NotFound);
  }

  TEST(MessageInspection, RejectsOpaqueInterpretationMalformedFramesAndInvalidPayloadValues)
  {
    const auto definitions = catalog({field(1, "Enabled", EFieldBase::BASE_BYTE, 1, FieldLogicalKind::Boolean)});
    const auto inspector = angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, definitions));
    const auto bytes = encode(definitions, {true});
    auto record = angaria::test_support::require_value(MessageRecordCodec::decode(bytes, 1048576));
    record._header._format = PayloadFormat::Json;
    expect_error(inspector.inspect(record.view()), ErrorCode::UnsupportedPolicy);
    record._header._format = PayloadFormat::Binary;
    record._payload[0] = std::byte{2};
    expect_error(inspector.inspect(record.view()), ErrorCode::InvalidArgument);
    record._header._payloadBytes = 2;
    expect_error(inspector.inspect(record.view()), ErrorCode::InvalidArgument);
    auto truncated = bytes;
    truncated.pop_back();
    expect_error(inspector.inspect(truncated), ErrorCode::InvalidArgument);
    auto trailing = bytes;
    trailing.push_back(std::byte{});
    expect_error(inspector.inspect(trailing), ErrorCode::InvalidArgument);
  }

  TEST(MessageInspection, TextBudgetIsExactAndBoundsEscapedLabelExpansion)
  {
    auto enumerator = field(1, "Value", EFieldBase::BASE_BYTE, 1, FieldLogicalKind::Enumeration);
    enumerator._logical._enumerators = {{std::string(128, '\n'), std::uint64_t{1}}};
    const auto definitions = catalog({enumerator});
    const auto bytes = encode(definitions, {EnumValue{std::uint8_t{1}}});
    const auto full = angaria::test_support::require_value(angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, definitions)).inspect(bytes));
    InspectionLimits exact;
    exact._maxTextBytes = full.size();
    EXPECT_EQ(angaria::test_support::require_value(angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, definitions, exact)).inspect(bytes)), full);
    --exact._maxTextBytes;
    expect_error(angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, definitions, exact)).inspect(bytes), ErrorCode::CapacityExceeded);
    exact._maxTextBytes = 200;
    expect_error(angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, definitions, exact)).inspect(bytes), ErrorCode::CapacityExceeded);
  }

  TEST(MessageInspection, CatalogBudgetChargesOnlyOwnedDefinitionsBeforeCopies)
  {
    auto definitions = catalog({field(1, "V", EFieldBase::BASE_BYTE, 1)});
    TaskSnapshot irrelevant;
    irrelevant._descriptor._doc = std::string(65536, 'x');
    definitions._tasks.push_back(irrelevant);
    // Field:88+1, schema:88+5+4. Topology and Task metadata are not retained.
    InspectionLimits limits;
    limits._maxCatalogBytes = 186;
    const auto bytes = encode(definitions, {std::uint8_t{7}});
    EXPECT_EQ(angaria::test_support::require_value(angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, definitions, limits)).inspect(bytes)),
              heading() + "  [0] \"V\" byte = 7\n");
    limits._maxCatalogBytes = 185;
    expect_error(MessageInspector::from_snapshot(source_session, definitions, limits), ErrorCode::CapacityExceeded);
    auto large = definitions;
    large._fields[0]._descriptor._name = std::string(65536, 'x');
    expect_error(MessageInspector::from_snapshot(source_session, large, limits), ErrorCode::CapacityExceeded);
  }

  TEST(MessageInspection, ExpandedLayoutBudgetPrecedesRepeatedEnumDescriptorCopies)
  {
    auto enumerator = field(1, "V", EFieldBase::BASE_BYTE, 1, FieldLogicalKind::Enumeration);
    enumerator._logical._enumerators = {{std::string(1024, 'x'), std::uint64_t{1}}};
    auto definitions = catalog({enumerator});
    definitions._messages[0]._descriptor._fields.push_back(enumerator._id);
    InspectionLimits limits;
    limits._maxCatalogBytes = 1254; // One retained Field plus the two-ID schema.
    const auto inspector = angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, definitions, limits));
    const std::array payload{std::byte{1}, std::byte{1}};
    const auto bytes = angaria::test_support::require_value(MessageRecordCodec::encode(header(payload.size()), payload, 1024));
    const auto result = inspector.inspect(bytes);
    expect_error(result, ErrorCode::CapacityExceeded);
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    EXPECT_EQ(std::get<Error>(result)._path, "inspection.layout");
  }

  TEST(MessageInspection, OrderedFieldOccurrencesHaveNoHiddenSixteenBitCountLimit)
  {
    auto definitions = catalog({field(1, "V", EFieldBase::BASE_BYTE, 1)});
    definitions._messages[0]._descriptor._fields.assign(65536, definitions._fields[0]._descriptor._id);
    const auto inspector = angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, definitions));
    const std::vector payload(65536, std::byte{7});
    const auto bytes = angaria::test_support::require_value(MessageRecordCodec::encode(header(payload.size()), payload, 1048576));
    const auto text = angaria::test_support::require_value(inspector.inspect(bytes));
    EXPECT_NE(text.find("  [65535] \"V\" byte = 7\n"), std::string::npos);
  }

  TEST(MessageInspection, LimitsAndEmptyCatalogsHaveExplicitFailures)
  {
    const auto definitions = catalog({field(1, "V", EFieldBase::BASE_BYTE, 1)});
    expect_error(MessageInspector::from_snapshot({}, definitions), ErrorCode::InvalidArgument);
    InspectionLimits limits;
    limits._maxRecordBytes = 79;
    expect_error(MessageInspector::from_snapshot(source_session, definitions, limits), ErrorCode::InvalidArgument);
    limits = {};
    limits._maxTextBytes = 0;
    expect_error(MessageInspector::from_snapshot(source_session, definitions, limits), ErrorCode::InvalidArgument);
    limits = {};
    limits._maxRecordBytes = 80;
    const auto bytes = encode(definitions, {std::uint8_t{7}});
    expect_error(angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, definitions, limits)).inspect(bytes), ErrorCode::CapacityExceeded);
    limits = {};
    limits._maxCatalogBytes = 0;
    const auto empty = angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, RegistrySnapshot{}, limits));
    expect_error(empty.inspect(bytes), ErrorCode::NotFound);
  }

  TEST(MessageInspection, AmbiguousAndIncompleteCatalogsAreRejected)
  {
    const auto valid = catalog({field(1, "V", EFieldBase::BASE_BYTE, 1)});
    std::vector<RegistrySnapshot> invalid;
    auto changed = valid;
    changed._fields.push_back(changed._fields[0]);
    invalid.push_back(changed);
    changed = valid;
    auto duplicate = changed._fields[0];
    duplicate._descriptor._id = static_cast<FieldId>(0xee000002);
    changed._fields.push_back(duplicate);
    invalid.push_back(changed);
    changed = valid;
    changed._messages.push_back(changed._messages[0]);
    invalid.push_back(changed);
    changed = valid;
    auto other = changed._messages[0];
    other._descriptor._schema._version = 2;
    other._descriptor._name = "Renamed";
    changed._messages.push_back(other);
    invalid.push_back(changed);
    changed = valid;
    changed._messages[0]._descriptor._fields = {static_cast<FieldId>(0xee000002)};
    invalid.push_back(changed);
    changed = valid;
    changed._fields[0]._descriptor._id = FieldId{};
    invalid.push_back(changed);
    for (const auto& snapshot : invalid)
    {
      expect_error(MessageInspector::from_snapshot(source_session, snapshot), ErrorCode::InvalidArgument);
    }
  }

  TEST(MessageInspection, UnusedRecognizedFutureFieldsDoNotBlockSupportedSchemas)
  {
    auto definitions = catalog({field(1, "V", EFieldBase::BASE_BYTE, 1)});
    definitions._fields.push_back({field(2, "Later", EFieldBase::BASE_TIMESTAMP, 8), {}});
    auto future = definitions._messages[0];
    future._descriptor._schema = {MessageType{2}, 1};
    future._descriptor._name = "Future";
    future._descriptor._fields = {definitions._fields[1]._descriptor._id};
    definitions._messages.push_back(future);
    const auto inspector = angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, definitions));
    EXPECT_NE(angaria::test_support::require_value(inspector.inspect(encode(definitions, {std::uint8_t{7}}))).find("byte = 7"), std::string::npos);
    const std::array<std::byte, 8> payload{};
    const auto record = angaria::test_support::require_value(MessageRecordCodec::encode(header(8, future._descriptor._schema), payload, 88));
    expect_error(inspector.inspect(record), ErrorCode::UnsupportedPolicy);
  }

  TEST(MessageInspection, CopiesAndMovesPreserveTheCatalogContextAndInvalidateSources)
  {
    auto definitions = catalog({field(1, "V", EFieldBase::BASE_BYTE, 1)});
    const auto bytes = encode(definitions, {std::uint8_t{7}});
    auto original = angaria::test_support::require_value(MessageInspector::from_snapshot(source_session, definitions));
    auto copy = original;
    definitions._fields[0]._descriptor._name = "Changed";
    EXPECT_EQ(angaria::test_support::require_value(copy.inspect(bytes)), angaria::test_support::require_value(original.inspect(bytes)));
    auto replacement = angaria::test_support::require_value(MessageInspector::from_snapshot({3, 4}, definitions));
    replacement = original;
    EXPECT_EQ(replacement.session(), source_session);
    EXPECT_EQ(angaria::test_support::require_value(replacement.inspect(bytes)), angaria::test_support::require_value(original.inspect(bytes)));
    const auto* self = &replacement;
    replacement = *self;
    auto moved = std::move(replacement);
    expect_error(replacement.inspect(bytes), ErrorCode::InvalidState);
    EXPECT_EQ(replacement.session(), SessionId{});
    copy = std::move(moved);
    expect_error(moved.inspect(bytes), ErrorCode::InvalidState);
    EXPECT_EQ(angaria::test_support::require_value(copy.inspect(bytes)), angaria::test_support::require_value(original.inspect(bytes)));
  }

  class InspectionRegistryTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (std::filesystem::temp_directory_path() / "angaria-inspection-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
    }
    void TearDown() override
    {
      std::error_code ignored;
      std::filesystem::remove_all(_root, ignored);
    }
    std::filesystem::path _root;
  };

  // Private compatibility setup follows the predecessor scalar layout: close
  // all owners before recording its 2.0 profile. This is a test fixture, never
  // an application downgrade/migration API. The public inspector then reads
  // that native catalog and retains its meaning after every native owner exits.
  TEST_F(InspectionRegistryTest, CurrentCatalogInspectsRecordsAcrossProducerReplacement)
  {
    namespace mapped = angaria::detail::mapped;
    const auto directory = _root / "current";
    SessionId identity;
    std::vector<std::byte> first;
    std::vector<std::byte> replacement;
    std::optional<MessageInspector> retained;
    {
      SessionOptions options;
      options._name = "Current scalar inspection";
      options._directory = directory;
      options._maxLogBytes = 1048576;
      options._limits = CapacityLimits::empty().tasks(1).fields(2).message_schemas(1);
      TaskDescriptor setup;
      setup._name = "Setup";
      auto created = angaria::test_support::require_value(Session::create(options, setup));
      identity = created._session.identity();
      auto registry = created._creator.registry();
      const auto price = angaria::test_support::require_value(registry.register_field(field(1, "Price", EFieldBase::BASE_INT64, 8)));
      const auto quantity = angaria::test_support::require_value(registry.register_field(field(2, "Quantity", EFieldBase::BASE_UINT64, 8)));
      MessageDescriptor quote;
      quote._schema = quote_schema;
      quote._name = "Quote";
      quote._fields = {price, quantity};
      angaria::test_support::require_value(registry.register_message(quote));
      const auto codec = angaria::test_support::require_value(BinaryPayloadCodec::from_registry(created._session.registry(), quote_schema, 16));
      const auto payload = angaria::test_support::require_value(codec.encode({std::int64_t{-2}, std::uint64_t{7}}));
      auto metadata = header(payload.size(), quote_schema, identity);
      metadata._publication = angaria::test_support::require_value(created._creator.next_publication_id());
      first = angaria::test_support::require_value(MessageRecordCodec::encode(metadata, payload, 96));
    }
    {
      auto session = angaria::test_support::require_value(Session::attach(directory));
      EXPECT_EQ(session.identity(), identity);
      auto producer = angaria::test_support::require_value(session.attach_task(TaskId{1}));
      EXPECT_EQ(producer.claim()._incarnation, 2u);
      const auto codec = angaria::test_support::require_value(BinaryPayloadCodec::from_registry(session.registry(), quote_schema, 16));
      const auto payload = angaria::test_support::require_value(codec.encode({std::int64_t{-3}, std::uint64_t{8}}));
      auto metadata = header(payload.size(), quote_schema, identity);
      metadata._publication = angaria::test_support::require_value(producer.next_publication_id());
      replacement = angaria::test_support::require_value(MessageRecordCodec::encode(metadata, payload, 96));
      retained.emplace(angaria::test_support::require_value(MessageInspector::from_session(session)));
    }
    EXPECT_NE(angaria::test_support::require_value(retained->inspect(first)).find("  [0] \"Price\" int64 = -2\n  [1] \"Quantity\" uint64 = 7\n"),
              std::string::npos);
    const auto text = angaria::test_support::require_value(retained->inspect(replacement));
    EXPECT_NE(text.find("incarnation=2 sequence=1"), std::string::npos);
    EXPECT_NE(text.find("  [0] \"Price\" int64 = -3\n  [1] \"Quantity\" uint64 = 8\n"), std::string::npos);

    // Inspectors keep exact definitions without retaining a Task lease. A
    // replacement uses the same current-format Session and a new incarnation.
    auto session = angaria::test_support::require_value(Session::attach(directory));
    auto producer = angaria::test_support::require_value(session.attach_task(TaskId{1}));
    EXPECT_EQ(producer.claim()._incarnation, 3u);
    auto file = angaria::test_support::require_value(mapped::NativeFile::open_existing(directory / "session.ang", mapped::FileAccess::ReadOnly));
    mapped::SuperblockBytes bytes{};
    angaria::test_support::require_value(file.read_exact(0, bytes));
    const auto profile = angaria::test_support::require_value(mapped::decode_superblock(bytes));
    EXPECT_EQ(profile._formatMinor, 0u);
    EXPECT_EQ(profile._requiredFeatures, mapped::journal_feature | mapped::basic_fields_feature | mapped::native_delivery_feature);
  }

  // Source applications have exited before inspection. Retained schema versions
  // and copied inspector metadata outlive every Session/Task/Registry wrapper;
  // acquiring a replacement claim also proves inspectors retained no Task lease.
  TEST_F(InspectionRegistryTest, MultipleTypesAndHistoricalVersionsRemainInspectableAfterExit)
  {
    std::optional<MessageInspector> old;
    std::optional<MessageInspector> latest;
    std::vector<std::vector<std::byte>> records;
    const auto directory = _root / "session";
    {
      SessionOptions options;
      options._name = "Inspection examples";
      options._directory = directory;
      options._maxLogBytes = 1048576;
      options._limits = CapacityLimits::empty().tasks(2).fields(8).message_schemas(8);
      TaskDescriptor setup;
      setup._name = "Setup";
      auto created = angaria::test_support::require_value(Session::create(options, setup));
      auto registry = created._creator.registry();
      const auto signedId = angaria::test_support::require_value(registry.register_field(field(1, "Signed", EFieldBase::BASE_INT16, 2)));
      MessageDescriptor quote;
      quote._schema = quote_schema;
      quote._name = "Quote";
      quote._fields = {signedId};
      angaria::test_support::require_value(registry.register_message(quote));
      const auto capture = [&](SchemaId schema, std::initializer_list<PayloadValue> values)
      {
        const auto codec = angaria::test_support::require_value(BinaryPayloadCodec::from_registry(created._session.registry(), schema, 64));
        const auto payload = angaria::test_support::require_value(codec.encode(values));
        auto metadata = header(payload.size(), schema, created._session.identity());
        // Private portable-capture fixture: these identities describe supplied
        // bytes, not live Queue acceptance or public ID issuance during setup.
        metadata._publication = {created._creator.claim(), records.size() + 1};
        records.push_back(angaria::test_support::require_value(MessageRecordCodec::encode(metadata, payload, 144)));
      };
      capture(quote._schema, {std::int16_t{-2}});
      old.emplace(angaria::test_support::require_value(MessageInspector::from_session(created._session)));
      quote._schema._version = 2;
      quote._fields = {angaria::test_support::require_value(registry.register_field(field(2, "Unsigned", EFieldBase::BASE_UINT16, 2)))};
      angaria::test_support::require_value(registry.register_message(quote));
      capture(quote._schema, {std::uint16_t{65534}});
      auto enabled = field(3, "Enabled", EFieldBase::BASE_BYTE, 1, FieldLogicalKind::Boolean);
      MessageDescriptor status;
      status._name = "Status";
      status._schema = {MessageType{2}, 1};
      status._fields = {angaria::test_support::require_value(registry.register_field(enabled))};
      angaria::test_support::require_value(registry.register_message(status));
      capture(status._schema, {true});
      latest.emplace(angaria::test_support::require_value(MessageInspector::from_session(created._session)));
    }
    EXPECT_NE(angaria::test_support::require_value(old->inspect(records[0])).find("int16 = -2"), std::string::npos);
    expect_error(old->inspect(records[1]), ErrorCode::NotFound);
    EXPECT_NE(angaria::test_support::require_value(latest->inspect(records[1])).find("uint16 = 65534"), std::string::npos);
    EXPECT_NE(angaria::test_support::require_value(latest->inspect(records[2])).find("\"Status\""), std::string::npos);
    auto attached = angaria::test_support::require_value(Session::attach(directory));
    auto replacement = angaria::test_support::require_value(attached.attach_task(TaskId{1}));
    EXPECT_GT(replacement.claim()._incarnation, 1u);
    EXPECT_NE(angaria::test_support::require_value(latest->inspect(records[0])).find("incarnation=1"), std::string::npos);
  }
} // namespace
