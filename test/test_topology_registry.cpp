// Public draft-topology examples. A live Task records definitions and a Session
// Registry discovers them without attaching producer/consumer Tasks. Activation,
// queue resources, and endpoints follow step 2.9 and later milestones. Raw edits
// below are deliberate corruption/geometry fixtures, not application APIs.

#include "angaria/runtime.h"
#include "native_atomic.h"
#include "runtime_state.h"
#include "topology_metadata.h"

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
    EXPECT_EQ(error._code, code) << error._path << ": " << error._message;
    EXPECT_FALSE(error._path.empty());
    EXPECT_FALSE(error._message.empty());
  }

  SourceQueueSetDescriptor source(std::string name = "Inputs", TaskId owner = TaskId{1})
  {
    SourceQueueSetDescriptor descriptor;
    descriptor._name = std::move(name);
    descriptor._owner = owner;
    descriptor._doc = "Consumer inputs";
    return descriptor;
  }

  QueueDescriptor queue(std::string name = "Feed", TaskId producer = TaskId{1},
                        SourceQueueSetId inputs = SourceQueueSetId::Invalid,
                        std::uint8_t priority = 1)
  {
    QueueDescriptor descriptor;
    descriptor._name = std::move(name);
    descriptor._producer = producer;
    descriptor._sourceSet = inputs;
    descriptor._priority = priority;
    descriptor._doc = "Producer to consumer connection";
    return descriptor;
  }

  PublishingQueueSetDescriptor publishing(std::string name = "Outputs", TaskId owner = TaskId{1},
                                          std::vector<QueueId> destinations = {})
  {
    PublishingQueueSetDescriptor descriptor;
    descriptor._name = std::move(name);
    descriptor._owner = owner;
    descriptor._destinations = std::move(destinations);
    descriptor._doc = "Publication destinations";
    return descriptor;
  }

  struct TopologyObservation
  {
    mapped::JournalSnapshot _snapshot;
    std::vector<std::uint64_t> _image;
    std::vector<std::string> _claims;

    bool operator==(const TopologyObservation&) const = default;
  };

  // Private fixture builder: journal framing remains valid while tests control
  // relationships in historical definitions. This deliberately bypasses public
  // Registry preflight so domain corruption cannot be mistaken for bad slices,
  // incomplete redo, or an invalid record boundary.
  // Capture Task::claim() before taking RuntimeAccess's gate: claim inspection
  // validates ownership under its own gate and cannot nest inside this fixture.
  class TopologyBatch
  {
  public:
    TopologyBatch(runtime::RuntimeAccess& access, TaskClaim actor) : _access(access), _next(access._snapshot._tail)
    {
      _plan._base = access._snapshot;
      _plan._kind = mapped::JournalKind::Append;
      _plan._actor = actor._task;
      _plan._actorIncarnation = actor._incarnation;
      _plan._newRevision = access._snapshot._revision + 1;
    }

    std::uint64_t add_queue(mapped::QueueDefinition definition, std::uint64_t slot = 0,
                             std::uint64_t previous = 0)
    {
      definition._registration._revision = _plan._newRevision;
      const auto reference = add_record(3, previous,
          require_value(mapped::encode_queue_definition(definition, _next)));
      add_index(mapped::SectionKind::QueueIndex, slot, static_cast<std::uint16_t>(definition._id), reference);
      return reference;
    }

    std::uint64_t add_source(mapped::SourceDefinition definition, std::uint64_t slot = 0,
                              std::uint64_t previous = 0)
    {
      definition._registration._revision = _plan._newRevision;
      const auto reference = add_record(4, previous,
          require_value(mapped::encode_source_definition(definition, _next)));
      add_index(mapped::SectionKind::SourceIndex, slot, static_cast<std::uint16_t>(definition._id), reference);
      add_members(slot, definition._members, 1, _plan._newRevision);
      return reference;
    }

    std::uint64_t add_publishing(mapped::PublishingDefinition definition, std::uint64_t slot = 0,
                                  std::uint64_t previous = 0)
    {
      definition._registration._revision = _plan._newRevision;
      const auto reference = add_record(5, previous,
          require_value(mapped::encode_publishing_definition(definition, _next)));
      add_index(mapped::SectionKind::PublishingIndex, slot, static_cast<std::uint16_t>(definition._id), reference);
      const std::array<std::uint64_t, 2> state{definition._topologyRevision, reference};
      _plan._commands.push_back(runtime::runtime_write_words(
          _access.section_offset(mapped::SectionKind::PublishingState) + slot * mapped::publishing_state_bytes, state));
      add_members(_access._session->_controller->configuration()._limits._sourceSets + slot,
                   definition._descriptor._destinations, 0, definition._topologyRevision);
      return reference;
    }

    void commit()
    {
      require_value(_access._journal.execute(_access._guard, _plan));
    }

  private:
    std::uint64_t add_record(std::uint16_t kind, std::uint64_t previous, std::vector<std::byte> body)
    {
      const auto reference = _next;
      _next += *mapped::checked_align(mapped::record_envelope_bytes + *mapped::checked_align(body.size(), 8), 64);
      mapped::JournalRecord record;
      record._kind = kind;
      record._previous = previous;
      record._immutable = std::move(body);
      _plan._records.push_back(std::move(record));
      return reference;
    }

    void add_index(mapped::SectionKind kind, std::uint64_t slot, std::uint16_t id, std::uint64_t reference)
    {
      const std::array<std::uint64_t, 4> row{id, reference, _plan._newRevision, 1};
      _plan._commands.push_back(runtime::runtime_write_words(
          _access.section_offset(kind) + slot * mapped::index_entry_bytes, row));
    }

    void add_members(std::uint64_t group, const std::vector<QueueId>& members, std::uint8_t priority,
                       std::uint64_t revision)
    {
      const auto capacity = _access._session->_controller->configuration()._limits._membershipsPerSet;
      // An oversized historical descriptor intentionally writes only reserved
      // rows. A later valid revision restores the current mirror; detecting the
      // historical capacity violation is then the domain reader's job.
      for (std::uint64_t member = 0; member < capacity; ++member)
      {
        std::array<std::uint64_t, 2> row{};
        if (member < members.size())
        {
          row = {static_cast<std::uint16_t>(members[member]) | (std::uint64_t{priority} << 16), revision};
        }
        const auto offset = _access.section_offset(mapped::SectionKind::MembershipState) +
            (group * capacity + member) * mapped::membership_bytes;
        _plan._commands.push_back(runtime::runtime_write_words(offset, row));
      }
    }

    runtime::RuntimeAccess& _access;
    mapped::TransactionPlan _plan;
    std::uint64_t _next;
  };

  class TopologyRegistryTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-topology-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
      _directory = _root / "session";
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    static CapacityLimits limits()
    {
      return CapacityLimits::empty().tasks(8).queues(8).source_sets(4)
          .publishing_sets(4).memberships_per_set(4);
    }

    CreatedSession create(CapacityLimits capacities = limits(), std::uint64_t maxBytes = 2097152) const
    {
      SessionOptions options;
      options._name = "Draft topology examples";
      options._directory = _directory;
      options._maxLogBytes = maxBytes;
      options._limits = capacities;
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

    TopologyObservation observe() const
    {
      runtime::RuntimeAccess access(state());
      TopologyObservation result;
      result._snapshot = access._snapshot;
      result._image = access.read_words(0, static_cast<std::size_t>(access._snapshot._fileBytes / 8));
      for (const auto& entry : fs::directory_iterator(_directory / "claims"))
      {
        result._claims.push_back(entry.path().filename().string());
      }
      std::sort(result._claims.begin(), result._claims.end());
      return result;
    }

    std::uint64_t section(mapped::SectionKind kind) const
    {
      runtime::RuntimeAccess access(state());
      return access.section_offset(kind);
    }

    std::pair<std::uint64_t, std::uint64_t> index_row(mapped::SectionKind kind, std::uint64_t key) const
    {
      runtime::RuntimeAccess access(state());
      const auto layout = *mapped::make_fixed_layout(access._session->_controller->configuration()._limits);
      for (const auto& entry : layout._sections)
      {
        if (entry._kind != kind)
        {
          continue;
        }
        const auto rows = access.read_words(entry._offset, static_cast<std::size_t>(entry._byteLength / 8));
        for (std::size_t word = 0; word < rows.size(); word += 8)
        {
          if (rows[word] == key)
          {
            return {entry._offset + word * 8, rows[word + 1]};
          }
        }
      }
      throw std::runtime_error("Fixture could not find registered topology index");
    }

    void set_word(std::uint64_t offset, std::uint64_t value) const
    {
      auto file = require_value(mapped::NativeFile::open_existing(_directory / "session.ang", mapped::FileAccess::ReadWrite));
      auto region = require_value(file.map_read_write(offset, 8));
      require_value(mapped::AtomicWord::bind(require_value(region.writable_bytes()), 0)).store_release(value);
    }

    // Corruption evidence is read directly, independently of Registry decoding.
    std::vector<std::uint64_t> raw_image() const
    {
      auto file = require_value(mapped::NativeFile::open_existing(_directory / "session.ang", mapped::FileAccess::ReadWrite));
      const auto length = require_value(file.size());
      auto region = require_value(file.map_read_write(0, length));
      const auto bytes = require_value(region.writable_bytes());
      std::vector<std::uint64_t> words(static_cast<std::size_t>(length / 8));
      for (std::size_t word = 0; word < words.size(); ++word)
      {
        words[word] = require_value(mapped::AtomicWord::bind(bytes, word * 8)).load_acquire();
      }
      return words;
    }

    fs::path _root;
    fs::path _directory;
  };

  // Registration records a usable definition graph while every entity remains
  // Draft. Only the creator needs a live claim; producer and consumer identities
  // describe offline participants that will attach later.
  TEST_F(TopologyRegistryTest, RegistersAndDiscoversDraftGraphWithOfflineOwners)
  {
    auto created = create();
    auto actor = created._creator.registry();
    TaskDescriptor task;
    task._name = "Consumer";
    const auto consumer = require_value(actor.register_task(task));
    task._name = "Producer";
    const auto producer = require_value(actor.register_task(task));
    const auto inputs = require_value(actor.register_source_set(source("Inputs", consumer)));
    const auto feed = require_value(actor.register_queue(queue("Feed", producer, inputs, 7)));
    const auto outputs = require_value(actor.register_publishing_set(publishing("Outputs", producer, {feed})));
    auto reader = created._session.registry();
    EXPECT_EQ(require_value(reader.find_queue("Feed")), feed);
    EXPECT_EQ(require_value(reader.find_source_set("Inputs")), inputs);
    EXPECT_EQ(require_value(reader.find_publishing_set("Outputs")), outputs);
    EXPECT_EQ(require_value(reader.describe_queue(feed))._producer, producer);
    EXPECT_EQ(require_value(reader.describe_source_set(inputs))._owner, consumer);
    EXPECT_EQ(require_value(reader.describe_publishing_set(outputs))._destinations, std::vector<QueueId>{feed});
    EXPECT_EQ(require_value(reader.source_members(inputs)), std::vector<QueueId>{feed});
    EXPECT_EQ(require_value(reader.state(feed)), EntityState::Draft);
    EXPECT_EQ(require_value(reader.state(inputs)), EntityState::Draft);
    EXPECT_EQ(require_value(reader.state(outputs)), EntityState::Draft);
    EXPECT_FALSE(require_value(reader.task_status(consumer))._attached);
    EXPECT_FALSE(require_value(reader.task_status(producer))._attached);
    EXPECT_EQ(require_value(reader.metadata(feed))._createdBy, TaskId{1});
    // Topology adds definitions and reserved-state rows, never Task claim files.
    EXPECT_EQ(observe()._claims.size(), 3u);
  }

  // Empty/unowned entities support staged programmatic setup. Missing ownership
  // is explicit zero, rather than an implicit assignment to the registrar.
  TEST_F(TopologyRegistryTest, UnownedAndUnassignedDefinitionsRemainExplicitDrafts)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto inputs = require_value(actor.register_source_set(source("UnownedInputs", TaskId::Invalid)));
    const auto feed = require_value(actor.register_queue(queue("Unassigned", TaskId::Invalid)));
    const auto outputs = require_value(actor.register_publishing_set(publishing("UnownedOutputs", TaskId::Invalid)));
    EXPECT_EQ(require_value(actor.describe_queue(feed))._producer, TaskId::Invalid);
    EXPECT_EQ(require_value(actor.describe_queue(feed))._sourceSet, SourceQueueSetId::Invalid);
    EXPECT_EQ(require_value(actor.describe_source_set(inputs))._owner, TaskId::Invalid);
    EXPECT_EQ(require_value(actor.describe_publishing_set(outputs))._owner, TaskId::Invalid);
    EXPECT_TRUE(require_value(actor.source_members(inputs)).empty());
    EXPECT_EQ(require_value(actor.state(feed)), EntityState::Draft);
    EXPECT_EQ(require_value(actor.state(inputs)), EntityState::Draft);
    EXPECT_EQ(require_value(actor.state(outputs)), EntityState::Draft);
    const auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.registry().find_queue("Unassigned")), feed);
  }

  // Inspection returns effective policies. Omitting an override and supplying
  // its resolved value are the same descriptor and do not create another row.
  TEST_F(TopologyRegistryTest, ResolvedDefaultsAndExplicitOverridesRetryWithoutWrites)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto inputs = require_value(actor.register_source_set(source()));
    const auto feed = require_value(actor.register_queue(queue("Feed", TaskId{1}, inputs, 0)));
    const auto outputs = require_value(actor.register_publishing_set(publishing("Outputs", TaskId{1}, {feed})));
    auto explicit_source = source();
    explicit_source._selection = SelectionPolicy::Strict;
    explicit_source._wait = WaitPolicy::Spin;
    auto explicit_queue = queue("Feed", TaskId{1}, inputs, 0);
    explicit_queue._ringCapacity = 1024;
    explicit_queue._delivery = DeliveryPolicy::RecoverFromLog;
    auto explicit_outputs = publishing("Outputs", TaskId{1}, {feed});
    explicit_outputs._acceptance = AcceptancePolicy::AllDestinations;
    const auto before = observe();
    EXPECT_EQ(require_value(actor.register_source_set(explicit_source)), inputs);
    EXPECT_EQ(require_value(actor.register_queue(explicit_queue)), feed);
    EXPECT_EQ(require_value(actor.register_publishing_set(explicit_outputs)), outputs);
    EXPECT_EQ(require_value(actor.register_source_set(require_value(actor.describe_source_set(inputs)))), inputs);
    EXPECT_EQ(require_value(actor.register_queue(require_value(actor.describe_queue(feed)))), feed);
    EXPECT_EQ(require_value(actor.register_publishing_set(require_value(actor.describe_publishing_set(outputs)))), outputs);
    EXPECT_EQ(observe(), before);
    EXPECT_EQ(require_value(actor.describe_queue(feed))._ringCapacity, std::optional<std::uint64_t>{1024});
    EXPECT_EQ(require_value(actor.describe_queue(feed))._delivery, DeliveryPolicy::RecoverFromLog);
    EXPECT_EQ(require_value(actor.describe_source_set(inputs))._selection, SelectionPolicy::Strict);
    EXPECT_EQ(require_value(actor.describe_source_set(inputs))._wait, WaitPolicy::Spin);
    EXPECT_FALSE(require_value(actor.describe_source_set(inputs))._randomSeed);
    EXPECT_EQ(require_value(actor.describe_publishing_set(outputs))._acceptance, AcceptancePolicy::AllDestinations);
  }

  // A queue join appends both definitions in one Registry revision. The Source
  // retains its original creation attribution/time, and earlier immutable bytes
  // remain readable even though the Source index now names a later definition.
  TEST_F(TopologyRegistryTest, QueueJoinCommitsOneRevisionAndRetainsSourceHistory)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto inputs = require_value(actor.register_source_set(source()));
    const auto first_metadata = require_value(actor.metadata(inputs));
    const auto initial_reference = index_row(mapped::SectionKind::SourceIndex, static_cast<std::uint16_t>(inputs)).second;
    std::vector<std::byte> initial_body;
    {
      runtime::RuntimeAccess access(state());
      initial_body = access.immutable_record(initial_reference, 4);
    }
    const auto before = observe();
    const auto first = require_value(actor.register_queue(queue("First", TaskId{1}, inputs, 255)));
    const auto after = observe();
    EXPECT_EQ(after._snapshot._revision, before._snapshot._revision + 1);
    EXPECT_EQ(after._snapshot._generation, before._snapshot._generation + 1);
    const auto metadata = require_value(actor.metadata(inputs));
    EXPECT_EQ(metadata._createdBy, first_metadata._createdBy);
    EXPECT_EQ(metadata._createdAt, first_metadata._createdAt);
    EXPECT_EQ(metadata._revision, require_value(actor.metadata(first))._revision);
    EXPECT_GT(metadata._revision, first_metadata._revision);
    const auto second = require_value(actor.register_queue(queue("Second", TaskId{1}, inputs, 0)));
    EXPECT_EQ(require_value(actor.source_members(inputs)), (std::vector<QueueId>{first, second}));
    const auto latest_reference = index_row(mapped::SectionKind::SourceIndex, static_cast<std::uint16_t>(inputs)).second;
    {
      runtime::RuntimeAccess access(state());
      EXPECT_EQ(access.immutable_record(initial_reference, 4), initial_body);
      const auto preceding = access.record_envelope(latest_reference, 4)._previous;
      EXPECT_NE(preceding, initial_reference);
      EXPECT_EQ(access.record_envelope(preceding, 4)._previous, initial_reference);
    }
    const auto retry_before = observe();
    EXPECT_EQ(require_value(actor.register_queue(queue("First", TaskId{1}, inputs, 255))), first);
    EXPECT_EQ(observe(), retry_before);
    EXPECT_EQ(require_value(actor.source_members(inputs)), (std::vector<QueueId>{first, second}));
  }

  // Queue Set names and IDs occupy one namespace, while typed lookup prevents
  // interpreting a Source as a Publishing set or the reverse. Queue names have
  // their own namespace and may reuse a Queue Set's spelling.
  TEST_F(TopologyRegistryTest, QueueSetKindsShareIdentitiesAndNamesWithoutTypedAliasing)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto inputs = require_value(actor.register_source_set(source("Same")));
    const auto outputs = require_value(actor.register_publishing_set(publishing("Different")));
    EXPECT_NE(static_cast<std::uint16_t>(inputs), static_cast<std::uint16_t>(outputs));
    require_value(actor.register_queue(queue("Same")));
    const auto before = observe();
    expect_error(actor.register_publishing_set(publishing("Same")), ErrorCode::Conflict);
    expect_error(actor.register_source_set(source("Different")), ErrorCode::Conflict);
    expect_error(actor.find_publishing_set("Same"), ErrorCode::NotFound);
    expect_error(actor.find_source_set("Different"), ErrorCode::NotFound);
    expect_error(actor.describe_source_set(static_cast<SourceQueueSetId>(outputs)), ErrorCode::NotFound);
    expect_error(actor.describe_publishing_set(static_cast<PublishingQueueSetId>(inputs)), ErrorCode::NotFound);
    expect_error(actor.state(static_cast<SourceQueueSetId>(outputs)), ErrorCode::NotFound);
    EXPECT_EQ(observe(), before);
  }

  // Publishing references coexist independently: the same producer's queues
  // can occur in several sets, and each set preserves its declared order.
  TEST_F(TopologyRegistryTest, MultiplePublishingSetsPreserveOrderedIndependentReferences)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto inputs = require_value(actor.register_source_set(source()));
    const auto first = require_value(actor.register_queue(queue("First", TaskId{1}, inputs)));
    const auto second = require_value(actor.register_queue(queue("Second", TaskId{1}, inputs)));
    const auto all = require_value(actor.register_publishing_set(publishing("All", TaskId{1}, {second, first})));
    const auto audit = require_value(actor.register_publishing_set(publishing("Audit", TaskId{1}, {first})));
    EXPECT_EQ(require_value(actor.describe_publishing_set(all))._destinations, (std::vector<QueueId>{second, first}));
    EXPECT_EQ(require_value(actor.describe_publishing_set(audit))._destinations, std::vector<QueueId>{first});
    EXPECT_EQ(require_value(actor.source_members(inputs)), (std::vector<QueueId>{first, second}));
    const auto before = observe();
    expect_error(actor.register_publishing_set(publishing("All", TaskId{1}, {first, second})), ErrorCode::Conflict);
    expect_error(actor.register_publishing_set(publishing("Duplicate", TaskId{1}, {first, first})), ErrorCode::InvalidArgument);
    expect_error(actor.register_publishing_set(publishing("Missing", TaskId{1}, {QueueId{65000}})), ErrorCode::NotFound);
    expect_error(actor.register_publishing_set(publishing("Invalid", TaskId{1}, {QueueId::Invalid})), ErrorCode::InvalidArgument);
    EXPECT_EQ(observe(), before);
  }

  // Logical ownership need not equal the actor or require an attached owner.
  // Referenced Tasks must exist, however, and destinations belong to the exact
  // producer named by their Publishing set.
  TEST_F(TopologyRegistryTest, ValidatesTaskSourceAndDestinationOwnershipBeforeWriting)
  {
    auto created = create();
    auto actor = created._creator.registry();
    TaskDescriptor thread;
    thread._name = "ProducerThread";
    thread._kind = TaskKind::Thread;
    thread._process = TaskId{1};
    const auto producer = require_value(actor.register_task(thread));
    const auto inputs = require_value(actor.register_source_set(source("ThreadInputs", producer)));
    const auto feed = require_value(actor.register_queue(queue("ThreadFeed", producer, inputs)));
    const auto outputs = require_value(actor.register_publishing_set(publishing("ThreadOutputs", producer, {feed})));
    const auto before = observe();
    expect_error(actor.register_source_set(source("MissingOwner", TaskId{60000})), ErrorCode::NotFound);
    expect_error(actor.register_queue(queue("MissingProducer", TaskId{60000})), ErrorCode::NotFound);
    expect_error(actor.register_queue(queue("MissingSource", TaskId{1}, SourceQueueSetId{60000})), ErrorCode::NotFound);
    expect_error(actor.register_queue(queue("WrongKind", TaskId{1}, static_cast<SourceQueueSetId>(outputs))), ErrorCode::NotFound);
    expect_error(actor.register_publishing_set(publishing("WrongProducer", TaskId{1}, {feed})), ErrorCode::WrongOwner);
    expect_error(actor.register_publishing_set(publishing("Unowned", TaskId::Invalid, {feed})), ErrorCode::InvalidArgument);
    expect_error(actor.register_publishing_set(publishing("MissingPublisher", TaskId{60000})), ErrorCode::NotFound);
    EXPECT_EQ(observe(), before);
  }

  // Exact registration is not an edit operation. Even Draft descriptor changes
  // conflict; the later adoption/topology APIs provide explicit transitions.
  TEST_F(TopologyRegistryTest, ChangedDescriptorsConflictWithoutChangingDefinitions)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto inputs = require_value(actor.register_source_set(source()));
    const auto feed = require_value(actor.register_queue(queue("Feed", TaskId{1}, inputs)));
    const auto outputs = require_value(actor.register_publishing_set(publishing("Outputs", TaskId{1}, {feed})));
    const auto before = observe();
    auto changed_source = source();
    changed_source._owner = TaskId::Invalid;
    expect_error(actor.register_source_set(changed_source), ErrorCode::Conflict);
    auto changed_queue = queue("Feed", TaskId{1}, inputs);
    changed_queue._priority = 2;
    expect_error(actor.register_queue(changed_queue), ErrorCode::Conflict);
    changed_queue = queue("Feed", TaskId{1}, inputs);
    changed_queue._ringCapacity = 64;
    expect_error(actor.register_queue(changed_queue), ErrorCode::Conflict);
    auto changed_outputs = publishing("Outputs", TaskId{1}, {feed});
    changed_outputs._doc += " changed";
    expect_error(actor.register_publishing_set(changed_outputs), ErrorCode::Conflict);
    EXPECT_EQ(require_value(actor.metadata(inputs))._revision, require_value(actor.metadata(feed))._revision);
    EXPECT_EQ(require_value(actor.state(outputs)), EntityState::Draft);
    EXPECT_EQ(observe(), before);
  }

  // Portable format codes do not promise a live policy implementation. Known
  // future policies return UnsupportedPolicy; malformed enums and geometry are
  // InvalidArgument. Every rejection leaves the Registry and claim files exact.
  TEST_F(TopologyRegistryTest, RejectsUnsupportedPoliciesAndMalformedArgumentsWithoutWrites)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto before = observe();
    auto descriptor = source();
    descriptor._selection = SelectionPolicy::Fair;
    expect_error(actor.register_source_set(descriptor), ErrorCode::UnsupportedPolicy);
    descriptor._selection = SelectionPolicy::WeightedRandom;
    descriptor._randomSeed = 17;
    expect_error(actor.register_source_set(descriptor), ErrorCode::UnsupportedPolicy);
    descriptor = source();
    descriptor._wait = WaitPolicy::EventFd;
    expect_error(actor.register_source_set(descriptor), ErrorCode::UnsupportedPolicy);
    descriptor._wait = static_cast<WaitPolicy>(99);
    expect_error(actor.register_source_set(descriptor), ErrorCode::InvalidArgument);
    descriptor = source();
    descriptor._selection = static_cast<SelectionPolicy>(99);
    expect_error(actor.register_source_set(descriptor), ErrorCode::InvalidArgument);
    descriptor = source();
    descriptor._randomSeed = 0;
    expect_error(actor.register_source_set(descriptor), ErrorCode::InvalidArgument);
    auto feed = queue();
    feed._delivery = DeliveryPolicy::AllowLoss;
    expect_error(actor.register_queue(feed), ErrorCode::UnsupportedPolicy);
    feed._delivery = static_cast<DeliveryPolicy>(99);
    expect_error(actor.register_queue(feed), ErrorCode::InvalidArgument);
    feed = queue();
    feed._ringCapacity = 0;
    expect_error(actor.register_queue(feed), ErrorCode::InvalidArgument);
    feed._ringCapacity = std::numeric_limits<std::uint64_t>::max();
    expect_error(actor.register_queue(feed), ErrorCode::InvalidArgument);
    auto outputs = publishing();
    outputs._acceptance = AcceptancePolicy::Partial;
    expect_error(actor.register_publishing_set(outputs), ErrorCode::UnsupportedPolicy);
    outputs._acceptance = static_cast<AcceptancePolicy>(99);
    expect_error(actor.register_publishing_set(outputs), ErrorCode::InvalidArgument);
    expect_error(actor.describe_queue(QueueId::Invalid), ErrorCode::InvalidArgument);
    expect_error(actor.describe_source_set(SourceQueueSetId::Invalid), ErrorCode::InvalidArgument);
    expect_error(actor.describe_publishing_set(PublishingQueueSetId::Invalid), ErrorCode::InvalidArgument);
    expect_error(actor.source_members(SourceQueueSetId::Invalid), ErrorCode::InvalidArgument);
    expect_error(actor.find_queue(""), ErrorCode::InvalidArgument);
    expect_error(actor.find_source_set(std::string("a\0b", 3)), ErrorCode::InvalidArgument);
    EXPECT_EQ(observe(), before);

    // An omitted selector inherits Strict. A seed is invalid even when this
    // name already exists: invalid effective input must not become Conflict.
    descriptor = source();
    require_value(actor.register_source_set(descriptor));
    const auto registered = observe();
    descriptor._randomSeed = 17;
    expect_error(actor.register_source_set(descriptor), ErrorCode::InvalidArgument);
    descriptor._name = "AnotherInvalidSeed";
    expect_error(actor.register_source_set(descriptor), ErrorCode::InvalidArgument);
    EXPECT_EQ(observe(), registered);
  }

  // Registry names use length-delimited UTF-8 rather than legacy NAMELEN arrays.
  // Documentation may contain NUL; names cannot, and malformed UTF-8 is rejected.
  TEST_F(TopologyRegistryTest, PreservesLongUtf8NamesAndLengthDelimitedDocumentation)
  {
    auto created = create();
    auto actor = created._creator.registry();
    auto inputs = source("Entrées" + std::string(160, 'x'));
    inputs._doc = std::string("consumer\0notes", 14);
    const auto source_id = require_value(actor.register_source_set(inputs));
    auto feed = queue("Données" + std::string(160, 'y'), TaskId{1}, source_id);
    feed._doc = std::string("queue\0notes", 11);
    const auto queue_id = require_value(actor.register_queue(feed));
    auto outputs = publishing("Sorties" + std::string(160, 'z'), TaskId{1}, {queue_id});
    outputs._doc = std::string("publisher\0notes", 15);
    const auto publishing_id = require_value(actor.register_publishing_set(outputs));
    EXPECT_EQ(require_value(actor.describe_source_set(source_id))._doc, inputs._doc);
    EXPECT_EQ(require_value(actor.describe_queue(queue_id))._doc, feed._doc);
    EXPECT_EQ(require_value(actor.describe_publishing_set(publishing_id))._doc, outputs._doc);
    EXPECT_EQ(require_value(actor.find_queue(feed._name)), queue_id);
    const auto before = observe();
    inputs._name = std::string("broken\0name", 11);
    expect_error(actor.register_source_set(inputs), ErrorCode::InvalidArgument);
    feed._name = std::string(1, static_cast<char>(0xff));
    expect_error(actor.register_queue(feed), ErrorCode::InvalidArgument);
    outputs._name = "BadDoc";
    outputs._doc = std::string(1, static_cast<char>(0xff));
    expect_error(actor.register_publishing_set(outputs), ErrorCode::InvalidArgument);
    EXPECT_EQ(observe(), before);
  }

  // Per-kind capacity counts allocation rows; Source and Publishing memberships
  // are independently bounded. A failed join must not create the queue or alter
  // its Source. An existing queue may still appear in another Publishing set.
  TEST_F(TopologyRegistryTest, CapacityAndMembershipFailuresPreserveEntireGraph)
  {
    auto capacities = limits().queues(2).source_sets(1).publishing_sets(2).memberships_per_set(1);
    auto created = create(capacities);
    auto actor = created._creator.registry();
    const auto inputs = require_value(actor.register_source_set(source()));
    const auto first = require_value(actor.register_queue(queue("First", TaskId{1}, inputs)));
    const auto second = require_value(actor.register_queue(queue("Second")));
    const auto outputs = require_value(actor.register_publishing_set(publishing("Outputs", TaskId{1}, {first})));
    const auto before = observe();
    expect_error(actor.register_source_set(source("Overflow")), ErrorCode::CapacityExceeded);
    expect_error(actor.register_queue(queue("Overflow", TaskId{1}, inputs)), ErrorCode::CapacityExceeded);
    expect_error(actor.register_publishing_set(publishing("TooMany", TaskId{1}, {first, second})), ErrorCode::CapacityExceeded);
    EXPECT_EQ(require_value(actor.register_source_set(source())), inputs);
    EXPECT_EQ(require_value(actor.register_queue(queue("First", TaskId{1}, inputs))), first);
    EXPECT_EQ(require_value(actor.register_publishing_set(publishing("Outputs", TaskId{1}, {first}))), outputs);
    EXPECT_EQ(observe(), before);
    require_value(actor.register_publishing_set(publishing("Audit", TaskId{1}, {first})));
    const auto full = observe();
    expect_error(actor.register_publishing_set(publishing("Overflow")), ErrorCode::CapacityExceeded);
    EXPECT_EQ(observe(), full);
  }

  // This isolates membership exhaustion from queue exhaustion: one spare queue
  // row remains, yet adding it to the full Source must be wholly rejected.
  TEST_F(TopologyRegistryTest, FullSourceMembershipDoesNotConsumeSpareQueueIdentity)
  {
    auto created = create(limits().queues(3).memberships_per_set(1));
    auto actor = created._creator.registry();
    const auto inputs = require_value(actor.register_source_set(source()));
    const auto first = require_value(actor.register_queue(queue("First", TaskId{1}, inputs)));
    const auto before = observe();
    expect_error(actor.register_queue(queue("Overflow", TaskId{1}, inputs)), ErrorCode::CapacityExceeded);
    expect_error(actor.find_queue("Overflow"), ErrorCode::NotFound);
    EXPECT_EQ(require_value(actor.source_members(inputs)), std::vector<QueueId>{first});
    EXPECT_EQ(observe(), before);
    const auto second = require_value(actor.register_queue(queue("Unassigned")));
    EXPECT_EQ(static_cast<std::uint16_t>(second), static_cast<std::uint16_t>(first) + 1);
  }

  // Zero capacities disable facilities without disabling inspection of the
  // Session. Zero membership capacity still permits empty Draft sets and an
  // unassigned queue; it cannot admit a relationship requiring a member row.
  TEST_F(TopologyRegistryTest, ZeroCapacitiesDisableEntitiesAndRelationshipsExplicitly)
  {
    {
      auto created = create(CapacityLimits::empty().tasks(1));
      auto actor = created._creator.registry();
      const auto before = observe();
      expect_error(actor.register_source_set(source()), ErrorCode::CapacityExceeded);
      expect_error(actor.register_queue(queue()), ErrorCode::CapacityExceeded);
      expect_error(actor.register_publishing_set(publishing()), ErrorCode::CapacityExceeded);
      expect_error(actor.find_source_set("Inputs"), ErrorCode::NotFound);
      EXPECT_EQ(observe(), before);
    }
    fs::remove_all(_directory);
    auto created = create(limits().memberships_per_set(0));
    auto actor = created._creator.registry();
    const auto inputs = require_value(actor.register_source_set(source()));
    const auto feed = require_value(actor.register_queue(queue()));
    require_value(actor.register_publishing_set(publishing()));
    const auto before = observe();
    expect_error(actor.register_queue(queue("Member", TaskId{1}, inputs)), ErrorCode::CapacityExceeded);
    expect_error(actor.register_publishing_set(publishing("Member", TaskId{1}, {feed})), ErrorCode::CapacityExceeded);
    EXPECT_TRUE(require_value(actor.source_members(inputs)).empty());
    EXPECT_EQ(observe(), before);
  }

  // Measure a complete append instead of mirroring implementation arithmetic.
  // Queue registration includes its new Source revision, both indexes, all
  // membership redo, the manifest, and padding in the exact/T-1 byte boundary.
  TEST_F(TopologyRegistryTest, CompleteRegistrationBudgetsIncludeMultiRecordSourceJoin)
  {
    for (const auto kind : {0, 1, 2})
    {
      SCOPED_TRACE(kind);
      std::uint64_t required{};
      {
        auto created = create();
        auto actor = created._creator.registry();
        const auto inputs = require_value(actor.register_source_set(source()));
        if (kind >= 1)
        {
          const auto feed = require_value(actor.register_queue(queue("Feed", TaskId{1}, inputs)));
          if (kind == 2)
          {
            require_value(actor.register_publishing_set(publishing("Outputs", TaskId{1}, {feed})));
          }
        }
        required = observe()._snapshot._tail;
      }
      fs::remove_all(_directory);
      for (const auto exact : {false, true})
      {
        SCOPED_TRACE(exact);
        {
          auto created = create(limits(), required - (exact ? 0 : 1));
          auto actor = created._creator.registry();
          SourceQueueSetId inputs{};
          QueueId feed{};
          if (kind >= 1)
          {
            inputs = require_value(actor.register_source_set(source()));
          }
          if (kind == 2)
          {
            feed = require_value(actor.register_queue(queue("Feed", TaskId{1}, inputs)));
          }
          const auto before = observe();
          if (kind == 0)
          {
            const auto result = actor.register_source_set(source());
            if (exact)
            {
              inputs = require_value(result);
              EXPECT_EQ(require_value(actor.register_source_set(source())), inputs);
            }
            else
            {
              expect_error(result, ErrorCode::LogFull);
            }
          }
          else if (kind == 1)
          {
            const auto descriptor = queue("Feed", TaskId{1}, inputs);
            const auto result = actor.register_queue(descriptor);
            if (exact)
            {
              feed = require_value(result);
              const auto committed = observe();
              EXPECT_EQ(require_value(actor.register_queue(descriptor)), feed);
              EXPECT_EQ(observe(), committed);
              EXPECT_EQ(require_value(actor.source_members(inputs)), std::vector<QueueId>{feed});
            }
            else
            {
              expect_error(result, ErrorCode::LogFull);
              EXPECT_TRUE(require_value(actor.source_members(inputs)).empty());
            }
          }
          else
          {
            const auto descriptor = publishing("Outputs", TaskId{1}, {feed});
            const auto result = actor.register_publishing_set(descriptor);
            if (exact)
            {
              const auto id = require_value(result);
              const auto committed = observe();
              EXPECT_EQ(require_value(actor.register_publishing_set(descriptor)), id);
              EXPECT_EQ(observe(), committed);
            }
            else
            {
              expect_error(result, ErrorCode::LogFull);
            }
          }
          if (exact)
          {
            EXPECT_EQ(observe()._snapshot._tail, required);
            EXPECT_EQ(observe()._snapshot._accounted, required);
          }
          else
          {
            EXPECT_EQ(observe(), before);
          }
        }
        fs::remove_all(_directory);
      }
    }
  }

  // Exact retries consume no generation, even when a private boundary fixture
  // has exhausted the operation counter. New entities cannot wrap that counter.
  TEST_F(TopologyRegistryTest, ExhaustedGenerationStillAllowsAllExactTopologyRetries)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto inputs = require_value(actor.register_source_set(source()));
    const auto feed_descriptor = queue("Feed", TaskId{1}, inputs);
    const auto feed = require_value(actor.register_queue(feed_descriptor));
    const auto outputs_descriptor = publishing("Outputs", TaskId{1}, {feed});
    const auto outputs = require_value(actor.register_publishing_set(outputs_descriptor));
    const auto generation = observe()._snapshot._generation;
    set_word(120, std::numeric_limits<std::uint64_t>::max());
    const auto before = observe();
    expect_error(actor.register_source_set(source("NewInputs")), ErrorCode::CapacityExceeded);
    expect_error(actor.register_queue(queue("NewFeed")), ErrorCode::CapacityExceeded);
    expect_error(actor.register_publishing_set(publishing("NewOutputs")), ErrorCode::CapacityExceeded);
    EXPECT_EQ(require_value(actor.register_source_set(source())), inputs);
    EXPECT_EQ(require_value(actor.register_queue(feed_descriptor)), feed);
    EXPECT_EQ(require_value(actor.register_publishing_set(outputs_descriptor)), outputs);
    EXPECT_EQ(observe(), before);
    set_word(120, generation);
  }

  // Session Registries inspect but cannot register. A Thread actor registers on
  // its attaching thread, retains its parent claim, and attributes new entities
  // to that logical Thread rather than to a native PID.
  TEST_F(TopologyRegistryTest, RegistryViewsEnforceReadOnlyAndThreadActorOwnership)
  {
    auto created = create();
    auto reader = created._session.registry();
    const auto before = observe();
    expect_error(reader.register_source_set(source()), ErrorCode::ReadOnlyRegistry);
    expect_error(reader.register_queue(queue()), ErrorCode::ReadOnlyRegistry);
    expect_error(reader.register_publishing_set(publishing()), ErrorCode::ReadOnlyRegistry);
    expect_error(Registry{}.register_queue(queue()), ErrorCode::InvalidState);
    EXPECT_EQ(observe(), before);
    TaskDescriptor child;
    child._name = "RegistrarThread";
    child._kind = TaskKind::Thread;
    child._process = TaskId{1};
    const auto id = require_value(created._creator.registry().register_task(child));
    auto task = require_value(created._session.attach_task(id));
    auto actor = task.registry();
    std::optional<Result<QueueId>> result;
    std::thread other([&]
    {
      result.emplace(actor.register_queue(queue()));
    });
    other.join();
    ASSERT_TRUE(result);
    expect_error(*result, ErrorCode::WrongOwner);
    const auto inputs = require_value(actor.register_source_set(source()));
    const auto feed = require_value(actor.register_queue(queue("ThreadRegistered", TaskId{1}, inputs)));
    const auto outputs = require_value(actor.register_publishing_set(publishing("ThreadOutputs", TaskId{1}, {feed})));
    EXPECT_EQ(require_value(reader.metadata(inputs))._createdBy, id);
    EXPECT_EQ(require_value(reader.metadata(feed))._createdBy, id);
    EXPECT_EQ(require_value(reader.metadata(outputs))._createdBy, id);
  }

  // Numeric identities do not choose allocation slots. This private fixture
  // relocates empty Source allocation geometry and gives its retained definition
  // the highest u16 ID before normal public registration resumes.
  TEST_F(TopologyRegistryTest, SparseIdentityAndAllocationSlotsRemainIndependent)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto initial = require_value(actor.register_source_set(source("SparseInputs")));
    const auto [old_row, reference] = index_row(mapped::SectionKind::SourceIndex, static_cast<std::uint16_t>(initial));
    const auto target = section(mapped::SectionKind::SourceIndex) + 3 * mapped::index_entry_bytes;
    const auto original = raw_image();
    for (std::uint64_t word = 0; word < 8; ++word)
    {
      set_word(target + word * 8, original[static_cast<std::size_t>(old_row / 8 + word)]);
      set_word(old_row + word * 8, 0);
    }
    set_word(target, 65535);
    set_word(reference + mapped::record_envelope_bytes, 65535);
    const auto sparse = SourceQueueSetId{65535};
    EXPECT_EQ(require_value(actor.find_source_set("SparseInputs")), sparse);
    const auto outputs = require_value(actor.register_publishing_set(publishing()));
    EXPECT_NE(static_cast<std::uint16_t>(outputs), 65535);
    const auto feed = require_value(actor.register_queue(queue("SparseFeed", TaskId{1}, sparse)));
    EXPECT_EQ(require_value(actor.source_members(sparse)), std::vector<QueueId>{feed});
    EXPECT_EQ(require_value(actor.describe_queue(feed))._sourceSet, sparse);
    const auto next = require_value(actor.register_source_set(source("NewInputs")));
    EXPECT_NE(next, sparse);
    EXPECT_NE(static_cast<std::uint16_t>(next), static_cast<std::uint16_t>(outputs));
    EXPECT_EQ(require_value(actor.find_source_set("SparseInputs")), sparse);
  }

  // Topology history cannot silently disappear behind a zeroed current row.
  // Even a sole empty set/unassigned queue has a committed retained definition
  // that attachment and held Registry views must continue to account for.
  TEST_F(TopologyRegistryTest, MissingCurrentIndexesRejectWithoutReusingRetainedIdentities)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto inputs = require_value(actor.register_source_set(source()));
    const auto feed = require_value(actor.register_queue(queue()));
    const auto outputs = require_value(actor.register_publishing_set(publishing()));
    const std::array<std::pair<mapped::SectionKind, std::uint64_t>, 3> targets{{
        {mapped::SectionKind::SourceIndex, static_cast<std::uint16_t>(inputs)},
        {mapped::SectionKind::QueueIndex, static_cast<std::uint16_t>(feed)},
        {mapped::SectionKind::PublishingIndex, static_cast<std::uint16_t>(outputs)}}};
    for (const auto& [kind, key] : targets)
    {
      SCOPED_TRACE(static_cast<unsigned>(kind));
      const auto row = index_row(kind, key).first;
      const auto original = raw_image();
      for (std::uint64_t word = 0; word < 8; ++word)
      {
        set_word(row + word * 8, 0);
      }
      // Publishing rows also contain a metadata-only runtime mirror. Clear it
      // with the index so retained-history coverage, rather than leftover live
      // state alone, must expose this otherwise apparently unused slot.
      const auto publishing_state = section(mapped::SectionKind::PublishingState);
      if (kind == mapped::SectionKind::PublishingIndex)
      {
        for (std::uint64_t word = 0; word < mapped::publishing_state_bytes / 8; ++word)
        {
          set_word(publishing_state + word * 8, 0);
        }
      }
      const auto evidence = raw_image();
      expect_error(Session::attach(_directory), ErrorCode::IoError);
      expect_error(actor.find_source_set("Inputs"), ErrorCode::IoError);
      expect_error(actor.find_queue("Feed"), ErrorCode::IoError);
      expect_error(actor.find_publishing_set("Outputs"), ErrorCode::IoError);
      expect_error(actor.register_source_set(source()), ErrorCode::IoError);
      expect_error(actor.register_queue(queue()), ErrorCode::IoError);
      expect_error(actor.register_publishing_set(publishing()), ErrorCode::IoError);
      EXPECT_EQ(raw_image(), evidence);
      for (std::uint64_t word = 0; word < 8; ++word)
      {
        set_word(row + word * 8, original[static_cast<std::size_t>(row / 8 + word)]);
      }
      if (kind == mapped::SectionKind::PublishingIndex)
      {
        for (std::uint64_t word = 0; word < mapped::publishing_state_bytes / 8; ++word)
        {
          const auto offset = publishing_state + word * 8;
          set_word(offset, original[static_cast<std::size_t>(offset / 8)]);
        }
      }
      EXPECT_EQ(raw_image(), original);
    }
  }

  // Source predecessor links, duplicate identities, membership mirrors, and
  // reserved live state are persisted evidence, not data to normalize on attach.
  // Each independent corruption must fail before any repair or registration.
  TEST_F(TopologyRegistryTest, CorruptChainsMembershipAndDraftStateRejectWithoutWrites)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto inputs = require_value(actor.register_source_set(source()));
    const auto first = require_value(actor.register_queue(queue("First", TaskId{1}, inputs, 7)));
    const auto second = require_value(actor.register_queue(queue("Second", TaskId{1}, inputs, 9)));
    const auto outputs = require_value(actor.register_publishing_set(publishing("Outputs", TaskId{1}, {first, second})));
    const auto source_reference = index_row(mapped::SectionKind::SourceIndex, static_cast<std::uint16_t>(inputs)).second;
    const auto queue_reference = index_row(mapped::SectionKind::QueueIndex, static_cast<std::uint16_t>(first)).second;
    const auto publishing_row = index_row(mapped::SectionKind::PublishingIndex, static_cast<std::uint16_t>(outputs)).first;
    const auto publishing_membership = section(mapped::SectionKind::MembershipState) +
        std::uint64_t{limits()._sourceSets} * limits()._membershipsPerSet * mapped::membership_bytes;
    const std::array<std::uint64_t, 13> targets{
        source_reference + 32,
        queue_reference + mapped::record_envelope_bytes,
        publishing_row,
        section(mapped::SectionKind::MembershipState),
        section(mapped::SectionKind::MembershipState) + 8,
        section(mapped::SectionKind::MembershipState) + 16,
        section(mapped::SectionKind::MembershipState) + 24,
        publishing_membership,
        publishing_membership + 8,
        section(mapped::SectionKind::QueueState),
        section(mapped::SectionKind::SourceState),
        section(mapped::SectionKind::PublishingState),
        section(mapped::SectionKind::PublishingState) + 120};
    for (const auto target : targets)
    {
      SCOPED_TRACE(target);
      const auto original = raw_image();
      const auto old = original[static_cast<std::size_t>(target / 8)];
      set_word(target, old ^ 1);
      const auto evidence = raw_image();
      expect_error(Session::attach(_directory), ErrorCode::IoError);
      expect_error(actor.source_members(inputs), ErrorCode::IoError);
      expect_error(actor.register_queue(queue("New")), ErrorCode::IoError);
      EXPECT_EQ(raw_image(), evidence);
      set_word(target, old);
    }
    EXPECT_EQ(require_value(actor.source_members(inputs)), (std::vector<QueueId>{first, second}));
  }

  // A later correct Source revision cannot repair the meaning of an earlier
  // committed Registry revision. Here the Queue appears first without an atomic
  // inverse Source update; the next append makes only the latest graph valid.
  TEST_F(TopologyRegistryTest, LaterSourceRevisionCannotHideHistoricalQueueAssignmentGap)
  {
    auto created = create();
    const auto claim = created._creator.claim();
    auto actor = created._creator.registry();
    const auto inputs = require_value(actor.register_source_set(source()));
    const auto initial_metadata = require_value(actor.metadata(inputs));
    const auto initial_reference = index_row(mapped::SectionKind::SourceIndex, static_cast<std::uint16_t>(inputs)).second;
    mapped::QueueDefinition queue_definition;
    queue_definition._id = QueueId{1};
    queue_definition._descriptor = queue("Feed", TaskId{1}, inputs);
    queue_definition._descriptor._ringCapacity = 1024;
    queue_definition._descriptor._delivery = DeliveryPolicy::RecoverFromLog;
    queue_definition._registration = initial_metadata;
    {
      runtime::RuntimeAccess access(state());
      TopologyBatch batch(access, claim);
      batch.add_queue(queue_definition);
      batch.commit();
    }
    mapped::SourceDefinition updated;
    updated._id = inputs;
    updated._descriptor = source();
    updated._descriptor._selection = SelectionPolicy::Strict;
    updated._descriptor._wait = WaitPolicy::Spin;
    updated._registration = initial_metadata;
    updated._members = {QueueId{1}};
    {
      runtime::RuntimeAccess access(state());
      TopologyBatch batch(access, claim);
      batch.add_source(updated, 0, initial_reference);
      batch.commit();
    }
    const auto evidence = raw_image();
    expect_error(Session::attach(_directory), ErrorCode::IoError);
    expect_error(actor.source_members(inputs), ErrorCode::IoError);
    expect_error(actor.describe_queue(QueueId{1}), ErrorCode::IoError);
    expect_error(actor.register_queue(queue("New")), ErrorCode::IoError);
    EXPECT_EQ(raw_image(), evidence);
  }

  // Reserved membership capacity binds every retained revision, not just the
  // currently indexed definition. Private journal batches create an oversized
  // older list and a smaller valid current graph, preserving all other chains,
  // ownership, relationships, and state mirrors.
  TEST_F(TopologyRegistryTest, ValidLatestGraphCannotHideOversizedHistoricalMemberships)
  {
    for (const auto publishing_kind : {false, true})
    {
      SCOPED_TRACE(publishing_kind);
      {
        auto created = create(limits().memberships_per_set(1));
        const auto claim = created._creator.claim();
        const auto time = require_value(created._session.metadata())._createdAt;
        const RegistrationMetadata metadata{TaskId{1}, time, 1};
        mapped::QueueDefinition first;
        first._id = QueueId{1};
        first._descriptor = queue("First", TaskId{1}, publishing_kind ? SourceQueueSetId::Invalid : SourceQueueSetId{1});
        first._descriptor._ringCapacity = 1024;
        first._descriptor._delivery = DeliveryPolicy::RecoverFromLog;
        first._registration = metadata;
        auto second = first;
        second._id = QueueId{2};
        second._descriptor._name = "Second";
        mapped::SourceDefinition inputs;
        inputs._id = SourceQueueSetId{1};
        inputs._descriptor = source();
        inputs._descriptor._selection = SelectionPolicy::Strict;
        inputs._descriptor._wait = WaitPolicy::Spin;
        inputs._registration = metadata;
        inputs._members = {QueueId{1}, QueueId{2}};
        mapped::PublishingDefinition outputs;
        outputs._id = PublishingQueueSetId{1};
        outputs._descriptor = publishing("Outputs", TaskId{1}, {QueueId{1}, QueueId{2}});
        outputs._descriptor._acceptance = AcceptancePolicy::AllDestinations;
        outputs._registration = metadata;
        outputs._topologyRevision = 1;
        std::uint64_t second_reference{};
        std::uint64_t set_reference{};
        {
          runtime::RuntimeAccess access(state());
          TopologyBatch batch(access, claim);
          batch.add_queue(first);
          second_reference = batch.add_queue(second, 1);
          set_reference = publishing_kind ? batch.add_publishing(outputs) : batch.add_source(inputs);
          batch.commit();
        }
        {
          runtime::RuntimeAccess access(state());
          TopologyBatch batch(access, claim);
          if (publishing_kind)
          {
            outputs._descriptor._destinations = {QueueId{1}};
            outputs._topologyRevision = 2;
            batch.add_publishing(outputs, 0, set_reference);
          }
          else
          {
            // The detached second Queue keeps the latest inverse graph valid.
            second._descriptor._sourceSet = SourceQueueSetId::Invalid;
            batch.add_queue(second, 1, second_reference);
            inputs._members = {QueueId{1}};
            batch.add_source(inputs, 0, set_reference);
          }
          batch.commit();
        }
        const auto evidence = raw_image();
        auto reader = created._session.registry();
        expect_error(Session::attach(_directory), ErrorCode::IoError);
        expect_error(reader.describe_queue(QueueId{1}), ErrorCode::IoError);
        expect_error(created._creator.registry().register_queue(queue("New")), ErrorCode::IoError);
        EXPECT_EQ(raw_image(), evidence);
      }
      fs::remove_all(_directory);
    }
  }

  // Atomic batch order does not decide whether a relationship existed at its
  // Registry revision. A Source may precede or follow its Queue physically; the
  // complete same-revision pair must be accepted in either order.
  TEST_F(TopologyRegistryTest, SameRevisionSourceAndQueueRelationshipsAllowEitherRecordOrder)
  {
    for (const auto source_first : {false, true})
    {
      SCOPED_TRACE(source_first);
      {
        auto created = create();
        const auto claim = created._creator.claim();
        const RegistrationMetadata metadata{TaskId{1}, require_value(created._session.metadata())._createdAt, 1};
        mapped::QueueDefinition feed;
        feed._id = QueueId{1};
        feed._descriptor = queue("Feed", TaskId{1}, SourceQueueSetId{1});
        feed._descriptor._ringCapacity = 1024;
        feed._descriptor._delivery = DeliveryPolicy::RecoverFromLog;
        feed._registration = metadata;
        mapped::SourceDefinition inputs;
        inputs._id = SourceQueueSetId{1};
        inputs._descriptor = source();
        inputs._descriptor._selection = SelectionPolicy::Strict;
        inputs._descriptor._wait = WaitPolicy::Spin;
        inputs._registration = metadata;
        inputs._members = {QueueId{1}};
        {
          runtime::RuntimeAccess access(state());
          TopologyBatch batch(access, claim);
          if (source_first)
          {
            batch.add_source(inputs);
            batch.add_queue(feed);
          }
          else
          {
            batch.add_queue(feed);
            batch.add_source(inputs);
          }
          batch.commit();
        }
        auto attached = require_value(Session::attach(_directory));
        EXPECT_EQ(require_value(attached.registry().find_queue("Feed")), QueueId{1});
        EXPECT_EQ(require_value(attached.registry().source_members(SourceQueueSetId{1})), std::vector<QueueId>{QueueId{1}});
        EXPECT_EQ(require_value(attached.registry().metadata(QueueId{1}))._revision,
                  require_value(attached.registry().metadata(SourceQueueSetId{1}))._revision);
      }
      fs::remove_all(_directory);
    }
  }

  // A full log must not pin a Task claim forever merely because Draft topology
  // exists. Closing/replacing the registrar performs fixed-state lifetime work,
  // preserves definitions and memberships, and issues in its new incarnation.
  TEST_F(TopologyRegistryTest, DraftTopologyAllowsTaskReplacementAtExactLogLimit)
  {
    std::uint64_t required{};
    {
      auto created = create();
      auto actor = created._creator.registry();
      const auto inputs = require_value(actor.register_source_set(source()));
      const auto feed = require_value(actor.register_queue(queue("Feed", TaskId{1}, inputs)));
      require_value(actor.register_publishing_set(publishing("Outputs", TaskId{1}, {feed})));
      required = observe()._snapshot._tail;
    }
    fs::remove_all(_directory);
    auto created = create(limits(), required);
    auto session = std::move(created._session);
    SourceQueueSetId inputs{};
    QueueId feed{};
    PublishingQueueSetId outputs{};
    RegistrationMetadata metadata;
    {
      auto actor = created._creator.registry();
      inputs = require_value(actor.register_source_set(source()));
      feed = require_value(actor.register_queue(queue("Feed", TaskId{1}, inputs)));
      outputs = require_value(actor.register_publishing_set(publishing("Outputs", TaskId{1}, {feed})));
      metadata = require_value(actor.metadata(feed));
    }
    const auto before = observe();
    EXPECT_EQ(before._snapshot._tail, required);
    {
      auto closing = std::move(created._creator);
    }
    EXPECT_FALSE(require_value(session.registry().task_status(TaskId{1}))._attached);
    auto replacement = require_value(session.attach_task(TaskId{1}));
    EXPECT_EQ(replacement.claim()._incarnation, 2u);
    EXPECT_EQ(require_value(replacement.next_publication_id())._sequence, 1u);
    EXPECT_EQ(observe()._snapshot._tail, before._snapshot._tail);
    EXPECT_EQ(observe()._snapshot._accounted, before._snapshot._accounted);
    EXPECT_EQ(require_value(session.registry().metadata(feed))._createdAt, metadata._createdAt);
    EXPECT_EQ(require_value(session.registry().metadata(feed))._revision, metadata._revision);
    EXPECT_EQ(require_value(session.registry().source_members(inputs)), std::vector<QueueId>{feed});
    EXPECT_EQ(require_value(session.registry().describe_publishing_set(outputs))._destinations, std::vector<QueueId>{feed});
  }

  // Draft metadata does not authorize discarding delivery progress. Deliberately
  // inject Queue/Source progress after detachment, then prove a failed successor
  // attachment leaves the complete image and incarnation unchanged. Restoring
  // the evidence permits the same logical Task to attach normally afterward.
  TEST_F(TopologyRegistryTest, TaskReplacementRejectsDraftDeliveryProgressWithoutWrites)
  {
    auto created = create();
    auto session = std::move(created._session);
    {
      auto actor = created._creator.registry();
      const auto inputs = require_value(actor.register_source_set(source()));
      const auto feed = require_value(actor.register_queue(queue("Feed", TaskId{1}, inputs)));
      require_value(actor.register_publishing_set(publishing("Outputs", TaskId{1}, {feed})));
    }
    {
      auto closing = std::move(created._creator);
    }
    for (const auto kind : {mapped::SectionKind::QueueState, mapped::SectionKind::SourceState})
    {
      SCOPED_TRACE(static_cast<unsigned>(kind));
      const auto previous = require_value(session.registry().task_status(TaskId{1}))._incarnation;
      const auto offset = section(kind);
      set_word(offset, 1);
      const auto evidence = raw_image();
      expect_error(session.attach_task(TaskId{1}), ErrorCode::IoError);
      EXPECT_EQ(raw_image(), evidence);
      set_word(offset, 0);
      auto replacement = require_value(session.attach_task(TaskId{1}));
      EXPECT_EQ(replacement.claim()._incarnation, previous + 1);
    }
  }
} // namespace
