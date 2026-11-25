// Public TOML factories: a complete file describes the same Registry that an
// application can build with descriptors. These tests use ordinary files and
// public APIs; malformed files below are deliberate application input, not
// edits to Angaria's persisted representation.

#include "angaria/runtime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
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
  void expect_error(const Result<T>& result, ErrorCode code, std::string_view key = {})
  {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    const auto& error = std::get<Error>(result);
    EXPECT_EQ(error._code, code) << error._path << ": " << error._message;
    EXPECT_FALSE(error._message.empty());
    EXPECT_FALSE(error._path.empty());
    if (!key.empty())
    {
      EXPECT_NE(error._path.find(key), std::string::npos) << error._path << ": " << error._message;
    }
  }

  std::string replace_once(std::string input, std::string_view from, std::string_view to)
  {
    const auto position = input.find(from);
    if (position == std::string::npos)
    {
      throw std::runtime_error("Configuration fixture did not contain replacement text");
    }
    input.replace(position, from.size(), to);
    return input;
  }

  std::string read_bytes(const fs::path& path)
  {
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
      throw std::runtime_error("Cannot read fixture file " + path.string());
    }
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
  }

  std::set<std::string> directory_entries(const fs::path& directory)
  {
    std::set<std::string> result;
    for (const auto& entry : fs::directory_iterator(directory))
    {
      result.insert(entry.path().filename().string());
    }
    return result;
  }

  std::string settings()
  {
    return R"toml(config_version = 1

[session]
name = "Configuration example"
application_version = "sample-1"
directory = "session"
max_log_bytes = 1048576
storage = "two_plane"

[limits]
tasks = 8
queues = 4
source_sets = 4
publishing_sets = 4
memberships_per_set = 4
fields = 4
message_schemas = 4
symbols = 0
string_pool_bytes = 0

[defaults.queue]
ring_capacity = 8
delivery = "recover_from_log"

[defaults.source_set]
selection = "strict"
wait = "spin"

[defaults.publishing_set]
acceptance = "all_destinations"
)toml";
  }

  std::string tasks()
  {
    return R"toml(
[[tasks]]
name = "Setup"
kind = "process"
doc = "Creates the initial Registry"

[[tasks]]
name = "Feed"

[[tasks]]
name = "Consumer"
)toml";
  }

  std::string topology()
  {
    return R"toml(
[[source_sets]]
name = "Inputs"
owner = "Consumer"

[[queues]]
name = "Quotes"
producer = "Feed"
source_set = "Inputs"
priority = 7

[[publishing_sets]]
name = "Updates"
owner = "Feed"
destinations = ["Quotes"]
)toml";
  }

  std::string schemas()
  {
    return R"toml(
[[fields]]
name = "Price"
id = 0xEE000000
type = "int64"
doc = "Price in ticks"

[[fields]]
name = "Quantity"
id = 0xEEFFFFFF
type = "uint64"

[[messages]]
name = "Quote"
type = 1
version = 2
fields = ["Quantity", "Price", "Price"]

[[messages]]
name = "Quote"
type = 1
version = 1
fields = ["Price"]
)toml";
  }

  std::string complete_configuration()
  {
    return settings() + tasks() + topology() + schemas();
  }

  std::string logical_settings()
  {
    return replace_once(replace_once(settings(), "config_version = 1", "config_version = 2"),
                        "fields = 4", "fields = 8");
  }

  std::string logical_fields()
  {
    return R"toml(
[[fields]]
name = "TradingEnabled"
id = 0xEE000010
type = "bool"

[[fields]]
name = "OrderFlags"
id = 0xEE000011
type = "bits"
bits = 16

[[fields]]
name = "Side"
id = 0xEE000012
type = "enum"
underlying = "byte"
enumerators = [{ name = "Sell", value = 2 }, { name = "Buy", value = "1" }]

[[fields]]
name = "InstrumentCode"
id = 0xEE000013
type = "chars"
size = 6

[[fields]]
name = "Delta"
id = 0xEE000014
type = "int8"

[[fields]]
name = "SignedStatus"
id = 0xEE000015
type = "enum"
underlying = "int64"
enumerators = [
  { name = "Low", value = -9223372036854775808 },
  { name = "High", value = "9223372036854775807" }
]

[[fields]]
name = "ExternalStatus"
id = 0xEE000016
type = "enum"
underlying = "uint64"
enumerators = [
  { name = "AllBitsSet", value = "18446744073709551615" },
  { name = "HighBit", value = "9223372036854775808" }
]

[[messages]]
name = "OrderSettings"
type = 2
version = 1
fields = ["TradingEnabled", "OrderFlags", "Side", "InstrumentCode", "Delta", "SignedStatus", "ExternalStatus"]
)toml";
  }

  std::string one_logical_field(std::string_view declaration)
  {
    return logical_settings() + tasks() + "\n[[fields]]\nname = \"Logical\"\nid = 0xEE000010\n" +
           std::string{declaration} + "\n";
  }

  class FileConfigurationTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-configuration-XXXXXX").string();
      const auto directory = ::mkdtemp(pattern.data());
      if (!directory)
      {
        throw std::runtime_error("Cannot create temporary configuration directory");
      }
      _root = fs::canonical(directory);
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    fs::path write_configuration(const std::string& text, const std::string& name = "application.toml")
    {
      const auto path = _root / name;
      std::ofstream output(path, std::ios::binary);
      output.exceptions(std::ios::badbit | std::ios::failbit);
      output << text;
      output.close();
      return path;
    }

    void expect_creation_failure(const std::string& text, ErrorCode code,
                                 std::string_view key, std::string_view creator = "Setup")
    {
      const auto file = write_configuration(text);
      const auto before = directory_entries(_root);
      const auto result = Session::create_from_file(file, creator);
      expect_error(result, code, key);
      // Ordinary failure must leave no published or abandoned staging Session.
      // The caller can correct the same input and safely retry the same path.
      EXPECT_EQ(directory_entries(_root), before);
      EXPECT_FALSE(fs::exists(_root / "session"));
    }

    fs::path _root;
  };

  // Registration and activation are separate operations in the programmatic
  // API. A complete initial file performs both before publishing the Session.
  TEST_F(FileConfigurationTest, CreatesCompleteTopologyAndAttributesDefinitionsToDeclaredCreator)
  {
    const auto file = write_configuration(complete_configuration());
    auto created = require_value(Session::create_from_file(file, "Setup"));
    auto registry = created._session.registry();
    const auto setup = require_value(registry.find_task("Setup"));
    const auto feed = require_value(registry.find_task("Feed"));
    const auto consumer = require_value(registry.find_task("Consumer"));
    const auto source = require_value(registry.find_source_set("Inputs"));
    const auto queue = require_value(registry.find_queue("Quotes"));
    const auto publisher = require_value(registry.find_publishing_set("Updates"));

    EXPECT_EQ(created._creator.claim()._task, setup);
    EXPECT_EQ(require_value(created._session.metadata())._createdBy, setup);
    EXPECT_EQ(require_value(registry.describe_task(setup))._doc, "Creates the initial Registry");
    EXPECT_TRUE(require_value(registry.task_status(setup))._attached);
    EXPECT_FALSE(require_value(registry.task_status(feed))._attached);
    EXPECT_FALSE(require_value(registry.task_status(consumer))._attached);
    EXPECT_EQ(require_value(registry.metadata(feed))._createdBy, setup);
    EXPECT_EQ(require_value(registry.metadata(source))._createdBy, setup);
    EXPECT_EQ(require_value(registry.metadata(queue))._createdBy, setup);
    EXPECT_EQ(require_value(registry.metadata(publisher))._createdBy, setup);

    const auto connection = require_value(registry.describe_queue(queue));
    EXPECT_EQ(connection._producer, feed);
    EXPECT_EQ(connection._sourceSet, source);
    EXPECT_EQ(connection._priority, 7);
    EXPECT_EQ(connection._ringCapacity, 8u);
    EXPECT_EQ(connection._delivery, DeliveryPolicy::RecoverFromLog);
    EXPECT_EQ(require_value(registry.source_members(source)), (std::vector<QueueId>{queue}));
    EXPECT_EQ(require_value(registry.describe_source_set(source))._owner, consumer);
    EXPECT_EQ(require_value(registry.describe_publishing_set(publisher))._destinations,
              (std::vector<QueueId>{queue}));
    EXPECT_EQ(require_value(registry.state(source)), EntityState::Active);
    EXPECT_EQ(require_value(registry.state(queue)), EntityState::Active);
    EXPECT_EQ(require_value(registry.state(publisher)), EntityState::Active);

    // The configuration actor need not be either endpoint owner. Both owners
    // were offline during creation and can now attach and open their endpoints.
    auto producer = require_value(created._session.attach_task(feed));
    auto receiver = require_value(created._session.attach_task(consumer));
    EXPECT_EQ(require_value(producer.open_writer(queue)).id(), queue);
    EXPECT_EQ(require_value(producer.open_publishing_set(publisher)).id(), publisher);
    EXPECT_EQ(require_value(receiver.open_source_set(source)).id(), source);
  }

  TEST_F(FileConfigurationTest, ReservesCreatorOnceAndResolvesThreadParentDeclaredLater)
  {
    auto text = replace_once(settings(), "tasks = 8", "tasks = 3");
    text += R"toml(
[[tasks]]
name = "Worker"
kind = "thread"
process = "Host"

[[tasks]]
name = "Host"

[[tasks]]
name = "Setup"
)toml";
    auto created = require_value(Session::create_from_file(write_configuration(text), "Setup"));
    auto registry = created._creator.registry();
    const auto host = require_value(registry.find_task("Host"));
    const auto worker = require_value(registry.find_task("Worker"));
    const auto descriptor = require_value(registry.describe_task(worker));
    EXPECT_EQ(descriptor._kind, TaskKind::Thread);
    EXPECT_EQ(descriptor._process, host);
    TaskDescriptor setup;
    setup._name = "Setup";
    EXPECT_EQ(require_value(registry.register_task(setup)), created._creator.claim()._task);
    TaskDescriptor extra;
    extra._name = "Extra";
    expect_error(registry.register_task(extra), ErrorCode::CapacityExceeded);
  }

  TEST_F(FileConfigurationTest, PreservesFullFieldIdsExactSchemaVersionsOrderAndRepetition)
  {
    auto created = require_value(Session::create_from_file(
        write_configuration(complete_configuration()), "Setup"));
    auto registry = created._session.registry();
    const auto price = require_value(registry.find_field("Price"));
    const auto quantity = require_value(registry.find_field("Quantity"));
    EXPECT_EQ(static_cast<std::uint32_t>(price), 0xEE000000u);
    EXPECT_EQ(static_cast<std::uint32_t>(quantity), 0xEEFFFFFFu);
    EXPECT_EQ(require_value(registry.describe_field(price))._size, 8u);
    EXPECT_EQ(require_value(registry.describe_field(quantity))._base, EFieldBase::BASE_UINT64);
    const auto version_one = require_value(registry.find_message("Quote", 1));
    const auto version_two = require_value(registry.find_message("Quote", 2));
    EXPECT_EQ(require_value(registry.describe_message(version_one))._fields, (std::vector<FieldId>{price}));
    EXPECT_EQ(require_value(registry.describe_message(version_two))._fields,
              (std::vector<FieldId>{quantity, price, price}));
    EXPECT_EQ(require_value(registry.metadata(version_one))._createdBy, created._creator.claim()._task);
    expect_error(registry.find_message("Quote", 3), ErrorCode::NotFound);
  }

  // The loader is another way to describe normal public registrations. It
  // must not give a TOML-created Queue different defaults or ownership rules.
  TEST_F(FileConfigurationTest, ProducesEquivalentEffectiveDefinitionsToProgrammaticSetup)
  {
    auto from_file = require_value(Session::create_from_file(
        write_configuration(complete_configuration()), "Setup"));
    SessionOptions options;
    options._name = "Configuration example";
    options._applicationVersion = "sample-1";
    options._directory = _root / "programmatic";
    options._maxLogBytes = 1048576;
    options._limits = CapacityLimits::empty().tasks(8).queues(4).source_sets(4)
        .publishing_sets(4).memberships_per_set(4).fields(4).message_schemas(4);
    options._defaults._ringCapacity = 8;
    TaskDescriptor setup;
    setup._name = "Setup";
    setup._doc = "Creates the initial Registry";
    auto from_code = require_value(Session::create(options, setup));
    auto actor = from_code._creator.registry();
    TaskDescriptor feed_descriptor;
    feed_descriptor._name = "Feed";
    const auto feed = require_value(actor.register_task(feed_descriptor));
    TaskDescriptor consumer_descriptor;
    consumer_descriptor._name = "Consumer";
    const auto consumer = require_value(actor.register_task(consumer_descriptor));
    SourceQueueSetDescriptor inputs;
    inputs._name = "Inputs";
    inputs._owner = consumer;
    const auto source = require_value(actor.register_source_set(inputs));
    QueueDescriptor quotes;
    quotes._name = "Quotes";
    quotes._producer = feed;
    quotes._sourceSet = source;
    quotes._priority = 7;
    const auto queue = require_value(actor.register_queue(quotes));
    PublishingQueueSetDescriptor updates;
    updates._name = "Updates";
    updates._owner = feed;
    updates._destinations = {queue};
    const auto publisher = require_value(actor.register_publishing_set(updates));
    FieldDescriptor price;
    price._id = static_cast<FieldId>(0xEE000000u);
    price._name = "Price";
    price._base = EFieldBase::BASE_INT64;
    price._size = 8;
    price._doc = "Price in ticks";
    require_value(actor.register_field(price));
    FieldDescriptor quantity;
    quantity._id = static_cast<FieldId>(0xEEFFFFFFu);
    quantity._name = "Quantity";
    quantity._base = EFieldBase::BASE_UINT64;
    quantity._size = 8;
    require_value(actor.register_field(quantity));
    MessageDescriptor quote;
    quote._schema = {static_cast<MessageType>(1), 2};
    quote._name = "Quote";
    quote._fields = {quantity._id, price._id, price._id};
    require_value(actor.register_message(quote));
    quote._schema._version = 1;
    quote._fields = {price._id};
    require_value(actor.register_message(quote));
    require_value(actor.activate_source_set(source));
    require_value(actor.activate_queue(queue));
    require_value(actor.activate_publishing_set(publisher));

    auto file_registry = from_file._session.registry();
    const auto file_queue = require_value(file_registry.find_queue("Quotes"));
    const auto actual_queue = require_value(file_registry.describe_queue(file_queue));
    const auto expected_queue = require_value(actor.describe_queue(queue));
    EXPECT_EQ(actual_queue._name, expected_queue._name);
    EXPECT_EQ(actual_queue._priority, expected_queue._priority);
    EXPECT_EQ(actual_queue._ringCapacity, expected_queue._ringCapacity);
    EXPECT_EQ(actual_queue._delivery, expected_queue._delivery);
    EXPECT_EQ(require_value(file_registry.describe_task(actual_queue._producer))._name,
              require_value(actor.describe_task(expected_queue._producer))._name);
    const auto actual_source = require_value(file_registry.describe_source_set(actual_queue._sourceSet));
    const auto expected_source = require_value(actor.describe_source_set(source));
    EXPECT_EQ(actual_source._name, expected_source._name);
    EXPECT_EQ(actual_source._selection, expected_source._selection);
    EXPECT_EQ(actual_source._wait, expected_source._wait);
    EXPECT_EQ(require_value(file_registry.describe_task(actual_source._owner))._name,
              require_value(actor.describe_task(expected_source._owner))._name);
    for (const auto version : {1u, 2u})
    {
      const auto actual = require_value(file_registry.describe_message(
          require_value(file_registry.find_message("Quote", version))));
      const auto expected = require_value(actor.describe_message(require_value(actor.find_message("Quote", version))));
      EXPECT_EQ(actual._name, expected._name);
      EXPECT_EQ(actual._schema, expected._schema);
      EXPECT_EQ(actual._fields, expected._fields);
    }
    const auto actual_price = require_value(file_registry.describe_field(price._id));
    EXPECT_EQ(actual_price._name, price._name);
    EXPECT_EQ(actual_price._base, price._base);
    EXPECT_EQ(actual_price._size, price._size);
    EXPECT_EQ(actual_price._doc, price._doc);
    EXPECT_EQ(require_value(file_registry.state(file_queue)), require_value(actor.state(queue)));
    const auto actual_publisher = require_value(file_registry.describe_publishing_set(
        require_value(file_registry.find_publishing_set("Updates"))));
    EXPECT_EQ(actual_publisher._acceptance, require_value(actor.describe_publishing_set(publisher))._acceptance);
    EXPECT_EQ(actual_publisher._destinations, (std::vector<QueueId>{file_queue}));
  }

  TEST_F(FileConfigurationTest, LoadsDocumentedTradingConfigurationThroughPublicFactory)
  {
    // Copy the shipped example into this test's temporary directory, changing
    // only its destination. Tests never create/remove the reader's build path.
    const auto text = replace_once(read_bytes(ANGARIA_CONFIGURATION_EXAMPLE),
        "directory = \"../build/trading-session\"", "directory = \"session\"");
    auto created = require_value(Session::create_from_file(write_configuration(text), "MarketData"));
    auto registry = created._session.registry();
    const auto strategy = require_value(registry.find_task("Strategy"));
    const auto host = require_value(registry.find_task("StrategyHost"));
    EXPECT_EQ(require_value(registry.describe_task(strategy))._process, host);
    EXPECT_EQ(require_value(registry.source_members(require_value(registry.find_source_set("RiskInputs")))).size(), 2u);
    const auto publisher = require_value(registry.find_publishing_set("MarketDataUpdates"));
    EXPECT_EQ(require_value(registry.describe_publishing_set(publisher))._destinations.size(), 2u);
    EXPECT_EQ(require_value(registry.state(publisher)), EntityState::Active);
    EXPECT_EQ(require_value(registry.find_message("Quote", 1))._version, 1u);
  }

  TEST_F(FileConfigurationTest, OmittedDefaultsResolveAndEmptySetsRemainDraft)
  {
    auto text = settings();
    text.erase(text.find("[defaults.queue]"));
    text += tasks() + R"toml(
[[source_sets]]
name = "EmptyInputs"
owner = "Consumer"

[[publishing_sets]]
name = "EmptyOutputs"
owner = "Feed"
destinations = []

[[messages]]
name = "Control"
type = 2
version = 1
fields = []
)toml";
    auto created = require_value(Session::create_from_file(write_configuration(text), "Setup"));
    auto registry = created._session.registry();
    EXPECT_EQ(require_value(created._session.options())._defaults._ringCapacity, 1024u);
    EXPECT_EQ(require_value(registry.state(require_value(registry.find_source_set("EmptyInputs")))), EntityState::Draft);
    EXPECT_EQ(require_value(registry.state(require_value(registry.find_publishing_set("EmptyOutputs")))), EntityState::Draft);
    EXPECT_TRUE(require_value(registry.describe_message(require_value(registry.find_message("Control", 1))))._fields.empty());
  }

  TEST_F(FileConfigurationTest, AcceptsExplicitEmptyDeclarationArraysAndZeroUnusedCapacities)
  {
    const std::string empty = "queues = []\nsource_sets = []\npublishing_sets = []\nfields = []\nmessages = []\n";
    auto text = empty + settings();
    for (const auto* key : {"queues", "source_sets", "publishing_sets", "fields", "message_schemas"})
    {
      text = replace_once(text, std::string{key} + " = 4", std::string{key} + " = 0");
    }
    text += tasks();
    auto created = require_value(Session::create_from_file(write_configuration(text), "Setup"));
    EXPECT_EQ(require_value(created._session.options())._limits._queues, 0u);
    expect_error(created._session.registry().find_queue("Absent"), ErrorCode::NotFound);
  }

  TEST_F(FileConfigurationTest, ResolvesDirectoryRelativeToConfigurationInsteadOfWorkingDirectory)
  {
    const auto file = write_configuration(settings() + tasks());
    const auto previous = fs::current_path();
    fs::create_directory(_root / "other");
    struct RestoreDirectory
    {
      fs::path _previous;
      ~RestoreDirectory()
      {
        std::error_code ignored;
        fs::current_path(_previous, ignored);
      }
    } restore{previous};
    fs::current_path(_root / "other");
    auto created = require_value(Session::create_from_file(file, "Setup"));
    EXPECT_EQ(require_value(created._session.options())._directory, fs::canonical(_root / "session"));
    EXPECT_FALSE(fs::exists(_root / "other" / "session"));
    auto attached = require_value(Session::attach_from_file(file));
    EXPECT_EQ(attached.identity(), created._session.identity());
  }

  TEST_F(FileConfigurationTest, ResolvesSymlinkParentBeforeDotDotLikeProgrammaticCreation)
  {
    fs::create_directories(_root / "actual" / "subdirectory");
    fs::create_directory_symlink(_root / "actual" / "subdirectory", _root / "link");
    const auto text = replace_once(settings() + tasks(), "directory = \"session\"",
                                   "directory = \"link/../session\"");
    const auto file = write_configuration(text);
    auto created = require_value(Session::create_from_file(file, "Setup"));
    EXPECT_EQ(require_value(created._session.options())._directory, fs::canonical(_root / "actual" / "session"));
    EXPECT_FALSE(fs::exists(_root / "session"));
    auto attached = require_value(Session::attach_from_file(file));
    EXPECT_EQ(attached.identity(), created._session.identity());
  }

  // Attachment validates settings and references without replaying initial
  // registrations or claiming the creator. It succeeds while Setup is owned.
  TEST_F(FileConfigurationTest, AttachesWithoutChangingDefinitionsOrClaims)
  {
    const auto file = write_configuration(complete_configuration());
    auto created = require_value(Session::create_from_file(file, "Setup"));
    const auto image = read_bytes(_root / "session" / "session.ang");
    auto attached = require_value(Session::attach_from_file(file));
    EXPECT_EQ(attached.identity(), created._session.identity());
    EXPECT_EQ(read_bytes(_root / "session" / "session.ang"), image);
    auto registry = attached.registry();
    EXPECT_TRUE(require_value(registry.task_status(created._creator.claim()._task))._attached);
    EXPECT_FALSE(require_value(registry.task_status(require_value(registry.find_task("Feed"))))._attached);
    EXPECT_FALSE(require_value(registry.task_status(require_value(registry.find_task("Consumer"))))._attached);
    expect_error(attached.attach_task(created._creator.claim()._task), ErrorCode::OwnershipBusy);
  }

  TEST_F(FileConfigurationTest, AttachmentResolvesExistingReferencesWithoutApplyingNewDeclarations)
  {
    auto created = require_value(Session::create_from_file(
        write_configuration(complete_configuration()), "Setup"));
    const auto image = read_bytes(_root / "session" / "session.ang");
    const auto reference_only = settings() + R"toml(
[[publishing_sets]]
name = "NotRegisteredByAttachment"
owner = "Feed"
destinations = ["Quotes"]
)toml";
    auto attached = require_value(Session::attach_from_file(write_configuration(reference_only, "attach.toml")));
    EXPECT_EQ(attached.identity(), created._session.identity());
    expect_error(attached.registry().find_publishing_set("NotRegisteredByAttachment"), ErrorCode::NotFound);
    EXPECT_EQ(read_bytes(_root / "session" / "session.ang"), image);
  }

  TEST_F(FileConfigurationTest, AttachmentKeepsDistinctNewAndRegisteredQueueReferencesDistinct)
  {
    auto created = require_value(Session::create_from_file(
        write_configuration(complete_configuration()), "Setup"));
    const auto image = read_bytes(_root / "session" / "session.ang");
    const auto text = settings() + R"toml(
[[queues]]
name = "DeclaredButNotRegistered"
producer = "Feed"
source_set = "Inputs"

[[publishing_sets]]
name = "MixedReferences"
owner = "Feed"
destinations = ["DeclaredButNotRegistered", "Quotes"]
)toml";
    // The first new declaration has no live Queue ID. It must not accidentally
    // alias the first registered Queue merely because both occupy position one.
    auto attached = require_value(Session::attach_from_file(write_configuration(text, "mixed.toml")));
    expect_error(attached.registry().find_queue("DeclaredButNotRegistered"), ErrorCode::NotFound);
    expect_error(attached.registry().find_publishing_set("MixedReferences"), ErrorCode::NotFound);
    EXPECT_EQ(read_bytes(_root / "session" / "session.ang"), image);
  }

  TEST_F(FileConfigurationTest, AttachmentRejectsUnownedRegisteredDestinationWithConfigurationDiagnostic)
  {
    auto created = require_value(Session::create_from_file(
        write_configuration(complete_configuration()), "Setup"));
    QueueDescriptor draft;
    draft._name = "UnownedDraft";
    require_value(created._creator.registry().register_queue(draft));
    const auto image = read_bytes(_root / "session" / "session.ang");
    const auto text = settings() + R"toml(
[[publishing_sets]]
name = "InvalidReferences"
owner = "Feed"
destinations = ["UnownedDraft"]
)toml";
    expect_error(Session::attach_from_file(write_configuration(text, "unowned.toml")),
                 ErrorCode::InvalidConfiguration, "destinations");
    EXPECT_EQ(read_bytes(_root / "session" / "session.ang"), image);
  }

  TEST_F(FileConfigurationTest, AttachmentRejectsMalformedReferenceNamesBeforeRegistryLookup)
  {
    auto created = require_value(Session::create_from_file(
        write_configuration(complete_configuration()), "Setup"));
    const auto image = read_bytes(_root / "session" / "session.ang");
    const auto publisher = settings() + R"toml(
[[publishing_sets]]
name = "ReferenceCheck"
owner = "Feed"
destinations = ["Quotes"]
)toml";
    const auto message = settings() + R"toml(
