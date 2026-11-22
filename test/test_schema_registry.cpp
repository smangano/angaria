// Public format-2 Field and message-schema examples. Register metadata through a
// live Task's Registry, then use a Session view to inspect exact identities.
// Raw file edits below are deliberate corruption fixtures, not application APIs.

#include "angaria/runtime.h"
#include "native_atomic.h"
#include "runtime_state.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;
  namespace fs = std::filesystem;

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

  FieldDescriptor field(std::uint32_t id = 0xee000001, std::string name = "Price",
                        EFieldBase base = EFieldBase::BASE_INT64, std::uint32_t size = 8,
                        std::string doc = "Price in ticks")
  {
    FieldDescriptor result;
    result._id = static_cast<FieldId>(id);
    result._name = std::move(name);
    result._base = base;
    result._size = size;
    result._doc = std::move(doc);
    return result;
  }

  MessageDescriptor message(std::uint16_t type = 1, std::uint32_t version = 1,
                            std::string name = "Quote", std::vector<FieldId> fields = {},
                            std::string doc = "Market data")
  {
    MessageDescriptor result;
    result._schema = SchemaId{static_cast<MessageType>(type), version};
    result._name = std::move(name);
    result._fields = std::move(fields);
    result._doc = std::move(doc);
    return result;
  }

  struct SchemaObservation
  {
    mapped::JournalSnapshot _snapshot;
    std::vector<std::uint64_t> _image;
    std::vector<std::string> _claims;

    bool operator==(const SchemaObservation&) const = default;
  };

  class SchemaRegistryTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-schema-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
      _directory = _root / "session";
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    CreatedSession create(std::uint32_t fields = 16, std::uint32_t schemas = 8,
                          std::uint64_t limit = 2097152) const
    {
      SessionOptions options;
      options._name = "Schema examples";
      options._directory = _directory;
      options._maxLogBytes = limit;
      options._limits = CapacityLimits::empty().tasks(4).fields(fields).message_schemas(schemas);
      TaskDescriptor creator;
      creator._name = "Creator";
      return require_value(Session::create(options, creator));
    }

    std::shared_ptr<runtime::SessionState> state() const
    {
      auto value = std::make_shared<runtime::SessionState>();
      value->_directory = fs::canonical(_directory);
      value->_controller = require_value(mapped::SessionController::open_existing(_directory));
      return value;
    }

    // The gate gives this byte comparison the same observation boundary as a
    // public lookup. Coordination files are included to catch accidental Task
    // resource allocation by Field/schema-only mutations.
    SchemaObservation observe() const
    {
      runtime::RuntimeAccess access(state());
      SchemaObservation result;
      result._snapshot = access._snapshot;
      result._image = access.read_words(0, static_cast<std::size_t>(access._snapshot._fileBytes / 8));
      for (const auto& entry : fs::directory_iterator(_directory / "claims"))
      {
        result._claims.push_back(entry.path().filename().string());
      }
      std::sort(result._claims.begin(), result._claims.end());
      return result;
    }

    void set_word(std::uint64_t offset, std::uint64_t value) const
    {
      auto file = require_value(mapped::NativeFile::open_existing(_directory / "session.ang", mapped::FileAccess::ReadWrite));
      auto region = require_value(file.map_read_write(offset, 8));
      require_value(mapped::AtomicWord::bind(require_value(region.writable_bytes()), 0)).store_release(value);
    }

    std::uint64_t reference(mapped::SectionKind kind, std::uint64_t key) const
    {
      runtime::RuntimeAccess access(state());
      const auto layout = *mapped::make_fixed_layout(access._session->_controller->configuration()._limits);
      for (const auto& section : layout._sections)
      {
        if (section._kind != kind)
        {
          continue;
        }
        const auto rows = access.read_words(section._offset, static_cast<std::size_t>(section._byteLength / 8));
        for (std::size_t row = 0; row < rows.size(); row += 8)
        {
          if (rows[row] == key)
          {
            return rows[row + 1];
          }
        }
      }
      throw std::runtime_error("Fixture could not find its registered definition");
    }

    static std::uint64_t key(SchemaId id)
    {
      return (static_cast<std::uint64_t>(id._type) << 32) | id._version;
    }

    fs::path _root;
    fs::path _directory;
  };

  // Start with the declared scalar widths, rather than native sizeof values.
  // IDs are application-selected full 32-bit identities: both ends of the EE
  // namespace are valid even though only a few table slots are reserved.
  TEST_F(SchemaRegistryTest, RegistersScalarFieldsWithFullWidthApplicationIdentities)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const std::array<std::pair<EFieldBase, std::uint32_t>, 10> types{{
        {EFieldBase::BASE_CHAR, 1}, {EFieldBase::BASE_BYTE, 1},
        {EFieldBase::BASE_INT16, 2}, {EFieldBase::BASE_UINT16, 2},
        {EFieldBase::BASE_INT32, 4}, {EFieldBase::BASE_UINT32, 4}, {EFieldBase::BASE_FLOAT32, 4},
        {EFieldBase::BASE_INT64, 8}, {EFieldBase::BASE_UINT64, 8}, {EFieldBase::BASE_FLOAT64, 8}}};
    for (std::size_t index = 0; index < types.size(); ++index)
    {
      const auto descriptor = field(0xee000000u + static_cast<std::uint32_t>(index),
                                     "Scalar" + std::to_string(index), types[index].first, types[index].second);
      const auto id = require_value(actor.register_field(descriptor));
      EXPECT_EQ(id, descriptor._id);
      EXPECT_EQ(require_value(created._session.registry().find_field(descriptor._name)), id);
      const auto read = require_value(actor.describe_field(id));
      EXPECT_EQ(read._id, descriptor._id);
      EXPECT_EQ(read._base, descriptor._base);
      EXPECT_EQ(read._size, descriptor._size);
      EXPECT_EQ(read._doc, descriptor._doc);
      const auto registration = require_value(actor.metadata(id));
      EXPECT_EQ(registration._createdBy, TaskId{1});
      EXPECT_EQ(registration._revision, index + 2);
    }
    EXPECT_EQ(require_value(actor.register_field(field(0xeeffffff, "Upper"))), static_cast<FieldId>(0xeeffffff));
    expect_error(actor.find_field("Absent"), ErrorCode::NotFound);
    expect_error(actor.describe_field(static_cast<FieldId>(0xee123456)), ErrorCode::NotFound);
    EXPECT_EQ(observe()._claims.size(), 1u);
  }

  // A type retains its name while each version retains its own ordered Field
  // list and metadata. Versions are caller-provided identities, so registration
  // order need not match numeric order, and the maximum type/version remains exact.
  TEST_F(SchemaRegistryTest, RetainsExactVersionsOrderedRepetitionsAndEmptyControlSchemas)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto price = require_value(actor.register_field(field()));
    const auto quantity = require_value(actor.register_field(field(0xeeffffff, "Quantity", EFieldBase::BASE_UINT64)));
    const auto first = message(65535, std::numeric_limits<std::uint32_t>::max(), "Quote", {price, quantity, price});
    const auto older_number = message(65535, 1, "Quote", {quantity, price}, "Earlier numeric version");
    EXPECT_EQ(require_value(actor.register_message(first)), first._schema);
    const auto initial_metadata = require_value(actor.metadata(first._schema));
    EXPECT_EQ(require_value(actor.register_message(older_number)), older_number._schema);
    const auto empty = message(2, 3, "Heartbeat", {}, "No payload fields");
    EXPECT_EQ(require_value(actor.register_message(empty)), empty._schema);
    EXPECT_EQ(require_value(created._session.registry().find_message("Quote", first._schema._version)), first._schema);
    EXPECT_EQ(require_value(actor.describe_message(first._schema))._fields, first._fields);
    EXPECT_EQ(require_value(actor.describe_message(older_number._schema))._fields, older_number._fields);
    EXPECT_TRUE(require_value(actor.describe_message(empty._schema))._fields.empty());
    EXPECT_EQ(require_value(actor.metadata(first._schema))._createdAt, initial_metadata._createdAt);
    EXPECT_EQ(require_value(actor.metadata(first._schema))._revision, initial_metadata._revision);
    expect_error(actor.find_message("Quote", 2), ErrorCode::NotFound);
  }

  // Field-array length is encoded independently of legacy 16-bit counters.
  // Repetition is meaningful schema data, not duplicate registration, and a
  // schema with 65536 references still consumes just one schema-capacity slot.
  TEST_F(SchemaRegistryTest, LargeFieldListsPreserveEveryFullWidthReference)
  {
    auto created = create(2, 1);
    auto actor = created._creator.registry();
    const auto low = require_value(actor.register_field(field(0xee000000, "Low")));
    const auto high = require_value(actor.register_field(field(0xeeffffff, "High")));
    auto descriptor = message();
    descriptor._fields.resize(65536, high);
    descriptor._fields.front() = low;
    descriptor._fields.back() = low;
    const auto id = require_value(actor.register_message(descriptor));
    const auto read = require_value(actor.describe_message(id));
    ASSERT_EQ(read._fields.size(), 65536u);
    EXPECT_EQ(read._fields, descriptor._fields);
    EXPECT_EQ(require_value(actor.register_message(descriptor)), id);
  }

  // Exact retries are lookups after recovery. They neither attribute a new
  // registration to the current caller nor touch the reusable journal, retained
  // definitions, or coordination namespace.
  TEST_F(SchemaRegistryTest, ExactFieldAndSchemaRetriesChangeNoStoredState)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto field_descriptor = field();
    const auto id = require_value(actor.register_field(field_descriptor));
    const auto schema = message(1, 1, "Quote", {id});
    require_value(actor.register_message(schema));
    const auto field_metadata = require_value(actor.metadata(id));
    const auto schema_metadata = require_value(actor.metadata(schema._schema));
    const auto before = observe();
    EXPECT_EQ(require_value(actor.register_field(field_descriptor)), id);
    EXPECT_EQ(require_value(actor.register_message(schema)), schema._schema);
    EXPECT_EQ(observe(), before);
    EXPECT_EQ(require_value(actor.metadata(id))._createdAt, field_metadata._createdAt);
    EXPECT_EQ(require_value(actor.metadata(schema._schema))._createdAt, schema_metadata._createdAt);
  }

  // A Field's ID and name each select one immutable meaning. Reusing either
  // with changed metadata is a conflict; a new meaning needs a different ID and
  // name rather than silently changing messages that already reference it.
  TEST_F(SchemaRegistryTest, ConflictingFieldIdsNamesAndDescriptorsPreserveHistory)
  {
    auto created = create();
    auto actor = created._creator.registry();
    auto original = field();
    require_value(actor.register_field(original));
    auto changed_doc = original;
    changed_doc._doc += " changed";
    auto changed_type = original;
    changed_type._base = EFieldBase::BASE_UINT64;
    auto changed_name = original;
    changed_name._name = "Renamed";
    auto changed_id = original;
    changed_id._id = static_cast<FieldId>(0xee000002);
    const auto before = observe();
    for (const auto& descriptor : {changed_doc, changed_type, changed_name, changed_id})
    {
      expect_error(actor.register_field(descriptor), ErrorCode::Conflict);
      EXPECT_EQ(observe(), before);
    }
  }

  // SchemaConflict distinguishes an attempt to redefine an immutable schema or
  // remap the shared type/name relationship. Field order and documentation are
  // included in exact descriptor equality, not just the set of referenced IDs.
  TEST_F(SchemaRegistryTest, SchemaIdentityAndTypeNameConflictsAreExplicit)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto first_field = require_value(actor.register_field(field()));
    const auto second_field = require_value(actor.register_field(field(0xee000002, "Quantity")));
    const auto original = message(1, 1, "Quote", {first_field, second_field});
    require_value(actor.register_message(original));
    auto reordered = original;
    std::reverse(reordered._fields.begin(), reordered._fields.end());
    auto changed_doc = original;
    changed_doc._doc += " changed";
    const auto changed_name = message(1, 2, "Different", {first_field});
    const auto changed_type = message(2, 2, "Quote", {first_field});
    const auto before = observe();
    for (const auto& descriptor : {reordered, changed_doc, changed_name, changed_type})
    {
      expect_error(actor.register_message(descriptor), ErrorCode::SchemaConflict);
      EXPECT_EQ(observe(), before);
    }
  }

  // Unknown wire identities and malformed text are invalid input. Recognized
  // non-scalar bases remain explicit UnsupportedPolicy until their payload
  // encodings arrive; no rejection reserves an index or changes history.
  TEST_F(SchemaRegistryTest, InvalidOrUnsupportedFieldsRejectBeforeAnyWrites)
  {
    auto created = create();
    auto actor = created._creator.registry();
    std::vector<FieldDescriptor> invalid{
        field(0), field(0xef000001), field(1), field(0xee000001, ""),
        field(0xee000001, std::string{"a\0b", 3}), field(0xee000001, "\xc0\xaf"),
        field(0xee000001, "BadWidth", EFieldBase::BASE_INT64, 4),
        field(0xee000001, "BadBase", static_cast<EFieldBase>(99))};
    auto bad_doc = field();
    bad_doc._doc = "\xff";
    invalid.push_back(bad_doc);
    const auto before = observe();
    for (const auto& descriptor : invalid)
    {
      expect_error(actor.register_field(descriptor), ErrorCode::InvalidArgument);
      EXPECT_EQ(observe(), before);
    }
    for (const auto base : {EFieldBase::BASE_NONE, EFieldBase::BASE_DATE, EFieldBase::BASE_TIMESTAMP,
                            EFieldBase::BASE_CHARS, EFieldBase::BASE_BYTES, EFieldBase::BASE_SYMBOL,
                            EFieldBase::BASE_STRING})
    {
      expect_error(actor.register_field(field(0xee000001, "Later", base, 0)), ErrorCode::UnsupportedPolicy);
      EXPECT_EQ(observe(), before);
    }
  }

  // Resolve every valid Field reference before allocating a new schema. A
  // missing registered Field differs from an ID outside the EE namespace; neither
  // permits a half-published schema or an implicit Field registration.
  TEST_F(SchemaRegistryTest, InvalidSchemasAndMissingFieldReferencesRejectWithoutWrites)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto before = observe();
    for (const auto& descriptor : {message(0), message(1, 0), message(1, 1, ""),
                                  message(1, 1, std::string{"bad\0name", 8}),
                                  message(1, 1, "BadRef", {static_cast<FieldId>(0xff000001)}),
                                  message(1, 1, "BadText", {}, "\xff")})
    {
      expect_error(actor.register_message(descriptor), ErrorCode::InvalidArgument);
      EXPECT_EQ(observe(), before);
    }
    expect_error(actor.register_message(message(1, 1, "Missing", {static_cast<FieldId>(0xee123456)})), ErrorCode::NotFound);
    EXPECT_EQ(observe(), before);
  }

  // Names are UTF-8 bytes and case sensitive, with no legacy short-name limit.
  // Documentation can contain embedded NULs because it is length delimited, and
  // its storage does not consume the separately reserved application string pool.
  TEST_F(SchemaRegistryTest, MetadataStringsRemainOwningAndLengthDelimited)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto lower = require_value(actor.register_field(field(0xee000001, "price")));
    const auto upper = require_value(actor.register_field(field(0xee000002, "Price")));
    EXPECT_NE(lower, upper);
    const auto descriptor = field(0xee000003, std::string(256, 'n') + "\xf0\x9f\x93\x88",
                                   EFieldBase::BASE_INT64, 8, std::string{"notes\0tail", 10});
    const auto id = require_value(actor.register_field(descriptor));
    const auto schema = message(3, 5, std::string(128, 's'), {id}, std::string{"doc\0tail", 8});
    require_value(actor.register_message(schema));
    EXPECT_EQ(require_value(actor.describe_field(id))._doc, descriptor._doc);
    EXPECT_EQ(require_value(actor.describe_message(schema._schema))._doc, schema._doc);
    EXPECT_EQ(require_value(created._session.options())._limits._stringPoolBytes, 0u);
  }

  // Each Field and each schema version consumes one index slot, independent of
  // the numeric magnitude of its identity. Exhaustion still permits exact
  // lookup-style retry, which must not be mistaken for a request for another slot.
  TEST_F(SchemaRegistryTest, CapacitiesCountDefinitionsAndVersionsNotIdentityMagnitudes)
  {
    auto created = create(1, 2);
    auto actor = created._creator.registry();
    const auto descriptor = field(0xeeffffff, "LastId");
    const auto id = require_value(actor.register_field(descriptor));
    const auto first = message(65535, 100, "Quote", {id});
    const auto second = message(65535, 1, "Quote", {id, id});
    require_value(actor.register_message(first));
    require_value(actor.register_message(second));
    const auto before = observe();
    expect_error(actor.register_field(field(0xee000000, "Another")), ErrorCode::CapacityExceeded);
    expect_error(actor.register_message(message(65535, 2, "Quote", {id})), ErrorCode::CapacityExceeded);
    EXPECT_EQ(require_value(actor.register_field(descriptor)), id);
    EXPECT_EQ(require_value(actor.register_message(first)), first._schema);
    EXPECT_EQ(require_value(actor.register_message(second)), second._schema);
    EXPECT_EQ(observe(), before);
  }

  // Measure the complete observed append rather than duplicating allocation
  // arithmetic. One byte below that size must reject without any change; exactly
  // that cap must accept and allow an unchanged retry at LogFull.
  TEST_F(SchemaRegistryTest, CompleteFieldAndSchemaBudgetsIncludeManifestAndPadding)
  {
    for (const auto schema : {false, true})
    {
      SCOPED_TRACE(schema);
      std::uint64_t required{};
      {
        auto created = create();
        auto actor = created._creator.registry();
        const auto id = require_value(actor.register_field(field()));
        if (schema)
        {
          require_value(actor.register_message(message(1, 1, "Quote", {id, id})));
        }
        required = observe()._snapshot._tail;
      }
      fs::remove_all(_directory);
      {
        auto created = create(16, 8, required - 1);
        auto actor = created._creator.registry();
        if (schema)
        {
          require_value(actor.register_field(field()));
        }
        const auto before = observe();
        if (schema)
        {
          expect_error(actor.register_message(message(1, 1, "Quote", {field()._id, field()._id})), ErrorCode::LogFull);
        }
        else
        {
          expect_error(actor.register_field(field()), ErrorCode::LogFull);
        }
        EXPECT_EQ(observe(), before);
      }
      fs::remove_all(_directory);
      {
        auto created = create(16, 8, required);
        auto actor = created._creator.registry();
        const auto id = require_value(actor.register_field(field()));
        const auto descriptor = message(1, 1, "Quote", {id, id});
        if (schema)
        {
          require_value(actor.register_message(descriptor));
        }
        const auto before = observe();
        EXPECT_EQ(before._snapshot._tail, required);
        EXPECT_EQ(before._snapshot._accounted, required);
        EXPECT_EQ(require_value(actor.register_field(field())), id);
        if (schema)
        {
          EXPECT_EQ(require_value(actor.register_message(descriptor)), descriptor._schema);
        }
        EXPECT_EQ(observe(), before);
      }
      fs::remove_all(_directory);
    }
  }

  // A Session view permits inspection but not mutation. A Thread actor may
  // register on its attaching thread only, and retaining that Registry keeps its
  // Process parent claim alive after the original wrappers close.
  TEST_F(SchemaRegistryTest, ActorViewsEnforceReadOnlyAndThreadOwnershipRules)
  {
    auto created = create();
    auto reader = created._session.registry();
    const auto before = observe();
    expect_error(reader.register_field(field()), ErrorCode::ReadOnlyRegistry);
    expect_error(reader.register_message(message()), ErrorCode::ReadOnlyRegistry);
    expect_error(Registry{}.register_field(field()), ErrorCode::InvalidState);
    EXPECT_EQ(observe(), before);
    TaskDescriptor child;
    child._name = "Registrar";
    child._kind = TaskKind::Thread;
    child._process = TaskId{1};
    const auto child_id = require_value(created._creator.registry().register_task(child));
    std::optional<Task> task(require_value(created._session.attach_task(child_id)));
    std::optional<Registry> actor(task->registry());
    std::optional<Result<FieldId>> result;
    const auto before_wrong_thread = observe();
    std::thread other([&]
    {
      result.emplace(actor->register_field(field()));
    });
    other.join();
    ASSERT_TRUE(result);
    expect_error(*result, ErrorCode::WrongOwner);
    EXPECT_EQ(observe(), before_wrong_thread);
    {
      auto closing = std::move(created._creator);
    }
    task.reset();
    const auto id = require_value(actor->register_field(field()));
    const auto schema = require_value(actor->register_message(message(1, 1, "Quote", {id})));
    EXPECT_EQ(require_value(reader.metadata(id))._createdBy, child_id);
    EXPECT_EQ(require_value(reader.metadata(schema))._createdBy, child_id);
    expect_error(created._session.attach_task(TaskId{1}), ErrorCode::OwnershipBusy);
    actor.reset();
    auto parent = require_value(created._session.attach_task(TaskId{1}));
    EXPECT_EQ(parent.claim()._incarnation, 2u);
  }

  // Counter exhaustion cannot turn a fresh identity into a wrapped generation.
  // The direct root edit is a private boundary fixture; an exact descriptor is
  // still a lookup and requires no additional operation generation.
  TEST_F(SchemaRegistryTest, ExhaustedGenerationStillAllowsExactFieldAndSchemaRetry)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto id = require_value(actor.register_field(field()));
    const auto schema = message(1, 1, "Quote", {id});
    require_value(actor.register_message(schema));
    const auto previous = observe()._snapshot._generation;
    set_word(120, std::numeric_limits<std::uint64_t>::max());
    const auto before = observe();
    expect_error(actor.register_field(field(0xee000002, "New")), ErrorCode::CapacityExceeded);
    expect_error(actor.register_message(message(1, 2, "Quote", {id})), ErrorCode::CapacityExceeded);
    EXPECT_EQ(require_value(actor.register_field(field())), id);
    EXPECT_EQ(require_value(actor.register_message(schema)), schema._schema);
    EXPECT_EQ(observe(), before);
    set_word(120, previous);
  }

  // New versions and a new actor incarnation must not rewrite an earlier
  // definition. Captured immutable bytes and creator/time/revision remain exact,
  // and the new version links to the latest registered version of the same type.
  TEST_F(SchemaRegistryTest, LaterVersionsAndActorRestartRetainEarlierDefinitionBytes)
  {
    auto created = create();
    auto session = std::move(created._session);
    const auto id = require_value(created._creator.registry().register_field(field()));
    const auto first = message(1, 3, "Quote", {id});
    require_value(created._creator.registry().register_message(first));
    const auto old_metadata = require_value(session.registry().metadata(first._schema));
    const auto old_reference = reference(mapped::SectionKind::SchemaIndex, key(first._schema));
    std::vector<std::byte> old_body;
    {
      runtime::RuntimeAccess access(state());
      old_body = access.immutable_record(old_reference, 7);
    }
    {
      auto closing = std::move(created._creator);
    }
    auto replacement = require_value(session.attach_task(TaskId{1}));
    const auto next = message(1, 1, "Quote", {id, id}, "Different numeric version");
    require_value(replacement.registry().register_message(next));
    const auto next_reference = reference(mapped::SectionKind::SchemaIndex, key(next._schema));
    runtime::RuntimeAccess access(state());
    EXPECT_EQ(access.immutable_record(old_reference, 7), old_body);
    EXPECT_EQ(access.record_envelope(next_reference, 7)._previous, old_reference);
    EXPECT_GT(access.record_envelope(next_reference, 7)._registryRevision, old_metadata._revision);
  }

  // A future configuration loader can publish several definitions in one
  // atomic batch. Two versions can share a registration revision, and their
  // referenced Field can appear later in that same batch. Physical record order,
  // not index-slot order or numeric version, selects the next chain predecessor.
  TEST_F(SchemaRegistryTest, BatchedVersionsUseRecordOrderAndResolveSameCommitFields)
  {
    for (const auto reversed : {false, true})
    {
      SCOPED_TRACE(reversed);
      {
        auto created = create();
        const auto claim = created._creator.claim();
        const auto registered_at = require_value(created._session.metadata())._createdAt;
        const auto field_id = static_cast<FieldId>(0xee000010);
        std::uint64_t first_reference{};
        std::uint64_t second_reference{};
        {
          runtime::RuntimeAccess access(state());
          mapped::TransactionPlan plan;
          plan._base = access._snapshot;
          plan._kind = mapped::JournalKind::Append;
          plan._actor = claim._task;
          plan._actorIncarnation = claim._incarnation;
          plan._newRevision = access._snapshot._revision + 1;
          const RegistrationMetadata metadata{claim._task, registered_at, plan._newRevision};
          auto offset = access._snapshot._tail;
          const auto append = [&](mapped::JournalRecord record)
          {
            const auto reference = offset;
            offset += *mapped::checked_align(64 + *mapped::checked_align(record._immutable.size(), 8), 64);
            plan._records.push_back(std::move(record));
            return reference;
          };
          const auto first = message(33, 5, "BatchMessage", {field_id}, "First in the batch");
          mapped::JournalRecord first_record;
          first_record._kind = 7;
          first_record._immutable = require_value(mapped::encode_schema_definition(
              mapped::SchemaDefinition{first, metadata}, offset));
          first_reference = append(std::move(first_record));
          const auto second = message(33, 2, "BatchMessage", {field_id, field_id}, "Latest in the batch");
          mapped::JournalRecord second_record;
          second_record._kind = 7;
          second_record._previous = first_reference;
          second_record._immutable = require_value(mapped::encode_schema_definition(
              mapped::SchemaDefinition{second, metadata}, offset));
          second_reference = append(std::move(second_record));
          mapped::JournalRecord field_record;
          field_record._kind = 6;
          field_record._immutable = require_value(mapped::encode_field_definition(
              mapped::FieldDefinition{field(0xee000010, "BatchField"), metadata}, offset));
          const auto field_reference = append(std::move(field_record));
          const auto schema_index = access.section_offset(mapped::SectionKind::SchemaIndex);
          const std::array<std::uint64_t, 4> first_row{key(first._schema), first_reference, plan._newRevision, 0};
          const std::array<std::uint64_t, 4> second_row{key(second._schema), second_reference, plan._newRevision, 0};
          const std::array<std::uint64_t, 4> field_row{static_cast<std::uint32_t>(field_id), field_reference,
                                                     plan._newRevision, 0};
          plan._commands = {
              runtime::runtime_write_words(schema_index + (reversed ? 64 : 0), first_row),
              runtime::runtime_write_words(schema_index + (reversed ? 0 : 64), second_row),
              runtime::runtime_write_words(access.section_offset(mapped::SectionKind::FieldIndex), field_row)};
          require_value(access._journal.execute(access._guard, plan));
        }
        auto registry = created._creator.registry();
        EXPECT_EQ(require_value(registry.find_field("BatchField")), field_id);
        EXPECT_EQ(require_value(registry.describe_message(SchemaId{MessageType{33}, 5}))._fields,
                  std::vector<FieldId>{field_id});
        const auto third = message(33, 3, "BatchMessage", {field_id});
        require_value(registry.register_message(third));
        const auto third_reference = reference(mapped::SectionKind::SchemaIndex, key(third._schema));
        runtime::RuntimeAccess access(state());
        EXPECT_EQ(access.record_envelope(third_reference, 7)._previous, second_reference);
        EXPECT_EQ(access.record_envelope(second_reference, 7)._previous, first_reference);
      }
      fs::remove_all(_directory);
    }
  }

  // Corrupt stored indexes and definition fields only after capturing a clean
  // layout. Attachment must reject the malformed state without writing repairs;
  // raw atomic access lets the fixture inspect and restore evidence after rejection.
  TEST_F(SchemaRegistryTest, AttachmentRejectsMalformedFieldAndSchemaMetadataWithoutWrites)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto id = require_value(actor.register_field(field()));
    const auto schema = message(1, 1, "Quote", {id, id});
    require_value(actor.register_message(schema));
    const auto field_reference = reference(mapped::SectionKind::FieldIndex, static_cast<std::uint32_t>(id));
    const auto schema_reference = reference(mapped::SectionKind::SchemaIndex, key(schema._schema));
    std::uint64_t field_index{};
    std::uint64_t schema_index{};
    {
      runtime::RuntimeAccess access(state());
      field_index = access.section_offset(mapped::SectionKind::FieldIndex);
      schema_index = access.section_offset(mapped::SectionKind::SchemaIndex);
    }
    auto file = require_value(mapped::NativeFile::open_existing(_directory / "session.ang", mapped::FileAccess::ReadWrite));
    const auto length = require_value(file.size());
    const auto read_image = [&]
    {
      auto region = require_value(file.map_read_write(0, length));
      const auto bytes = require_value(region.writable_bytes());
      std::vector<std::uint64_t> words(static_cast<std::size_t>(length / 8));
      for (std::size_t index = 0; index < words.size(); ++index)
      {
        words[index] = require_value(mapped::AtomicWord::bind(bytes, index * 8)).load_acquire();
      }
      return words;
    };
    for (const auto target : {field_index + 32, field_index + 64 + 8,
                              field_reference + 64, field_reference + 64 + 72,
                              schema_index + 32, schema_reference + 64 + 72,
                              schema_reference + 64 + 88})
    {
      const auto old = read_image()[static_cast<std::size_t>(target / 8)];
      const auto invalid = target == schema_reference + 64 + 88 ?
          (old & 0xffffffff00000000ull) | 0xee123456ull : std::uint64_t{1};
      set_word(target, invalid);
      const auto evidence = read_image();
      expect_error(Session::attach(_directory), ErrorCode::IoError);
      EXPECT_EQ(read_image(), evidence);
      set_word(target, old);
    }
  }
} // namespace