[[messages]]
name = "ReferenceMessage"
type = 2
version = 1
fields = ["Price"]
)toml";
    const std::vector<std::pair<std::string, const char*>> invalid{
        {replace_once(publisher, "owner = \"Feed\"", "owner = \"Feed\\u0000suffix\""), "owner"},
        {replace_once(publisher, "destinations = [\"Quotes\"]", "destinations = [\"Quotes\\u0000suffix\"]"), "destinations"},
        {replace_once(publisher, "owner = \"Feed\"", std::string{"owner = \"Bad"} + char{static_cast<char>(0xff)} + "\""), "owner"},
        {replace_once(message, "fields = [\"Price\"]", "fields = [\"Price\\u0000suffix\"]"), "fields"}};
    for (const auto& [text, key] : invalid)
    {
      SCOPED_TRACE(key);
      expect_error(Session::attach_from_file(write_configuration(text, "reference.toml")),
                   ErrorCode::InvalidConfiguration, key);
      EXPECT_EQ(read_bytes(_root / "session" / "session.ang"), image);
    }
  }

  TEST_F(FileConfigurationTest, AttachmentUsesActualDirectoryAfterSessionRelocation)
  {
    const auto valid = complete_configuration();
    SessionId identity;
    {
      auto created = require_value(Session::create_from_file(write_configuration(valid), "Setup"));
      identity = created._session.identity();
    }
    fs::rename(_root / "session", _root / "relocated");
    const auto relocated = replace_once(valid, "directory = \"session\"", "directory = \"relocated\"");
    const auto image = read_bytes(_root / "relocated" / "session.ang");
    auto attached = require_value(Session::attach_from_file(write_configuration(relocated)));
    EXPECT_EQ(attached.identity(), identity);
    EXPECT_EQ(require_value(attached.options())._directory, fs::canonical(_root / "relocated"));
    EXPECT_EQ(read_bytes(_root / "relocated" / "session.ang"), image);
    EXPECT_FALSE(fs::exists(_root / "session"));
  }

  TEST_F(FileConfigurationTest, AttachmentDoesNotRestoreDestinationsChangedByRuntimeOwner)
  {
    auto text = replace_once(complete_configuration(), "destinations = [\"Quotes\"]",
                            "destinations = [\"Quotes\", \"MoreQuotes\"]");
    text += "\n[[queues]]\nname = \"MoreQuotes\"\nproducer = \"Feed\"\nsource_set = \"Inputs\"\n";
    const auto file = write_configuration(text);
    auto created = require_value(Session::create_from_file(file, "Setup"));
    auto registry = created._session.registry();
    const auto publisher = require_value(registry.find_publishing_set("Updates"));
    const auto more = require_value(registry.find_queue("MoreQuotes"));
    auto producer = require_value(created._session.attach_task(require_value(registry.find_task("Feed"))));
    const std::vector<QueueId> destinations{more};
    require_value(producer.registry().set_destinations(publisher, destinations));
    const auto revision = require_value(registry.metadata(publisher))._revision;
    const auto image = read_bytes(_root / "session" / "session.ang");

    // The initial file still lists both destinations. Attachment checks the
    // configuration, then preserves the owner's committed runtime topology.
    auto attached = require_value(Session::attach_from_file(file));
    EXPECT_EQ(require_value(attached.registry().describe_publishing_set(publisher))._destinations, destinations);
    EXPECT_EQ(require_value(attached.registry().metadata(publisher))._revision, revision);
    EXPECT_EQ(read_bytes(_root / "session" / "session.ang"), image);
  }

  TEST_F(FileConfigurationTest, AttachmentComparesEffectiveSessionSettingsWithoutMutatingHealthySession)
  {
    const auto valid = complete_configuration();
    auto created = require_value(Session::create_from_file(write_configuration(valid), "Setup"));
    const auto image = read_bytes(_root / "session" / "session.ang");
    struct Change
    {
      const char* _from;
      const char* _to;
      const char* _key;
    };
    const std::array changes{
        Change{"name = \"Configuration example\"", "name = \"Different\"", "session.name"},
        Change{"application_version = \"sample-1\"", "application_version = \"sample-2\"", "application_version"},
        Change{"max_log_bytes = 1048576", "max_log_bytes = 2097152", "max_log_bytes"},
        Change{"tasks = 8", "tasks = 9", "tasks"},
        Change{"queues = 4", "queues = 5", "queues"},
        Change{"string_pool_bytes = 0", "string_pool_bytes = 64", "string_pool_bytes"},
        Change{"ring_capacity = 8", "ring_capacity = 16", "ring_capacity"}};
    for (const auto& change : changes)
    {
      SCOPED_TRACE(change._key);
      const auto file = write_configuration(replace_once(valid, change._from, change._to), "mismatch.toml");
      expect_error(Session::attach_from_file(file), ErrorCode::ConfigurationMismatch, change._key);
      EXPECT_EQ(read_bytes(_root / "session" / "session.ang"), image);
    }
  }

  TEST_F(FileConfigurationTest, RejectsMalformedSyntaxWithFilenameAndLineDiagnostic)
  {
    const auto file = write_configuration("config_version = 1\n[session\n", "broken.toml");
    const auto result = Session::create_from_file(file, "Setup");
    expect_error(result, ErrorCode::InvalidConfiguration);
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    const auto& error = std::get<Error>(result);
    EXPECT_NE((error._path + error._message).find("broken.toml"), std::string::npos);
    EXPECT_NE(error._message.find("line"), std::string::npos);
    EXPECT_FALSE(fs::exists(_root / "session"));
  }

  TEST_F(FileConfigurationTest, ReportsMissingConfigurationFileAsIoError)
  {
    const auto file = _root / "missing.toml";
    const auto result = Session::create_from_file(file, "Setup");
    expect_error(result, ErrorCode::IoError);
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    const auto& error = std::get<Error>(result);
    EXPECT_NE((error._path + error._message).find("missing.toml"), std::string::npos);
  }

  TEST_F(FileConfigurationTest, RejectsUnknownKeysTablesAndWrongExactTypes)
  {
    struct Invalid
    {
      std::string _text;
      const char* _key;
    };
    const auto valid = complete_configuration();
    const std::vector invalid{
        Invalid{"unknown_setting = 1\n" + valid, "unknown_setting"},
        Invalid{valid + "\n[misspelled]\nvalue = 1\n", "misspelled"},
        Invalid{replace_once(valid, "max_log_bytes = 1048576", "max_log_bites = 1048576"), "max_log_bites"},
        Invalid{replace_once(valid, "ring_capacity = 8", "ring_capacity = 8.0"), "ring_capacity"},
        Invalid{replace_once(valid, "tasks = 8", "tasks = true"), "tasks"},
        Invalid{replace_once(valid, "priority = 7", "priority = \"7\""), "priority"},
        Invalid{replace_once(valid, "config_version = 1", "config_version = 1.0"), "config_version"},
        Invalid{replace_once(valid, "destinations = [\"Quotes\"]", "destinations = \"Quotes\""), "destinations"},
        Invalid{replace_once(valid, "fields = [\"Price\"]", "fields = [1]"), "fields"},
        Invalid{replace_once(valid, "kind = \"process\"", "kind = 1"), "kind"}};
    for (const auto& value : invalid)
    {
      SCOPED_TRACE(value._key);
      expect_creation_failure(value._text, ErrorCode::InvalidConfiguration, value._key);
    }
  }

  TEST_F(FileConfigurationTest, RejectsUnsignedSignRangeAndRequiredValueFailures)
  {
    struct Invalid
    {
      const char* _from;
      const char* _to;
      const char* _key;
    };
    const auto valid = complete_configuration();
    const std::array invalid{
        Invalid{"config_version = 1", "config_version = 3", "config_version"},
        Invalid{"tasks = 8", "tasks = -1", "tasks"},
        Invalid{"tasks = 8", "tasks = 4294967296", "tasks"},
        Invalid{"priority = 7", "priority = -1", "priority"},
        Invalid{"priority = 7", "priority = 256", "priority"},
        Invalid{"ring_capacity = 8", "ring_capacity = 0", "ring_capacity"},
        Invalid{"version = 2", "version = 0", "version"},
        Invalid{"version = 2", "version = 4294967296", "version"},
        Invalid{"type = 1", "type = 0", "type"},
        Invalid{"type = 1", "type = 65536", "type"},
        Invalid{"id = 0xEE000000", "id = 0xEF000001", "id"},
        Invalid{"id = 0xEE000000", "id = -1", "id"},
        Invalid{"name = \"Setup\"", "name = \"\"", "name"},
        Invalid{"tasks = 8\n", "", "tasks"},
        Invalid{"max_log_bytes = 1048576", "max_log_bytes = 0", "max_log_bytes"}};
    for (const auto& value : invalid)
    {
      SCOPED_TRACE(value._key);
      expect_creation_failure(replace_once(valid, value._from, value._to), ErrorCode::InvalidConfiguration, value._key);
    }
  }

  TEST_F(FileConfigurationTest, RejectsNamesWithNullOrInvalidUtf8ButPreservesLengthDelimitedDocumentation)
  {
    const auto valid = complete_configuration();
    expect_creation_failure(replace_once(valid, "name = \"Setup\"", "name = \"Bad\\u0000Name\""),
                            ErrorCode::InvalidConfiguration, "name");
    expect_creation_failure(replace_once(valid, "name = \"Setup\"", std::string{"name = \"Bad"} + char{static_cast<char>(0xff)} + "\""),
                            ErrorCode::InvalidConfiguration, "name");
    auto with_doc = replace_once(valid, "doc = \"Creates the initial Registry\"", "doc = \"has\\u0000nul\"");
    auto created = require_value(Session::create_from_file(write_configuration(with_doc), "Setup"));
    const auto descriptor = require_value(created._session.registry().describe_task(created._creator.claim()._task));
    EXPECT_EQ(descriptor._doc, (std::string{"has\0nul", 7}));
  }

  TEST_F(FileConfigurationTest, RejectsUndeclaredThreadCreatorAndInvalidParentRelationships)
  {
    const auto valid = complete_configuration();
    expect_creation_failure(valid, ErrorCode::InvalidConfiguration, "creator", "Absent");
    expect_creation_failure(valid, ErrorCode::InvalidConfiguration, "creator", "");
    auto threaded = valid + "\n[[tasks]]\nname = \"Worker\"\nkind = \"thread\"\nprocess = \"Consumer\"\n";
    expect_creation_failure(threaded, ErrorCode::InvalidConfiguration, "creator", "Worker");
    expect_creation_failure(replace_once(threaded, "process = \"Consumer\"", "process = \"Missing\""),
                            ErrorCode::InvalidConfiguration, "process");
    expect_creation_failure(replace_once(threaded, "process = \"Consumer\"", "process = \"Worker\""),
                            ErrorCode::InvalidConfiguration, "process");
    expect_creation_failure(replace_once(valid, "kind = \"process\"", "kind = \"process\"\nprocess = \"Feed\""),
                            ErrorCode::InvalidConfiguration, "process");
  }

  TEST_F(FileConfigurationTest, RejectsUnresolvedReferencesWrongKindsAndProducerMismatch)
  {
    struct Invalid
    {
      const char* _from;
      const char* _to;
      const char* _key;
    };
    const auto valid = complete_configuration();
    const std::array invalid{
        Invalid{"owner = \"Consumer\"", "owner = \"Missing\"", "owner"},
        Invalid{"producer = \"Feed\"", "producer = \"Missing\"", "producer"},
        Invalid{"source_set = \"Inputs\"", "source_set = \"Updates\"", "source_set"},
        Invalid{"destinations = [\"Quotes\"]", "destinations = [\"Missing\"]", "destinations"},
        Invalid{"owner = \"Feed\"", "owner = \"Consumer\"", "destinations"},
        Invalid{"fields = [\"Price\"]", "fields = [\"Missing\"]", "fields"}};
    for (const auto& value : invalid)
    {
      SCOPED_TRACE(value._key);
      expect_creation_failure(replace_once(valid, value._from, value._to), ErrorCode::InvalidConfiguration, value._key);
    }
  }

  TEST_F(FileConfigurationTest, RejectsDuplicateDeclarationsSharedSetNamesAndSchemaFamilyConflicts)
  {
    const auto valid = complete_configuration();
    const std::vector<std::pair<std::string, const char*>> invalid{
        {valid + "\n[[tasks]]\nname = \"Feed\"\n", "name"},
        {replace_once(valid, "name = \"Updates\"", "name = \"Inputs\""), "name"},
        {replace_once(valid, "id = 0xEEFFFFFF", "id = 0xEE000000"), "id"},
        {replace_once(valid, "name = \"Quantity\"", "name = \"Price\""), "name"},
        {replace_once(valid, "version = 2", "version = 1"), "version"},
        {valid + "\n[[messages]]\nname = \"AnotherName\"\ntype = 1\nversion = 3\nfields = []\n", "name"},
        {valid + "\n[[messages]]\nname = \"Quote\"\ntype = 2\nversion = 3\nfields = []\n", "type"},
        {replace_once(valid, "destinations = [\"Quotes\"]", "destinations = [\"Quotes\", \"Quotes\"]"), "destinations"}};
    for (const auto& [text, key] : invalid)
    {
      SCOPED_TRACE(key);
      expect_creation_failure(text, ErrorCode::InvalidConfiguration, key);
    }
  }

  TEST_F(FileConfigurationTest, DistinguishesUnknownPoliciesFromKnownUnsupportedPolicies)
  {
    struct Policy
    {
      const char* _from;
      const char* _to;
      const char* _key;
      ErrorCode _code;
    };
    const auto valid = complete_configuration();
    const std::array policies{
        Policy{"storage = \"two_plane\"", "storage = \"segmented\"", "storage", ErrorCode::UnsupportedPolicy},
        Policy{"selection = \"strict\"", "selection = \"fair\"", "selection", ErrorCode::UnsupportedPolicy},
        Policy{"wait = \"spin\"", "wait = \"eventfd\"", "wait", ErrorCode::UnsupportedPolicy},
        Policy{"delivery = \"recover_from_log\"", "delivery = \"allow_loss\"", "delivery", ErrorCode::UnsupportedPolicy},
        Policy{"acceptance = \"all_destinations\"", "acceptance = \"partial\"", "acceptance", ErrorCode::UnsupportedPolicy},
        Policy{"selection = \"strict\"", "selection = \"strcit\"", "selection", ErrorCode::InvalidConfiguration},
        Policy{"type = \"int64\"", "type = \"string\"", "type", ErrorCode::UnsupportedPolicy},
        Policy{"type = \"int64\"", "type = \"integer\"", "type", ErrorCode::InvalidConfiguration}};
    for (const auto& policy : policies)
    {
      SCOPED_TRACE(policy._key);
      auto text = replace_once(valid, policy._from, policy._to);
      if (std::string_view{policy._key} == "storage")
      {
        // Segmented storage is known but not implemented. Remove the explicit
        // two-plane-only ring setting so that this checks support, not syntax.
        text = replace_once(text, "ring_capacity = 8\n", "");
      }
      expect_creation_failure(text, policy._code, policy._key);
    }
    expect_creation_failure(replace_once(valid, "storage = \"two_plane\"", "storage = \"segmented\""),
                            ErrorCode::InvalidConfiguration, "ring_capacity");
    expect_creation_failure(replace_once(valid, "owner = \"Consumer\"", "owner = \"Consumer\"\nrandom_seed = 1"),
                            ErrorCode::InvalidConfiguration, "random_seed");
    expect_creation_failure(replace_once(valid, "storage = \"two_plane\"", "storage = \"two_plane\"\nsegment_bytes = 1048576"),
                            ErrorCode::InvalidConfiguration, "segment_bytes");
    expect_creation_failure(replace_once(valid, "source_set = \"Inputs\"", "source_set = \"Inputs\"\ndelivery = \"allow_loss\""),
                            ErrorCode::UnsupportedPolicy, "delivery");
  }

  TEST_F(FileConfigurationTest, ChecksEveryDeclarationCapacityAndSourceMembershipBeforePublication)
  {
    const auto valid = complete_configuration();
    const std::array<std::pair<const char*, const char*>, 6> limits{{
        {"tasks = 8", "tasks = 2"}, {"queues = 4", "queues = 0"},
        {"source_sets = 4", "source_sets = 0"}, {"publishing_sets = 4", "publishing_sets = 0"},
        {"fields = 4", "fields = 1"}, {"message_schemas = 4", "message_schemas = 1"}}};
    for (const auto& [from, to] : limits)
    {
      SCOPED_TRACE(from);
      expect_creation_failure(replace_once(valid, from, to), ErrorCode::CapacityExceeded, "limits");
    }
    auto members = replace_once(valid, "memberships_per_set = 4", "memberships_per_set = 1");
    members += "\n[[queues]]\nname = \"MoreQuotes\"\nproducer = \"Feed\"\nsource_set = \"Inputs\"\n";
    expect_creation_failure(members, ErrorCode::CapacityExceeded, "memberships_per_set");
  }

  TEST_F(FileConfigurationTest, IncludesConfiguredRingsInByteBudgetAndLeavesNoPartiallyPublishedSession)
  {
    const auto valid = complete_configuration();
    // The count reservations fit, but this ring alone is larger than the
    // entire byte cap. A valid descriptor is not permission to publish it.
    const auto huge_ring = replace_once(valid, "priority = 7", "priority = 7\nring_capacity = 1048576");
    expect_creation_failure(huge_ring, ErrorCode::LogFull, "max_log_bytes");
    expect_creation_failure(replace_once(valid, "max_log_bytes = 1048576", "max_log_bytes = 4096"),
                            ErrorCode::LogFull, "max_log_bytes");
    // A corrected file can immediately use the originally requested path.
    auto created = require_value(Session::create_from_file(write_configuration(valid), "Setup"));
    EXPECT_TRUE(fs::exists(_root / "session" / "session.ang"));
    EXPECT_EQ(require_value(created._session.options())._name, "Configuration example");
  }

  TEST_F(FileConfigurationTest, CompleteFileFitsExactByteBudgetAndRejectsOneByteShort)
  {
    // Multiple members retain progressively larger Source definitions. Empty
    // sets skip activation, while the second nonempty Source has its own count.
    // Longer documentation also exercises record-alignment byte accounting.
    const auto valid = complete_configuration() + R"toml(
[[source_sets]]
name = "OtherInputs"
owner = "Consumer"
doc = "Additional inputs use another Source and retain their own independently sized definition."

[[source_sets]]
name = "EmptyInputs"
owner = "Consumer"

[[queues]]
name = "MoreQuotes"
producer = "Feed"
source_set = "Inputs"

[[queues]]
name = "OtherQuotes"
producer = "Feed"
source_set = "OtherInputs"
ring_capacity = 16

[[publishing_sets]]
name = "WiderUpdates"
owner = "Feed"
destinations = ["Quotes", "MoreQuotes", "OtherQuotes"]

[[publishing_sets]]
name = "EmptyOutputs"
owner = "Feed"
destinations = []
)toml";
    std::uint64_t required{};
    {
      auto created = require_value(Session::create_from_file(write_configuration(valid), "Setup"));
      required = fs::file_size(_root / "session" / "session.ang");
    }
    fs::remove_all(_root / "session");
    const auto short_file = replace_once(valid, "max_log_bytes = 1048576",
        "max_log_bytes = " + std::to_string(required - 1));
    expect_creation_failure(short_file, ErrorCode::LogFull, "max_log_bytes");
    const auto exact_file = replace_once(valid, "max_log_bytes = 1048576",
        "max_log_bytes = " + std::to_string(required));
    auto created = require_value(Session::create_from_file(write_configuration(exact_file), "Setup"));
    EXPECT_EQ(fs::file_size(_root / "session" / "session.ang"), required);
    const auto queue = require_value(created._session.registry().find_queue("Quotes"));
    EXPECT_EQ(require_value(created._session.registry().state(queue)), EntityState::Active);
    const auto empty = require_value(created._session.registry().find_source_set("EmptyInputs"));
    EXPECT_EQ(require_value(created._session.registry().state(empty)), EntityState::Draft);
  }

  TEST_F(FileConfigurationTest, NeverOverwritesAnExistingSessionDirectory)
  {
    const auto file = write_configuration(complete_configuration());
    auto created = require_value(Session::create_from_file(file, "Setup"));
    const auto image = read_bytes(_root / "session" / "session.ang");
    const auto entries = directory_entries(_root);
    expect_error(Session::create_from_file(file, "Setup"), ErrorCode::Conflict, "directory");
    EXPECT_EQ(read_bytes(_root / "session" / "session.ang"), image);
    EXPECT_EQ(directory_entries(_root), entries);
    EXPECT_EQ(require_value(Session::attach(_root / "session")).identity(), created._session.identity());
  }

  // Version 2 adds logical meaning without changing the earlier scalar or
  // topology spelling. Existing version-1 files remain useful input.
  TEST_F(FileConfigurationTest, AcceptsBothConfigurationVersionsForExistingScalarDefinitions)
  {
    const auto first = write_configuration(complete_configuration(), "first.toml");
    const auto second = write_configuration(replace_once(replace_once(complete_configuration(),
        "config_version = 1", "config_version = 2"), "directory = \"session\"", "directory = \"second\""),
        "second.toml");
    auto old_file = require_value(Session::create_from_file(first, "Setup"));
    auto new_file = require_value(Session::create_from_file(second, "Setup"));
    const auto old_price = require_value(old_file._session.registry().describe_field(static_cast<FieldId>(0xEE000000u)));
    const auto new_price = require_value(new_file._session.registry().describe_field(old_price._id));
    EXPECT_EQ(new_price._name, old_price._name);
    EXPECT_EQ(new_price._base, old_price._base);
    EXPECT_EQ(new_price._size, old_price._size);
    EXPECT_EQ(new_price._logical, old_price._logical);
    EXPECT_EQ(new_price._logical._kind, FieldLogicalKind::None);
  }

  TEST_F(FileConfigurationTest, CreatesLogicalFieldsWithOwningCanonicalMetadataAndFullIntegerRanges)
  {
    auto created = require_value(Session::create_from_file(
        write_configuration(logical_settings() + tasks() + logical_fields()), "Setup"));
    auto registry = created._creator.registry();
    const auto enabled = require_value(registry.describe_field(require_value(registry.find_field("TradingEnabled"))));
    EXPECT_EQ(enabled._base, EFieldBase::BASE_BYTE);
    EXPECT_EQ(enabled._size, 1u);
    EXPECT_EQ(enabled._logical._kind, FieldLogicalKind::Boolean);
    EXPECT_TRUE(enabled._logical._enumerators.empty());
    const auto flags = require_value(registry.describe_field(require_value(registry.find_field("OrderFlags"))));
    EXPECT_EQ(flags._base, EFieldBase::BASE_UINT16);
    EXPECT_EQ(flags._size, 2u);
    EXPECT_EQ(flags._logical._kind, FieldLogicalKind::Bits);
    const auto characters = require_value(registry.describe_field(require_value(registry.find_field("InstrumentCode"))));
    EXPECT_EQ(characters._base, EFieldBase::BASE_CHARS);
    EXPECT_EQ(characters._size, 6u);
    EXPECT_EQ(characters._logical._kind, FieldLogicalKind::FixedAscii);
    const auto delta = require_value(registry.describe_field(require_value(registry.find_field("Delta"))));
    EXPECT_EQ(delta._base, EFieldBase::BASE_INT8);
    EXPECT_EQ(delta._size, 1u);
    EXPECT_EQ(delta._logical._kind, FieldLogicalKind::None);

    // The same independently constructed public descriptor re-registers
    // idempotently, regardless of TOML integer/string spelling or entry order.
    FieldDescriptor side;
    side._id = static_cast<FieldId>(0xEE000012u);
    side._name = "Side";
    side._base = EFieldBase::BASE_BYTE;
    side._size = 1;
    side._logical._kind = FieldLogicalKind::Enumeration;
    side._logical._enumerators = {{"Buy", std::uint64_t{1}}, {"Sell", std::uint64_t{2}}};
    EXPECT_EQ(require_value(registry.describe_field(side._id))._logical, side._logical);
    const auto revision = require_value(registry.metadata(side._id))._revision;
    std::reverse(side._logical._enumerators.begin(), side._logical._enumerators.end());
    EXPECT_EQ(require_value(registry.register_field(side)), side._id);
    EXPECT_EQ(require_value(registry.metadata(side._id))._revision, revision);
    side._logical._enumerators.front()._name = "DifferentMeaning";
    expect_error(registry.register_field(side), ErrorCode::Conflict);

    const auto signed_status = require_value(registry.describe_field(require_value(registry.find_field("SignedStatus"))));
    const std::vector<EnumeratorDescriptor> signed_values{
        {"Low", std::numeric_limits<std::int64_t>::min()},
        {"High", std::numeric_limits<std::int64_t>::max()}};
    EXPECT_EQ(signed_status._logical._enumerators, signed_values);
    const auto external = require_value(registry.describe_field(require_value(registry.find_field("ExternalStatus"))));
    const std::vector<EnumeratorDescriptor> unsigned_values{
        {"HighBit", std::uint64_t{1} << 63}, {"AllBitsSet", std::numeric_limits<std::uint64_t>::max()}};
    EXPECT_EQ(external._logical._enumerators, unsigned_values);
    EXPECT_EQ(require_value(registry.describe_message(require_value(registry.find_message("OrderSettings", 1))))._fields.size(), 7u);
  }

  TEST_F(FileConfigurationTest, AcceptsEveryBasicBitWidthAndMaximumFixedAsciiSizeWithoutPayloadAllocation)
  {
    const std::array<std::pair<unsigned, EFieldBase>, 4> widths{{
        {8, EFieldBase::BASE_BYTE}, {16, EFieldBase::BASE_UINT16},
        {32, EFieldBase::BASE_UINT32}, {64, EFieldBase::BASE_UINT64}}};
    for (const auto& [bits, base] : widths)
    {
      SCOPED_TRACE(bits);
      auto text = one_logical_field("type = \"bits\"\nbits = " + std::to_string(bits));
      text = replace_once(text, "directory = \"session\"", "directory = \"bits-" + std::to_string(bits) + "\"");
      auto created = require_value(Session::create_from_file(write_configuration(text), "Setup"));
      const auto descriptor = require_value(created._session.registry().describe_field(static_cast<FieldId>(0xEE000010u)));
      EXPECT_EQ(descriptor._base, base);
      EXPECT_EQ(descriptor._size, bits / 8);
      EXPECT_EQ(descriptor._logical._kind, FieldLogicalKind::Bits);
    }
    // A Field definition records a width; it does not allocate a value buffer
    // of that width. Payload construction has its own explicit byte budget.
    auto created = require_value(Session::create_from_file(write_configuration(
        one_logical_field("type = \"chars\"\nsize = 4294967295")), "Setup"));
    EXPECT_EQ(require_value(created._session.registry().describe_field(static_cast<FieldId>(0xEE000010u)))._size,
              std::numeric_limits<std::uint32_t>::max());
  }

  TEST_F(FileConfigurationTest, NormalizesEveryEnumUnderlyingIntegerType)
  {
    struct Underlying
    {
      const char* _name;
      const char* _low;
      const char* _high;
      EnumerationNumber _minimum;
      EnumerationNumber _maximum;
    };
    const std::array types{
        Underlying{"byte", "0", "255", std::uint64_t{0}, std::uint64_t{255}},
        Underlying{"int8", "-128", "127", std::int64_t{-128}, std::int64_t{127}},
        Underlying{"int16", "-32768", "32767", std::int64_t{-32768}, std::int64_t{32767}},
        Underlying{"uint16", "0", "65535", std::uint64_t{0}, std::uint64_t{65535}},
        Underlying{"int32", "-2147483648", "2147483647", std::int64_t{-2147483648LL}, std::int64_t{2147483647}},
        Underlying{"uint32", "0", "4294967295", std::uint64_t{0}, std::uint64_t{4294967295ULL}},
        Underlying{"int64", "-9223372036854775808", "9223372036854775807",
                   std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()},
        Underlying{"uint64", "0", "18446744073709551615", std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()}};
    for (const auto& type : types)
    {
      SCOPED_TRACE(type._name);
      const auto declaration = std::string{"type = \"enum\"\nunderlying = \""} + type._name +
          "\"\nenumerators = [{ name = \"High\", value = \"" + type._high +
          "\" }, { name = \"Low\", value = \"" + type._low + "\" }]";
      const auto text = replace_once(one_logical_field(declaration), "directory = \"session\"",
          std::string{"directory = \"enum-"} + type._name + "\"");
      auto created = require_value(Session::create_from_file(write_configuration(text), "Setup"));
      const auto descriptor = require_value(created._session.registry().describe_field(static_cast<FieldId>(0xEE000010u)));
      ASSERT_EQ(descriptor._logical._enumerators.size(), 2u);
      EXPECT_EQ(descriptor._logical._enumerators[0]._name, "Low");
      EXPECT_EQ(descriptor._logical._enumerators[0]._value, type._minimum);
      EXPECT_EQ(descriptor._logical._enumerators[1]._value, type._maximum);
    }
  }

  TEST_F(FileConfigurationTest, RequiresVersionTwoForLogicalTypesAndTheirKeys)
  {
    const std::array declarations{
        "type = \"bool\"", "type = \"bits\"\nbits = 8", "type = \"int8\"",
        "type = \"chars\"\nsize = 6",
        "type = \"enum\"\nunderlying = \"byte\"\nenumerators = [{ name = \"A\", value = 1 }]"};
    for (const auto* declaration : declarations)
    {
      SCOPED_TRACE(declaration);
      const auto text = replace_once(one_logical_field(declaration), "config_version = 2", "config_version = 1");
      const auto result = Session::create_from_file(write_configuration(text), "Setup");
      expect_error(result, ErrorCode::InvalidConfiguration, "fields[0]");
      ASSERT_TRUE(std::holds_alternative<Error>(result));
      EXPECT_NE(std::get<Error>(result)._message.find("config_version = 2"), std::string::npos);
      EXPECT_FALSE(fs::exists(_root / "session"));
    }
  }

  TEST_F(FileConfigurationTest, RejectsInapplicableKeysWrongTypesAndLogicalBoundsBeforePublishing)
  {
    struct Invalid
    {
      const char* _declaration;
      const char* _key;
    };
    const std::array values{
        Invalid{"type = \"bool\"\nsize = 1", "size"},
        Invalid{"type = \"byte\"\nbits = 8", "bits"},
        Invalid{"type = \"chars\"\nsize = 0", "size"},
        Invalid{"type = \"chars\"\nsize = 4294967296", "size"},
        Invalid{"type = \"chars\"\nsize = \"6\"", "size"},
        Invalid{"type = \"bits\"\nbits = 7", "bits"},
        Invalid{"type = \"bits\"\nbits = 9", "bits"},
        Invalid{"type = \"bits\"\nbits = 128", "bits"},
        Invalid{"type = \"bits\"\nbits = true", "bits"},
        Invalid{"type = \"enum\"\nunderlying = \"float32\"\nenumerators = []", "underlying"},
        Invalid{"type = \"enum\"\nunderlying = \"char\"\nenumerators = []", "underlying"},
        Invalid{"type = \"enum\"\nunderlying = \"byte\"\nenumerators = []", "enumerators"},
        Invalid{"type = \"enum\"\nunderlying = \"byte\"\nenumerators = [1]", "enumerators"},
        Invalid{"type = \"enum\"\nunderlying = \"byte\"\nenumerators = [{ name = \"A\" }]", "enumerators[0].value"},
        Invalid{"type = \"enum\"\nunderlying = \"byte\"\nenumerators = [{ name = \"A\", value = 1, extra = 0 }]", "enumerators[0].extra"},
        Invalid{"type = \"enum\"\nunderlying = \"byte\"\nenumerators = [{ name = \"A\", value = 1 }, { name = \"B\", value = \"1\" }]", "enumerators[1].value"},
        Invalid{"type = \"enum\"\nunderlying = \"byte\"\nenumerators = [{ name = \"A\", value = 1 }, { name = \"A\", value = 2 }]", "enumerators[1].name"},
        Invalid{"type = \"enum\"\nunderlying = \"byte\"\nenumerators = [{ name = \"Bad\\u0000Name\", value = 1 }]", "enumerators[0].name"}};
    for (const auto& value : values)
    {
      SCOPED_TRACE(value._declaration);
      expect_creation_failure(one_logical_field(value._declaration), ErrorCode::InvalidConfiguration, value._key);
    }
  }

  TEST_F(FileConfigurationTest, RejectsNoncanonicalEnumNumbersAndUnderlyingOverflowWithIndexedPaths)
  {
    const std::array invalid_unsigned{
        "\"\"", "\"+1\"", "\"01\"", "\"-0\"", "\" 1\"", "\"1 \"", "\"1_0\"",
        "\"0xff\"", "\"1e2\"", "\"1.0\"", "\"-1\"", "-1", "true", "1.0", "[]",
        "\"18446744073709551616\""};
    for (const auto* literal : invalid_unsigned)
    {
      SCOPED_TRACE(literal);
      const auto declaration = std::string{"type = \"enum\"\nunderlying = \"uint64\"\nenumerators = [{ name = \"A\", value = "} + literal + " }]";
      expect_creation_failure(one_logical_field(declaration), ErrorCode::InvalidConfiguration, "enumerators[0].value");
    }
    const std::array<std::pair<const char*, const char*>, 8> overflow{{
        {"byte", "256"}, {"int8", "-129"}, {"int8", "128"}, {"uint16", "65536"},
        {"int16", "32768"}, {"int32", "-2147483649"},
        {"int64", "9223372036854775808"}, {"int64", "-9223372036854775809"}}};
    for (const auto& [base, number] : overflow)
    {
      SCOPED_TRACE(number);
      const auto declaration = std::string{"type = \"enum\"\nunderlying = \""} + base +
          "\"\nenumerators = [{ name = \"A\", value = \"" + number + "\" }]";
      expect_creation_failure(one_logical_field(declaration), ErrorCode::InvalidConfiguration, "enumerators[0].value");
    }
  }

  TEST_F(FileConfigurationTest, AttachmentValidatesLogicalDeclarationsWithoutReplacingRegisteredMeaning)
  {
    const auto valid = logical_settings() + tasks() + logical_fields();
    auto created = require_value(Session::create_from_file(write_configuration(valid), "Setup"));
    const auto side_id = require_value(created._session.registry().find_field("Side"));
    const auto before = require_value(created._session.registry().describe_field(side_id));
    const auto revision = require_value(created._session.registry().metadata(side_id))._revision;
    const auto image = read_bytes(_root / "session" / "session.ang");
    // Attach treats declarations as validation-only input, just as it does for
    // topology. Changing a valid supplied mapping does not edit the Registry.
    const auto different = replace_once(valid, "{ name = \"Sell\", value = 2 }", "{ name = \"Sell\", value = 3 }");
    auto attached = require_value(Session::attach_from_file(write_configuration(different, "attach.toml")));
    EXPECT_EQ(require_value(attached.registry().describe_field(side_id))._logical, before._logical);
    EXPECT_EQ(require_value(attached.registry().metadata(side_id))._revision, revision);
    EXPECT_EQ(read_bytes(_root / "session" / "session.ang"), image);
  }

  TEST_F(FileConfigurationTest, IncludesEnumArraysLabelsAndLogicalAttributesInExactConfigurationBudget)
  {
    const auto valid = logical_settings() + tasks() + logical_fields();
    std::uint64_t required{};
    {
      auto created = require_value(Session::create_from_file(write_configuration(valid), "Setup"));
      required = fs::file_size(_root / "session" / "session.ang");
    }
    fs::remove_all(_root / "session");
    expect_creation_failure(replace_once(valid, "max_log_bytes = 1048576",
        "max_log_bytes = " + std::to_string(required - 1)), ErrorCode::LogFull, "max_log_bytes");
    auto created = require_value(Session::create_from_file(write_configuration(replace_once(valid,
        "max_log_bytes = 1048576", "max_log_bytes = " + std::to_string(required))), "Setup"));
    EXPECT_EQ(fs::file_size(_root / "session" / "session.ang"), required);
    // Labels belong to immutable definition bytes, even with no string pool.
    EXPECT_EQ(require_value(created._session.options())._limits._stringPoolBytes, 0u);
  }

  TEST_F(FileConfigurationTest, BoundsCompleteEnumMetadataEvenWhenItsEntryArrayIsSmall)
  {
    // One entry has a tiny numeric array, but its label alone can exhaust the
    // Session budget. The loader accounts for label bytes and the complete
    // append allocation before producing another owning metadata copy.
    const std::string label(65536, 'L');
    const auto declaration = std::string{"type = \"enum\"\nunderlying = \"byte\"\nenumerators = [{ name = \""} +
        label + "\", value = 1 }]";
    const auto valid = one_logical_field(declaration);
    expect_creation_failure(replace_once(valid, "max_log_bytes = 1048576", "max_log_bytes = 65536"),
                            ErrorCode::LogFull, "max_log_bytes");

    // The same label is valid with adequate log space; this is a resource bound
    // rather than a hidden name-length limit or use of the application pool.
    auto created = require_value(Session::create_from_file(write_configuration(valid), "Setup"));
    const auto id = require_value(created._session.registry().find_field("Logical"));
    const auto field = require_value(created._session.registry().describe_field(id));
    ASSERT_EQ(field._logical._enumerators.size(), 1u);
    EXPECT_EQ(field._logical._enumerators.front()._name, label);
  }
} // namespace
