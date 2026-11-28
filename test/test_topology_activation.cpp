// Public topology completion and activation examples. A live registrar may finish
// offline owners' Draft definitions, then activate Source, Queue, and Publishing
// entities in dependency order. These tests inspect identities/resources, not
// message publication or delivery. Private byte edits and journal batches below
// are corruption/exhaustion fixtures, never application initialization APIs.

#include "native_fixture.h"
#include "result_assertions.h"
#include "angaria/runtime.h"
#include "native_atomic.h"
#include "topology_state.h"
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

  using angaria::test_support::require_value;

  using angaria::test_support::expect_error;

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

  struct ActivationObservation
  {
    mapped::JournalSnapshot _snapshot;
    std::vector<std::uint64_t> _image;
    std::vector<std::string> _claims;

    bool operator==(const ActivationObservation&) const = default;
  };

  struct ActivationGraph
  {
    SourceQueueSetId _source{};
    QueueId _queue{};
    PublishingQueueSetId _publishing{};
  };

  // Private atomic-batch fixture. Public mutations emit one final definition
  // per entity, while the format can retain intermediate same-revision records.
  // Preserve their frames/predecessors and install the final runtime mirrors so
  // these tests isolate committed transition semantics from byte corruption.
  // The caller captures Task::claim() before taking RuntimeAccess's gate.
  class TransitionBatch
  {
  public:
    TransitionBatch(runtime::RuntimeAccess& access, TaskClaim claim) : _access(access), _next(access._snapshot._tail)
    {
      _plan._base = access._snapshot;
      _plan._kind = mapped::JournalKind::Append;
      _plan._actor = claim._task;
      _plan._actorIncarnation = claim._incarnation;
      _plan._newRevision = access._snapshot._revision + 1;
    }

    std::uint64_t add_queue(mapped::QueueDefinition definition, const runtime::LoadedQueue& target,
                             std::uint64_t previous)
    {
      definition._registration._revision = _plan._newRevision;
      const auto reference = add_record(3, previous,
          require_value(mapped::encode_queue_definition(definition, _next)));
      add_index(target._indexOffset, static_cast<std::uint16_t>(definition._id), reference, definition._state);
      return reference;
    }

    std::uint64_t add_source(mapped::SourceDefinition definition, const runtime::LoadedSource& target,
                              std::uint64_t previous)
    {
      definition._registration._revision = _plan._newRevision;
      const auto reference = add_record(4, previous,
          require_value(mapped::encode_source_definition(definition, _next)));
      add_index(target._indexOffset, static_cast<std::uint16_t>(definition._id), reference, definition._state);
      add_members(target._slot, definition._members, 1, _plan._newRevision);
      return reference;
    }

    std::uint64_t add_publishing(mapped::PublishingDefinition definition, const runtime::LoadedPublishing& target,
                                  std::uint64_t previous)
    {
      definition._registration._revision = _plan._newRevision;
      const auto reference = add_record(5, previous,
          require_value(mapped::encode_publishing_definition(definition, _next)));
      add_index(target._indexOffset, static_cast<std::uint16_t>(definition._id), reference, definition._state);
      const std::array<std::uint64_t, 2> state{definition._topologyRevision, reference};
      _plan._commands.push_back(runtime::runtime_write_words(target._stateOffset, state));
      const auto group = _access._session->_controller->configuration()._limits._sourceSets + target._slot;
      add_members(group, definition._descriptor._destinations, 0, definition._topologyRevision);
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

    void add_index(std::uint64_t offset, std::uint16_t id, std::uint64_t reference, EntityState state)
    {
      const std::array<std::uint64_t, 4> row{id, reference, _plan._newRevision,
          state == EntityState::Active ? 2u : 1u};
      _plan._commands.push_back(runtime::runtime_write_words(offset, row));
    }

    void add_members(std::uint64_t group, const std::vector<QueueId>& members,
                       std::uint8_t priority, std::uint64_t revision)
    {
      const auto capacity = _access._session->_controller->configuration()._limits._membershipsPerSet;
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

  class TopologyActivationTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-activation-XXXXXX").string();
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

    CreatedSession create(std::uint64_t maxBytes = 2097152, CapacityLimits capacity = limits()) const
    {
      SessionOptions options;
      options._name = "Topology activation examples";
      options._directory = _directory;
      options._maxLogBytes = maxBytes;
      options._limits = capacity;
      // Small explicit rings make resource accounting visible without a large
      // message-storage fixture. Production defaults are unchanged.
      options._defaults._ringCapacity = 4;
      TaskDescriptor creator;
      creator._name = "Creator";
      return require_value(Session::create(options, creator));
    }

    static TaskId register_task(Registry actor, const std::string& name)
    {
      TaskDescriptor descriptor;
      descriptor._name = name;
      return require_value(actor.register_task(descriptor));
    }

    static ActivationGraph graph(Registry actor, TaskId producer = TaskId{1}, TaskId consumer = TaskId{1})
    {
      ActivationGraph value;
      value._source = require_value(actor.register_source_set(source("Inputs", consumer)));
      value._queue = require_value(actor.register_queue(queue("Feed", producer, value._source)));
      value._publishing = require_value(actor.register_publishing_set(publishing("Outputs", producer, {value._queue})));
      return value;
    }

    static void activate(Registry actor, const ActivationGraph& value)
    {
      require_value(actor.activate_source_set(value._source));
      require_value(actor.activate_queue(value._queue));
      require_value(actor.activate_publishing_set(value._publishing));
    }

    static OperationResult activate_kind(Registry actor, const ActivationGraph& value, unsigned kind)
    {
      if (kind == 0)
      {
        return actor.activate_source_set(value._source);
      }
      if (kind == 1)
      {
        return actor.activate_queue(value._queue);
      }
      return actor.activate_publishing_set(value._publishing);
    }

    static void prepare_kind(Registry actor, const ActivationGraph& value, unsigned kind)
    {
      if (kind >= 1)
      {
        require_value(actor.activate_source_set(value._source));
      }
      if (kind >= 2)
      {
        require_value(actor.activate_queue(value._queue));
      }
    }

    std::shared_ptr<runtime::SessionState> state() const
    {
      auto result = std::make_shared<runtime::SessionState>();
      result->_directory = fs::canonical(_directory);
      result->_controller = require_value(mapped::SessionController::open_existing(_directory));
      return result;
    }

    ActivationObservation observe() const
    {
      runtime::RuntimeAccess access(state());
      ActivationObservation result;
      result._snapshot = access._snapshot;
      result._image = access.read_words(0, static_cast<std::size_t>(access._snapshot._fileBytes / 8));
      for (const auto& entry : fs::directory_iterator(_directory / "claims"))
      {
        result._claims.push_back(entry.path().filename().string());
      }
      std::sort(result._claims.begin(), result._claims.end());
      return result;
    }

    std::vector<std::uint64_t> raw_image() const
    {
      auto file = require_value(mapped::NativeFile::open_existing(_directory / "session.ang", mapped::FileAccess::ReadWrite));
      const auto size = require_value(file.size());
      auto region = require_value(file.map_read_write(0, size));
      const auto bytes = require_value(region.writable_bytes());
      std::vector<std::uint64_t> words(static_cast<std::size_t>(size / 8));
      for (std::size_t position = 0; position < words.size(); ++position)
      {
        words[position] = require_value(mapped::AtomicWord::bind(bytes, position * 8)).load_acquire();
      }
      return words;
    }

    void set_word(std::uint64_t offset, std::uint64_t value) const
    {
      angaria::test_support::set_atomic_word(_directory, offset, value);
    }

    runtime::LoadedQueue loaded_queue(QueueId id) const
    {
      runtime::RuntimeAccess access(state());
      const auto topology = runtime::runtime_topology(access);
      const auto found = std::find_if(topology._queues.begin(), topology._queues.end(), [id](const auto& entry)
      {
        return entry._definition._id == id;
      });
      if (found == topology._queues.end())
      {
        throw std::runtime_error("Activation fixture Queue is absent");
      }
      return *found;
    }

    runtime::LoadedSource loaded_source(SourceQueueSetId id) const
    {
      runtime::RuntimeAccess access(state());
      const auto topology = runtime::runtime_topology(access);
      const auto found = std::find_if(topology._sources.begin(), topology._sources.end(), [id](const auto& entry)
      {
        return entry._definition._id == id;
      });
      if (found == topology._sources.end())
      {
        throw std::runtime_error("Activation fixture Source is absent");
      }
      return *found;
    }

    runtime::LoadedPublishing loaded_publishing(PublishingQueueSetId id) const
    {
      runtime::RuntimeAccess access(state());
      const auto topology = runtime::runtime_topology(access);
      const auto found = std::find_if(topology._publishing.begin(), topology._publishing.end(), [id](const auto& entry)
      {
        return entry._definition._id == id;
      });
      if (found == topology._publishing.end())
      {
        throw std::runtime_error("Activation fixture Publishing set is absent");
      }
      return *found;
    }

    std::vector<std::byte> definition_bytes(std::uint64_t reference, std::uint16_t kind) const
    {
      runtime::RuntimeAccess access(state());
      return access.immutable_record(reference, kind);
    }

    std::size_t ring_count() const
    {
      runtime::RuntimeAccess access(state());
      return access.record_references(8).size();
    }

    std::vector<std::uint64_t> queue_state(const runtime::LoadedQueue& value) const
    {
      runtime::RuntimeAccess access(state());
      return access.read_words(value._stateOffset, mapped::queue_state_bytes / 8);
    }

    // Private journal fixture: preserve byte framing while creating otherwise
    // forbidden historical owner changes. The caller captures claim before the
    // gate, and each later append supplies the prior fixture's returned reference.
    std::uint64_t append_source(mapped::SourceDefinition definition, TaskClaim claim,
                                  std::uint64_t index, std::uint64_t slot, std::uint64_t previous) const
    {
      runtime::RuntimeAccess access(state());
      mapped::TransactionPlan plan;
      plan._base = access._snapshot;
      plan._kind = mapped::JournalKind::Append;
      plan._actor = claim._task;
      plan._actorIncarnation = claim._incarnation;
      plan._newRevision = access._snapshot._revision + 1;
      definition._registration._revision = plan._newRevision;
      mapped::JournalRecord record;
      record._kind = 4;
      record._previous = previous;
      record._immutable = require_value(mapped::encode_source_definition(definition, access._snapshot._tail));
      plan._records.push_back(std::move(record));
      const std::array<std::uint64_t, 4> row{
          static_cast<std::uint16_t>(definition._id), access._snapshot._tail, plan._newRevision,
          definition._state == EntityState::Active ? 2u : 1u};
      plan._commands.push_back(runtime::runtime_write_words(index, row));
      const auto capacity = access._session->_controller->configuration()._limits._membershipsPerSet;
      for (std::uint64_t member = 0; member < capacity; ++member)
      {
        std::array<std::uint64_t, 2> membership{};
        if (member < definition._members.size())
        {
          membership = {static_cast<std::uint16_t>(definition._members[member]) | (std::uint64_t{1} << 16), plan._newRevision};
        }
        const auto offset = access.section_offset(mapped::SectionKind::MembershipState) +
            (slot * capacity + member) * mapped::membership_bytes;
        plan._commands.push_back(runtime::runtime_write_words(offset, membership));
      }
      require_value(access._journal.execute(access._guard, plan));
      return plan._base._tail;
    }

    void check_publishing_batch_transition(bool changed, std::uint64_t finalCounter)
    {
      {
        auto created = create();
        const auto claim = created._creator.claim();
        auto actor = created._creator.registry();
        const auto value = graph(actor);
        const auto second = require_value(actor.register_queue(queue("Second", TaskId{1}, value._source)));
        activate(actor, value);
        require_value(actor.activate_queue(second));
        const auto original = loaded_publishing(value._publishing);
        const auto before = observe();
        auto intermediate = original._definition;
        intermediate._descriptor._destinations = changed ? std::vector<QueueId>{second, value._queue} :
                                                           std::vector<QueueId>{second};
        intermediate._topologyRevision = 2;
        auto final = original._definition;
        final._descriptor._destinations = changed ? std::vector<QueueId>{second} : std::vector<QueueId>{value._queue};
        final._topologyRevision = finalCounter;
        {
          runtime::RuntimeAccess access(state());
          TransitionBatch batch(access, claim);
          const auto previous = batch.add_publishing(intermediate, original, original._reference);
          batch.add_publishing(final, original, previous);
          batch.commit();
        }
        const auto evidence = observe();
        if (finalCounter == (changed ? 2u : 1u))
        {
          auto attached = require_value(Session::attach(_directory));
          EXPECT_EQ(require_value(attached.registry().describe_publishing_set(value._publishing))._destinations,
                    final._descriptor._destinations);
          EXPECT_EQ(loaded_publishing(value._publishing)._definition._topologyRevision, finalCounter);
          const auto metadata = require_value(attached.registry().metadata(value._publishing));
          EXPECT_EQ(metadata._revision, before._snapshot._revision + 1);
          EXPECT_EQ(metadata._createdBy, original._definition._registration._createdBy);
          EXPECT_EQ(metadata._createdAt, original._definition._registration._createdAt);
          require_value(actor.set_destinations(value._publishing, final._descriptor._destinations));
          EXPECT_EQ(observe(), evidence);
        }
        else
        {
          expect_error(Session::attach(_directory), ErrorCode::IoError);
          expect_error(actor.find_publishing_set("Outputs"), ErrorCode::IoError);
          expect_error(actor.describe_publishing_set(value._publishing), ErrorCode::IoError);
          expect_error(actor.set_destinations(value._publishing, final._descriptor._destinations), ErrorCode::IoError);
          EXPECT_EQ(raw_image(), evidence._image);
        }
      }
      fs::remove_all(_directory);
    }

    fs::path _root;
    fs::path _directory;
  };

  // Incomplete Drafts are deliberate staged setup. The registrar may assign
  // offline registered owners and later revise those owners before activation.
  TEST_F(TopologyActivationTest, DraftAdoptionCompletesOfflineOwnershipAndPreservesCreationMetadata)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto producer = register_task(actor, "Producer");
    const auto consumer = register_task(actor, "Consumer");
    const auto inputs = require_value(actor.register_source_set(source("Inputs", TaskId::Invalid)));
    const auto feed = require_value(actor.register_queue(queue("Feed", TaskId::Invalid)));
    const auto outputs = require_value(actor.register_publishing_set(publishing("Outputs", TaskId::Invalid)));
    const auto source_metadata = require_value(actor.metadata(inputs));
    const auto queue_metadata = require_value(actor.metadata(feed));
    const auto publishing_metadata = require_value(actor.metadata(outputs));
    const auto before = observe();
    require_value(actor.adopt_queue(feed, producer));
    require_value(actor.adopt_source_set(inputs, consumer));
    require_value(actor.adopt_publishing_set(outputs, producer));
    EXPECT_EQ(require_value(actor.describe_queue(feed))._producer, producer);
    EXPECT_EQ(require_value(actor.describe_source_set(inputs))._owner, consumer);
    EXPECT_EQ(require_value(actor.describe_publishing_set(outputs))._owner, producer);
    EXPECT_FALSE(require_value(actor.task_status(producer))._attached);
    EXPECT_FALSE(require_value(actor.task_status(consumer))._attached);
    EXPECT_EQ(observe()._snapshot._revision, before._snapshot._revision + 3);
    EXPECT_EQ(require_value(actor.metadata(inputs))._createdAt, source_metadata._createdAt);
    EXPECT_EQ(require_value(actor.metadata(feed))._createdAt, queue_metadata._createdAt);
    EXPECT_EQ(require_value(actor.metadata(outputs))._createdAt, publishing_metadata._createdAt);
    EXPECT_EQ(require_value(actor.metadata(feed))._createdBy, TaskId{1});
    EXPECT_EQ(require_value(actor.state(feed)), EntityState::Draft);
    // Draft adoption is explicit setup, rather than a one-time legacy transfer.
    require_value(actor.adopt_queue(feed, TaskId{1}));
    require_value(actor.adopt_source_set(inputs, TaskId{1}));
    require_value(actor.adopt_publishing_set(outputs, TaskId{1}));
    const auto completed = observe();
    require_value(actor.adopt_queue(feed, TaskId{1}));
    require_value(actor.adopt_source_set(inputs, TaskId{1}));
    require_value(actor.adopt_publishing_set(outputs, TaskId{1}));
    EXPECT_EQ(observe(), completed);
    EXPECT_EQ(completed._claims, before._claims);
  }

  // Producer changes cannot leave any Publishing reference owned by another
  // Task. Remove the references first, complete Draft ownership, then restore.
  TEST_F(TopologyActivationTest, AdoptionRejectsInconsistentPublishingReferencesWithoutWrites)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto other = register_task(actor, "OtherProducer");
    const auto value = graph(actor);
    const auto before = observe();
    expect_error(actor.adopt_queue(value._queue, other), ErrorCode::WrongOwner);
    expect_error(actor.adopt_publishing_set(value._publishing, other), ErrorCode::WrongOwner);
    EXPECT_EQ(observe(), before);
    require_value(actor.set_destinations(value._publishing, {}));
    require_value(actor.adopt_queue(value._queue, other));
    require_value(actor.adopt_publishing_set(value._publishing, other));
    const std::array destinations{value._queue};
    require_value(actor.set_destinations(value._publishing, destinations));
    EXPECT_EQ(require_value(actor.describe_publishing_set(value._publishing))._owner, other);
    EXPECT_EQ(require_value(actor.describe_queue(value._queue))._producer, other);
  }

  // Moving a Draft Queue is one topology revision: both inverse membership
  // lists and the Queue assignment change together, while old bytes stay valid.
  TEST_F(TopologyActivationTest, SourceAssignmentMovesBothMembershipsAndRetainsHistory)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto left = require_value(actor.register_source_set(source("Left")));
    const auto right = require_value(actor.register_source_set(source("Right")));
    const auto feed = require_value(actor.register_queue(queue()));
    const auto initial_left = loaded_source(left);
    const auto initial_right = loaded_source(right);
    const auto initial_queue = loaded_queue(feed);
    const auto left_bytes = definition_bytes(initial_left._reference, 4);
    const auto right_bytes = definition_bytes(initial_right._reference, 4);
    const auto queue_bytes = definition_bytes(initial_queue._reference, 3);
    const auto before = observe();
    require_value(actor.assign_source(feed, left, 7));
    EXPECT_EQ(observe()._snapshot._revision, before._snapshot._revision + 1);
    EXPECT_EQ(require_value(actor.source_members(left)), std::vector<QueueId>{feed});
    const auto joined = loaded_source(left);
    const auto joined_bytes = definition_bytes(joined._reference, 4);
    const auto after_join = observe();
    require_value(actor.assign_source(feed, right, 0));
    EXPECT_EQ(observe()._snapshot._revision, after_join._snapshot._revision + 1);
    EXPECT_TRUE(require_value(actor.source_members(left)).empty());
    EXPECT_EQ(require_value(actor.source_members(right)), std::vector<QueueId>{feed});
    EXPECT_EQ(require_value(actor.describe_queue(feed))._sourceSet, right);
    EXPECT_EQ(require_value(actor.describe_queue(feed))._priority, 0);
    const auto revision = require_value(actor.metadata(feed))._revision;
    EXPECT_EQ(require_value(actor.metadata(left))._revision, revision);
    EXPECT_EQ(require_value(actor.metadata(right))._revision, revision);
    EXPECT_EQ(require_value(actor.metadata(left))._createdAt, initial_left._definition._registration._createdAt);
    EXPECT_EQ(definition_bytes(initial_left._reference, 4), left_bytes);
    EXPECT_EQ(definition_bytes(initial_right._reference, 4), right_bytes);
    EXPECT_EQ(definition_bytes(initial_queue._reference, 3), queue_bytes);
    EXPECT_EQ(definition_bytes(joined._reference, 4), joined_bytes);
    require_value(actor.assign_source(feed, SourceQueueSetId::Invalid, 3));
    EXPECT_TRUE(require_value(actor.source_members(right)).empty());
    EXPECT_EQ(require_value(actor.describe_queue(feed))._sourceSet, SourceQueueSetId::Invalid);
    EXPECT_EQ(require_value(actor.describe_queue(feed))._priority, 3);
    auto attached = require_value(Session::attach(_directory));
    EXPECT_TRUE(require_value(attached.registry().source_members(left)).empty());
    EXPECT_TRUE(require_value(attached.registry().source_members(right)).empty());
  }

  // Priority is part of Source membership as well as the Queue definition.
  // Updating it commits once and an exact retry consumes neither bytes nor IDs.
  TEST_F(TopologyActivationTest, SameSourcePriorityChangeUpdatesItsMirrorExactlyOnce)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto value = graph(actor);
    const auto before = observe();
    require_value(actor.assign_source(value._queue, value._source, 9));
    const auto updated = loaded_source(value._source);
    EXPECT_EQ(observe()._snapshot._revision, before._snapshot._revision + 1);
    EXPECT_EQ(require_value(actor.metadata(value._queue))._revision, updated._definition._registration._revision);
    {
      runtime::RuntimeAccess access(state());
      const auto group = access.section_offset(mapped::SectionKind::MembershipState) +
          updated._slot * limits()._membershipsPerSet * mapped::membership_bytes;
      const auto words = access.read_words(group, 4);
      EXPECT_EQ(words[0], static_cast<std::uint16_t>(value._queue) | (std::uint64_t{9} << 16));
      EXPECT_EQ(words[1], updated._definition._registration._revision);
      EXPECT_EQ(words[2], 0u);
      EXPECT_EQ(words[3], 0u);
    }
    const auto unchanged = observe();
    require_value(actor.assign_source(value._queue, value._source, 9));
    EXPECT_EQ(observe(), unchanged);
  }

  // Configuration can be completed by a registrar while both endpoint owners
  // are offline. Source activation precedes Queue resources, then Publishing.
  TEST_F(TopologyActivationTest, ActivationAllocatesAZeroedRingForOfflineEndpointOwners)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto producer = register_task(actor, "Producer");
    const auto consumer = register_task(actor, "Consumer");
    const auto value = graph(actor, producer, consumer);
    const auto registration = require_value(actor.metadata(value._queue));
    const auto files = observe()._claims;
    require_value(actor.activate_source_set(value._source));
    EXPECT_EQ(require_value(actor.state(value._source)), EntityState::Active);
    EXPECT_EQ(require_value(actor.state(value._queue)), EntityState::Draft);
    EXPECT_EQ(ring_count(), 0u);
    const auto before = observe();
    require_value(actor.activate_queue(value._queue));
    const auto active = loaded_queue(value._queue);
    const auto words = queue_state(active);
    ASSERT_EQ(words.size(), 32u);
    EXPECT_NE(words[2], 0u);
    EXPECT_EQ(words[3], 4u);
    EXPECT_EQ(ring_count(), 1u);
    EXPECT_EQ(observe()._snapshot._revision, before._snapshot._revision + 1);
    EXPECT_EQ(require_value(actor.metadata(value._queue))._createdAt, registration._createdAt);
    {
      runtime::RuntimeAccess access(state());
      const auto& ring = access.record_envelope(words[2], 8);
      EXPECT_EQ(ring._immutableBytes, 0u);
      EXPECT_EQ(ring._previous, 0u);
      EXPECT_EQ(ring._totalBytes, *mapped::ring_storage_bytes(4));
      EXPECT_EQ(ring._registryRevision, active._definition._registration._revision);
      const auto slots = access.read_words(words[2] + mapped::record_envelope_bytes,
                                           static_cast<std::size_t>((ring._totalBytes - mapped::record_envelope_bytes) / 8));
      EXPECT_TRUE(std::all_of(slots.begin(), slots.end(), [](auto word)
      {
        return word == 0;
      }));
    }
    require_value(actor.activate_publishing_set(value._publishing));
    EXPECT_EQ(require_value(actor.state(value._publishing)), EntityState::Active);
    EXPECT_FALSE(require_value(actor.task_status(producer))._attached);
    EXPECT_FALSE(require_value(actor.task_status(consumer))._attached);
    EXPECT_EQ(observe()._claims, files);
    auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.registry().state(value._queue)), EntityState::Active);
  }

  // Incomplete definitions fail before activation; complete definitions may
  // still wait for another entity's activation. Rejection allocates no ring.
  TEST_F(TopologyActivationTest, IncompleteAndUnreadyActivationPreservesEveryDefinition)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto empty = require_value(actor.register_source_set(source("Empty")));
    const auto unowned = require_value(actor.register_source_set(source("Unowned", TaskId::Invalid)));
    const auto incomplete = require_value(actor.register_source_set(source("Incomplete")));
    const auto unowned_feed = require_value(actor.register_queue(queue("UnownedFeed", TaskId{1}, unowned)));
    const auto missing_producer = require_value(actor.register_queue(queue("MissingProducer", TaskId::Invalid, incomplete)));
    const auto unassigned = require_value(actor.register_queue(queue("Unassigned")));
    const auto value = graph(actor);
    const auto no_owner = require_value(actor.register_publishing_set(publishing("UnownedOutputs", TaskId::Invalid)));
    const auto no_destinations = require_value(actor.register_publishing_set(publishing("EmptyOutputs")));
    const auto before = observe();
    expect_error(actor.activate_source_set(empty), ErrorCode::InvalidState);
    expect_error(actor.activate_source_set(unowned), ErrorCode::InvalidState);
    expect_error(actor.activate_source_set(incomplete), ErrorCode::InvalidState);
    expect_error(actor.activate_queue(missing_producer), ErrorCode::InvalidState);
    expect_error(actor.activate_queue(unassigned), ErrorCode::InvalidState);
    expect_error(actor.activate_queue(unowned_feed), ErrorCode::NotActive);
    expect_error(actor.activate_queue(value._queue), ErrorCode::NotActive);
    expect_error(actor.activate_publishing_set(no_owner), ErrorCode::InvalidState);
    expect_error(actor.activate_publishing_set(no_destinations), ErrorCode::InvalidState);
    expect_error(actor.activate_publishing_set(value._publishing), ErrorCode::NotActive);
    EXPECT_EQ(observe(), before);
    EXPECT_EQ(ring_count(), 0u);
  }

  // Active ownership and Queue assignment bind permanently. Exact repeats are
  // still idempotent, so a caller can safely resolve a previously lost result.
  TEST_F(TopologyActivationTest, ActiveDefinitionsRejectChangesButAllowExactNoops)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto other = register_task(actor, "Other");
    const auto value = graph(actor);
    const auto alternative = require_value(actor.register_source_set(source("Alternative")));
    activate(actor, value);
    const auto before = observe();
    require_value(actor.adopt_queue(value._queue, TaskId{1}));
    require_value(actor.adopt_source_set(value._source, TaskId{1}));
    require_value(actor.adopt_publishing_set(value._publishing, TaskId{1}));
    require_value(actor.assign_source(value._queue, value._source, 1));
    activate(actor, value);
    expect_error(actor.adopt_queue(value._queue, other), ErrorCode::InvalidState);
    expect_error(actor.adopt_source_set(value._source, other), ErrorCode::InvalidState);
    expect_error(actor.adopt_publishing_set(value._publishing, other), ErrorCode::InvalidState);
    expect_error(actor.assign_source(value._queue, alternative, 1), ErrorCode::InvalidState);
    expect_error(actor.assign_source(value._queue, SourceQueueSetId::Invalid, 1), ErrorCode::InvalidState);
    expect_error(actor.assign_source(value._queue, value._source, 2), ErrorCode::InvalidState);
    EXPECT_EQ(observe(), before);
  }

  // An Active Source can accept a newly registered complete Draft Queue. It
  // keeps its owner/policy and cannot be emptied by moving its last Draft Queue.
  TEST_F(TopologyActivationTest, ActiveSourcesAcceptCompleteMembersAndRemainNonempty)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto value = graph(actor);
    activate(actor, value);
    const auto extra = require_value(actor.register_queue(queue("Extra", TaskId{1}, value._source, 2)));
    EXPECT_EQ(require_value(actor.state(extra)), EntityState::Draft);
    EXPECT_EQ(require_value(actor.state(value._source)), EntityState::Active);
    EXPECT_EQ(require_value(actor.source_members(value._source)), (std::vector<QueueId>{value._queue, extra}));
    EXPECT_EQ(ring_count(), 1u);
    const auto before = observe();
    expect_error(actor.register_queue(queue("Incomplete", TaskId::Invalid, value._source)), ErrorCode::InvalidState);
    EXPECT_EQ(observe(), before);
    require_value(actor.assign_source(extra, SourceQueueSetId::Invalid, 2));
    const auto single = require_value(actor.register_source_set(source("Single")));
    require_value(actor.assign_source(extra, single, 2));
    require_value(actor.activate_source_set(single));
    const auto nonempty = observe();
    expect_error(actor.assign_source(extra, SourceQueueSetId::Invalid, 2), ErrorCode::InvalidState);
    EXPECT_EQ(observe(), nonempty);
    EXPECT_EQ(require_value(actor.source_members(single)), std::vector<QueueId>{extra});
  }

  // Destination order identifies a snapshot. Only a changed list advances its
  // topology counter; ownership completion and activation retain that counter.
  TEST_F(TopologyActivationTest, PublishingReplacementRetainsOrderedSnapshotsAndAdvancesOnlyOnChange)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto other = register_task(actor, "OtherProducer");
    const auto value = graph(actor);
    const auto second = require_value(actor.register_queue(queue("Second", TaskId{1}, value._source)));
    const auto initial = loaded_publishing(value._publishing);
    const auto initial_bytes = definition_bytes(initial._reference, 5);
    const std::array reordered{second, value._queue};
    require_value(actor.set_destinations(value._publishing, reordered));
    EXPECT_EQ(loaded_publishing(value._publishing)._definition._topologyRevision, 2u);
    EXPECT_EQ(require_value(actor.describe_publishing_set(value._publishing))._destinations,
              (std::vector<QueueId>{second, value._queue}));
    const auto identical = observe();
    require_value(actor.set_destinations(value._publishing, reordered));
    EXPECT_EQ(observe(), identical);
    require_value(actor.set_destinations(value._publishing, {}));
    EXPECT_EQ(loaded_publishing(value._publishing)._definition._topologyRevision, 3u);
    require_value(actor.adopt_publishing_set(value._publishing, other));
    require_value(actor.adopt_publishing_set(value._publishing, TaskId{1}));
    EXPECT_EQ(loaded_publishing(value._publishing)._definition._topologyRevision, 3u);
    const std::array restored{value._queue, second};
    require_value(actor.set_destinations(value._publishing, restored));
    require_value(actor.activate_source_set(value._source));
    require_value(actor.activate_queue(value._queue));
    require_value(actor.activate_queue(second));
    require_value(actor.activate_publishing_set(value._publishing));
    EXPECT_EQ(loaded_publishing(value._publishing)._definition._topologyRevision, 4u);
    require_value(actor.set_destinations(value._publishing, reordered));
    const auto current = loaded_publishing(value._publishing);
    EXPECT_EQ(current._definition._topologyRevision, 5u);
    EXPECT_EQ(current._definition._registration._createdAt, initial._definition._registration._createdAt);
    EXPECT_EQ(current._definition._registration._createdBy, initial._definition._registration._createdBy);
    EXPECT_EQ(definition_bytes(initial._reference, 5), initial_bytes);
    {
      runtime::RuntimeAccess access(state());
      const auto snapshot = access.read_words(current._stateOffset, 2);
      EXPECT_EQ(snapshot[0], 5u);
      EXPECT_EQ(snapshot[1], current._reference);
    }
  }

  // Once Active, only the producer owner may replace destinations, including an
  // exact retry. Every replacement remains nonempty and uses active owned Queues.
  TEST_F(TopologyActivationTest, ActivePublishingChangesRequireOwnerAndActiveDestinations)
  {
    auto created = create();
    auto registrar = created._creator.registry();
    const auto producer = register_task(registrar, "Producer");
    const auto value = graph(registrar, producer);
    const auto pending = require_value(registrar.register_queue(queue("Pending", producer, value._source)));
    const auto other_source = require_value(registrar.register_source_set(source("OtherInputs")));
    const auto other_queue = require_value(registrar.register_queue(queue("OtherFeed", TaskId{1}, other_source)));
    activate(registrar, value);
    require_value(registrar.activate_source_set(other_source));
    require_value(registrar.activate_queue(other_queue));
    auto owner = require_value(created._session.attach_task(producer));
    auto actor = owner.registry();
    const std::array original{value._queue};
    const std::array draft{pending};
    const std::array foreign{other_queue};
    const std::array missing{QueueId{999}};
    const std::array repeated{value._queue, value._queue};
    const auto before = observe();
    expect_error(registrar.set_destinations(value._publishing, original), ErrorCode::WrongOwner);
    expect_error(actor.set_destinations(value._publishing, {}), ErrorCode::InvalidArgument);
    expect_error(actor.set_destinations(value._publishing, draft), ErrorCode::NotActive);
    expect_error(actor.set_destinations(value._publishing, foreign), ErrorCode::WrongOwner);
    expect_error(actor.set_destinations(value._publishing, missing), ErrorCode::NotFound);
    expect_error(actor.set_destinations(value._publishing, repeated), ErrorCode::InvalidArgument);
    require_value(actor.set_destinations(value._publishing, original));
    EXPECT_EQ(observe(), before);
  }

  // Zero identities, absent references, and wrong Queue Set kinds fail through
  // typed Registry operations before any retained definition or mirror changes.
  TEST_F(TopologyActivationTest, InvalidIdentitiesAndReadOnlyViewsRejectWithoutWrites)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto value = graph(actor);
    const std::array destinations{value._queue};
    const std::array zero_destination{QueueId::Invalid};
    const auto before = observe();
    expect_error(actor.adopt_queue(QueueId::Invalid, TaskId{1}), ErrorCode::InvalidArgument);
    expect_error(actor.adopt_source_set(SourceQueueSetId::Invalid, TaskId{1}), ErrorCode::InvalidArgument);
    expect_error(actor.adopt_publishing_set(PublishingQueueSetId::Invalid, TaskId{1}), ErrorCode::InvalidArgument);
    expect_error(actor.adopt_queue(value._queue, TaskId::Invalid), ErrorCode::InvalidArgument);
    expect_error(actor.adopt_source_set(value._source, TaskId::Invalid), ErrorCode::InvalidArgument);
    expect_error(actor.adopt_publishing_set(value._publishing, TaskId::Invalid), ErrorCode::InvalidArgument);
    expect_error(actor.adopt_queue(value._queue, TaskId{999}), ErrorCode::NotFound);
    expect_error(actor.activate_queue(QueueId::Invalid), ErrorCode::InvalidArgument);
    expect_error(actor.activate_source_set(SourceQueueSetId::Invalid), ErrorCode::InvalidArgument);
    expect_error(actor.activate_publishing_set(PublishingQueueSetId::Invalid), ErrorCode::InvalidArgument);
    expect_error(actor.activate_queue(QueueId{999}), ErrorCode::NotFound);
    expect_error(actor.activate_source_set(SourceQueueSetId{999}), ErrorCode::NotFound);
    expect_error(actor.activate_publishing_set(PublishingQueueSetId{999}), ErrorCode::NotFound);
    expect_error(actor.assign_source(QueueId::Invalid, value._source, 1), ErrorCode::InvalidArgument);
    expect_error(actor.assign_source(value._queue, SourceQueueSetId{999}, 1), ErrorCode::NotFound);
    expect_error(actor.assign_source(value._queue,
        static_cast<SourceQueueSetId>(static_cast<std::uint16_t>(value._publishing)), 1), ErrorCode::NotFound);
    expect_error(actor.set_destinations(PublishingQueueSetId::Invalid, destinations), ErrorCode::InvalidArgument);
    expect_error(actor.set_destinations(static_cast<PublishingQueueSetId>(static_cast<std::uint16_t>(value._source)), destinations),
                 ErrorCode::NotFound);
    expect_error(actor.set_destinations(value._publishing, zero_destination), ErrorCode::InvalidArgument);
    auto reader = created._session.registry();
    expect_error(reader.adopt_queue(value._queue, TaskId{1}), ErrorCode::ReadOnlyRegistry);
    expect_error(reader.assign_source(value._queue, value._source, 1), ErrorCode::ReadOnlyRegistry);
    expect_error(reader.set_destinations(value._publishing, destinations), ErrorCode::ReadOnlyRegistry);
    expect_error(reader.activate_source_set(value._source), ErrorCode::ReadOnlyRegistry);
    Registry empty;
    expect_error(empty.adopt_queue(value._queue, TaskId{1}), ErrorCode::InvalidState);
    expect_error(empty.assign_source(value._queue, value._source, 1), ErrorCode::InvalidState);
    expect_error(empty.set_destinations(value._publishing, destinations), ErrorCode::InvalidState);
    expect_error(empty.activate_queue(value._queue), ErrorCode::InvalidState);
    EXPECT_EQ(observe(), before);
  }

  // A Thread Task's Registry keeps the ordinary affinity rules even though a
  // topology update describes offline participants rather than native endpoints.
  TEST_F(TopologyActivationTest, ThreadRegistrarsRejectCrossThreadMutation)
  {
    auto created = create();
    auto registrar = created._creator.registry();
    const auto value = graph(registrar);
    TaskDescriptor descriptor;
    descriptor._name = "ThreadRegistrar";
    descriptor._kind = TaskKind::Thread;
    descriptor._process = TaskId{1};
    const auto id = require_value(registrar.register_task(descriptor));
    auto thread_task = require_value(created._session.attach_task(id));
    auto actor = thread_task.registry();
    require_value(actor.assign_source(value._queue, value._source, 7));
    const auto before = observe();
    std::optional<OperationResult> result;
    std::thread other([&]
    {
      result = actor.activate_source_set(value._source);
    });
    other.join();
    ASSERT_TRUE(result);
    expect_error(*result, ErrorCode::WrongOwner);
    EXPECT_EQ(observe(), before);
    require_value(actor.activate_source_set(value._source));
    EXPECT_EQ(require_value(registrar.state(value._source)), EntityState::Active);
  }

  // Membership capacities apply to topology changes as well as registration.
  // Failing to move a Queue cannot remove it from its original Source first.
  TEST_F(TopologyActivationTest, MembershipCapacityFailurePreservesBothSources)
  {
    auto created = create(2097152, limits().memberships_per_set(1));
    auto actor = created._creator.registry();
    const auto left = require_value(actor.register_source_set(source("Left")));
    const auto right = require_value(actor.register_source_set(source("Right")));
    const auto first = require_value(actor.register_queue(queue("First", TaskId{1}, left)));
    const auto second = require_value(actor.register_queue(queue("Second", TaskId{1}, right)));
    const auto outputs = require_value(actor.register_publishing_set(publishing("Outputs", TaskId{1}, {first})));
    const std::array destinations{first, second};
    const auto before = observe();
    expect_error(actor.assign_source(first, right, 1), ErrorCode::CapacityExceeded);
    expect_error(actor.set_destinations(outputs, destinations), ErrorCode::CapacityExceeded);
    EXPECT_EQ(observe(), before);
    EXPECT_EQ(require_value(actor.source_members(left)), std::vector<QueueId>{first});
    EXPECT_EQ(require_value(actor.source_members(right)), std::vector<QueueId>{second});
  }

  // Measure the complete append for each activation, including the Queue's ring
  // envelope/slots, definition, manifest, redo commands, and alignment. One byte
  // short preserves Draft state and allocation; exactly enough publishes Active.
  TEST_F(TopologyActivationTest, ActivationBudgetsIncludeAllResourcesAndRejectOneByteShort)
  {
    for (unsigned kind = 0; kind != 3; ++kind)
    {
      SCOPED_TRACE(kind);
      std::uint64_t required{};
      {
        auto created = create();
        auto actor = created._creator.registry();
        const auto value = graph(actor);
        prepare_kind(actor, value, kind);
        require_value(activate_kind(actor, value, kind));
        required = observe()._snapshot._tail;
      }
      fs::remove_all(_directory);
      {
        auto created = create(required - 1);
        auto actor = created._creator.registry();
        const auto value = graph(actor);
        prepare_kind(actor, value, kind);
        const auto before = observe();
        const auto rings = ring_count();
        expect_error(activate_kind(actor, value, kind), ErrorCode::LogFull);
        EXPECT_EQ(observe(), before);
        EXPECT_EQ(ring_count(), rings);
        const auto state = kind == 0 ? actor.state(value._source) : kind == 1 ? actor.state(value._queue) : actor.state(value._publishing);
        EXPECT_EQ(require_value(state), EntityState::Draft);
      }
      fs::remove_all(_directory);
      {
        auto created = create(required);
        auto actor = created._creator.registry();
        const auto value = graph(actor);
        prepare_kind(actor, value, kind);
        require_value(activate_kind(actor, value, kind));
        EXPECT_EQ(observe()._snapshot._tail, required);
        const auto state = kind == 0 ? actor.state(value._source) : kind == 1 ? actor.state(value._queue) : actor.state(value._publishing);
        EXPECT_EQ(require_value(state), EntityState::Active);
        const auto before = observe();
        require_value(activate_kind(actor, value, kind));
        EXPECT_EQ(observe(), before);
      }
      fs::remove_all(_directory);
    }
  }

  // Exhaustion cannot prevent a pure identity retry. The private counter edit
  // reaches the otherwise impractical generation boundary without altering any
  // definition, topology counter, or metadata attribution.
  TEST_F(TopologyActivationTest, ExhaustedGenerationAllowsExactActiveRetries)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto value = graph(actor);
    const auto second = require_value(actor.register_queue(queue("Second", TaskId{1}, value._source)));
    activate(actor, value);
    require_value(actor.activate_queue(second));
    const auto original = observe()._snapshot._generation;
    set_word(120, std::numeric_limits<std::uint64_t>::max());
    const auto before = observe();
    activate(actor, value);
    require_value(actor.adopt_queue(value._queue, TaskId{1}));
    require_value(actor.adopt_source_set(value._source, TaskId{1}));
    require_value(actor.adopt_publishing_set(value._publishing, TaskId{1}));
    require_value(actor.assign_source(value._queue, value._source, 1));
    const std::array original_destinations{value._queue};
    const std::array changed_destinations{second};
    require_value(actor.set_destinations(value._publishing, original_destinations));
    expect_error(actor.set_destinations(value._publishing, changed_destinations), ErrorCode::CapacityExceeded);
    EXPECT_EQ(observe(), before);
    set_word(120, original);
  }

  // Active metadata and its reserved resource mirrors must agree. Corrupt one
  // fact at a time, capture the raw image independently of Registry decoding,
  // and prove attachment/lookup/no-op mutation refuse to repair or overwrite it.
  TEST_F(TopologyActivationTest, CorruptActiveResourcesRejectBeforeAnyWrites)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto value = graph(actor);
    activate(actor, value);
    const auto feed = loaded_queue(value._queue);
    const auto inputs = loaded_source(value._source);
    const auto outputs = loaded_publishing(value._publishing);
    const auto words = queue_state(feed);
    const auto ring = words[2];
    std::uint64_t source_state{};
    {
      runtime::RuntimeAccess access(state());
      source_state = access.section_offset(mapped::SectionKind::SourceState);
    }
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> changes{
        {feed._indexOffset + 24, 1},
        {inputs._indexOffset + 24, 1},
        {feed._stateOffset + 16, 0},
        {feed._stateOffset + 16, feed._reference},
        {feed._stateOffset + 24, 5},
        {feed._stateOffset, 1},
        {feed._stateOffset + 248, 1},
        {source_state, 1},
        {outputs._stateOffset + 8, inputs._reference},
        {outputs._reference + mapped::record_envelope_bytes + 88, std::numeric_limits<std::uint64_t>::max()},
        {ring + mapped::record_envelope_bytes, 1},
        {ring + 40, feed._definition._registration._revision + 1}};
    for (const auto& [target, replacement] : changes)
    {
      SCOPED_TRACE(target);
      const auto original = raw_image();
      const auto old = original[static_cast<std::size_t>(target / 8)];
      ASSERT_NE(old, replacement);
      set_word(target, replacement);
      const auto evidence = raw_image();
      expect_error(Session::attach(_directory), ErrorCode::IoError);
      expect_error(actor.state(value._queue), ErrorCode::IoError);
      expect_error(actor.activate_queue(value._queue), ErrorCode::IoError);
      EXPECT_EQ(raw_image(), evidence);
      set_word(target, old);
    }
    EXPECT_EQ(require_value(actor.state(value._queue)), EntityState::Active);
  }

  // One active Queue owns one ring. A shared ring reference and an unreferenced
  // committed allocation are corruption even when all slot bytes remain zero.
  TEST_F(TopologyActivationTest, SharedAndOrphanRingAllocationsAreRejected)
  {
    auto created = create();
    const auto claim = created._creator.claim();
    auto actor = created._creator.registry();
    const auto value = graph(actor);
    const auto second = require_value(actor.register_queue(queue("Second", TaskId{1}, value._source)));
    activate(actor, value);
    require_value(actor.activate_queue(second));
    const auto first = loaded_queue(value._queue);
    const auto other = loaded_queue(second);
    const auto first_ring = queue_state(first)[2];
    const auto other_ring = queue_state(other)[2];
    ASSERT_NE(first_ring, other_ring);
    set_word(other._stateOffset + 16, first_ring);
    const auto shared_evidence = raw_image();
    expect_error(Session::attach(_directory), ErrorCode::IoError);
    expect_error(actor.describe_queue(second), ErrorCode::IoError);
    EXPECT_EQ(raw_image(), shared_evidence);
    set_word(other._stateOffset + 16, other_ring);
    {
      // Valid journal framing deliberately bypasses activation's owner/Queue
      // binding. The domain validator, rather than byte framing, rejects it.
      runtime::RuntimeAccess access(state());
      mapped::TransactionPlan plan;
      plan._base = access._snapshot;
      plan._kind = mapped::JournalKind::Append;
      plan._actor = claim._task;
      plan._actorIncarnation = claim._incarnation;
      plan._newRevision = access._snapshot._revision + 1;
      mapped::JournalRecord ring;
      ring._kind = 8;
      ring._mutable.resize(4 * mapped::ring_slot_bytes);
      plan._records.push_back(std::move(ring));
      const std::array<std::uint64_t, 1> zero{0};
      plan._commands.push_back(runtime::runtime_write_words(
          access.section_offset(mapped::SectionKind::SourceState), zero));
      require_value(access._journal.execute(access._guard, plan));
    }
    const auto orphan_evidence = raw_image();
    expect_error(Session::attach(_directory), ErrorCode::IoError);
    expect_error(actor.state(value._queue), ErrorCode::IoError);
    expect_error(actor.activate_queue(value._queue), ErrorCode::IoError);
    EXPECT_EQ(raw_image(), orphan_evidence);
  }

  // Restoring the latest owner cannot erase an illegal change after activation.
  // Two private, well-framed revisions keep the latest mirrors consistent and
  // isolate the historical immutability rule from ordinary index corruption.
  TEST_F(TopologyActivationTest, LaterRestorationCannotHideActiveOwnershipChange)
  {
    auto created = create();
    const auto claim = created._creator.claim();
    auto actor = created._creator.registry();
    const auto other = register_task(actor, "OtherConsumer");
    const auto value = graph(actor);
    activate(actor, value);
    const auto inputs = loaded_source(value._source);
    auto changed = inputs._definition;
    changed._descriptor._owner = other;
    const auto altered = append_source(changed, claim, inputs._indexOffset, inputs._slot, inputs._reference);
    changed._descriptor._owner = TaskId{1};
    append_source(changed, claim, inputs._indexOffset, inputs._slot, altered);
    const auto evidence = raw_image();
    expect_error(Session::attach(_directory), ErrorCode::IoError);
    expect_error(actor.source_members(value._source), ErrorCode::IoError);
    expect_error(actor.activate_source_set(value._source), ErrorCode::IoError);
    EXPECT_EQ(raw_image(), evidence);
  }

  // An atomic batch may retain a temporary destination list before restoring
  // the committed one. The visible list did not change, so counter 1 remains
  // correct; counting the two physical changes and ending at 3 is corruption.
  TEST_F(TopologyActivationTest, RestoredPublishingDestinationsUseTheCommittedCounter)
  {
    for (const auto counter : {1u, 3u})
    {
      SCOPED_TRACE(counter);
      check_publishing_batch_transition(false, counter);
    }
  }

  // Multiple physical lists in one revision still represent only one visible
  // change. Compare the final list with the preceding committed list and require
  // one counter increment, rather than one increment per intermediate record.
  TEST_F(TopologyActivationTest, ChangedPublishingDestinationsIncrementOncePerCommittedRevision)
  {
    for (const auto counter : {2u, 3u})
    {
      SCOPED_TRACE(counter);
      check_publishing_batch_transition(true, counter);
    }
  }

  // Initial counters are also a committed-state rule. A newly introduced
  // Publishing identity must finish its first atomic batch at counter 1, even
  // when an earlier retained definition inside that batch carries another value.
  TEST_F(TopologyActivationTest, InitialPublishingCounterUsesTheFinalBatchDefinition)
  {
    for (const auto counter : {1u, 3u})
    {
      SCOPED_TRACE(counter);
      {
        auto created = create();
        const auto claim = created._creator.claim();
        auto actor = created._creator.registry();
        const auto inputs = require_value(actor.register_source_set(source()));
        const auto feed = require_value(actor.register_queue(queue("Feed", TaskId{1}, inputs)));
        require_value(actor.activate_source_set(inputs));
        require_value(actor.activate_queue(feed));
        mapped::PublishingDefinition definition;
        definition._id = PublishingQueueSetId{2};
        definition._descriptor = publishing("InitialOutputs", TaskId{1}, {feed});
        definition._descriptor._acceptance = AcceptancePolicy::AllDestinations;
        definition._state = EntityState::Active;
        definition._registration = RegistrationMetadata{TaskId{1},
            require_value(created._session.metadata())._createdAt, 1};
        definition._topologyRevision = 2;
        {
          runtime::RuntimeAccess access(state());
          runtime::LoadedPublishing target;
          target._indexOffset = access.section_offset(mapped::SectionKind::PublishingIndex);
          target._stateOffset = access.section_offset(mapped::SectionKind::PublishingState);
          TransitionBatch batch(access, claim);
          const auto previous = batch.add_publishing(definition, target, 0);
          definition._topologyRevision = counter;
          batch.add_publishing(definition, target, previous);
          batch.commit();
        }
        const auto evidence = observe();
        if (counter == 1)
        {
          auto attached = require_value(Session::attach(_directory));
          EXPECT_EQ(require_value(attached.registry().find_publishing_set("InitialOutputs")), definition._id);
          EXPECT_EQ(loaded_publishing(definition._id)._definition._topologyRevision, 1u);
          EXPECT_EQ(require_value(attached.registry().state(definition._id)), EntityState::Active);
          EXPECT_EQ(observe(), evidence);
        }
        else
        {
          expect_error(Session::attach(_directory), ErrorCode::IoError);
          expect_error(actor.find_publishing_set("InitialOutputs"), ErrorCode::IoError);
          expect_error(actor.activate_publishing_set(definition._id), ErrorCode::IoError);
          EXPECT_EQ(raw_image(), evidence._image);
        }
      }
      fs::remove_all(_directory);
    }
  }

  // Ownership/state rules bind consecutive committed snapshots. These private
  // Source and Queue records temporarily describe different Draft owners before
  // restoring the original Active definitions in the same atomic batch.
  TEST_F(TopologyActivationTest, SameRevisionQueueAndSourceRestorationPreservesActiveOwnership)
  {
    auto created = create();
    const auto claim = created._creator.claim();
    auto actor = created._creator.registry();
    const auto other = register_task(actor, "OtherOwner");
    const auto value = graph(actor);
    activate(actor, value);
    const auto inputs = loaded_source(value._source);
    const auto feed = loaded_queue(value._queue);
    const auto old_ring_state = queue_state(feed);
    auto intermediate_source = inputs._definition;
    intermediate_source._state = EntityState::Draft;
    intermediate_source._descriptor._owner = other;
    auto intermediate_queue = feed._definition;
    intermediate_queue._state = EntityState::Draft;
    intermediate_queue._descriptor._producer = other;
    intermediate_queue._descriptor._ringCapacity = 8;
    {
      runtime::RuntimeAccess access(state());
      TransitionBatch batch(access, claim);
      const auto source_previous = batch.add_source(intermediate_source, inputs, inputs._reference);
      const auto queue_previous = batch.add_queue(intermediate_queue, feed, feed._reference);
      batch.add_source(inputs._definition, inputs, source_previous);
      batch.add_queue(feed._definition, feed, queue_previous);
      batch.commit();
    }
    const auto evidence = observe();
    auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.registry().describe_source_set(value._source))._owner, TaskId{1});
    EXPECT_EQ(require_value(attached.registry().describe_queue(value._queue))._producer, TaskId{1});
    EXPECT_EQ(require_value(attached.registry().state(value._source)), EntityState::Active);
    EXPECT_EQ(require_value(attached.registry().state(value._queue)), EntityState::Active);
    EXPECT_EQ(queue_state(loaded_queue(value._queue)), old_ring_state);
    EXPECT_EQ(observe(), evidence);
  }

  // The same final-state rule applies to Publishing ownership and lifecycle.
  // Its original destination list and counter remain unchanged throughout;
  // only an unobservable intermediate definition names a different Draft owner.
  TEST_F(TopologyActivationTest, SameRevisionPublishingRestorationPreservesActiveOwnership)
  {
    auto created = create();
    const auto claim = created._creator.claim();
    auto actor = created._creator.registry();
    const auto other = register_task(actor, "OtherOwner");
    const auto value = graph(actor);
    activate(actor, value);
    const auto outputs = loaded_publishing(value._publishing);
    auto intermediate = outputs._definition;
    intermediate._state = EntityState::Draft;
    intermediate._descriptor._owner = other;
    {
      runtime::RuntimeAccess access(state());
      TransitionBatch batch(access, claim);
      const auto previous = batch.add_publishing(intermediate, outputs, outputs._reference);
      batch.add_publishing(outputs._definition, outputs, previous);
      batch.commit();
    }
    const auto evidence = observe();
    auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.registry().describe_publishing_set(value._publishing))._owner, TaskId{1});
    EXPECT_EQ(require_value(attached.registry().state(value._publishing)), EntityState::Active);
    EXPECT_EQ(loaded_publishing(value._publishing)._definition._topologyRevision, 1u);
    EXPECT_EQ(observe(), evidence);
  }

  // Coalescing logical transitions must not skip structural history checks.
  // An intermediate record with a broken predecessor or changed creation
  // attribution remains corrupt even when the final definition restores it.
  TEST_F(TopologyActivationTest, IntermediateRecordsStillRequirePredecessorsAndCreationAttribution)
  {
    for (unsigned corruption = 0; corruption != 3; ++corruption)
    {
      SCOPED_TRACE(corruption);
      {
        auto created = create();
        const auto claim = created._creator.claim();
        auto actor = created._creator.registry();
        const auto other = register_task(actor, "OtherCreator");
        const auto value = graph(actor);
        activate(actor, value);
        const auto outputs = loaded_publishing(value._publishing);
        auto intermediate = outputs._definition;
        if (corruption == 1)
        {
          intermediate._registration._createdBy = other;
        }
        if (corruption == 2)
        {
          intermediate._registration._createdAt += std::chrono::nanoseconds{1};
        }
        {
          runtime::RuntimeAccess access(state());
          TransitionBatch batch(access, claim);
          const auto previous = batch.add_publishing(intermediate, outputs, corruption == 0 ? 0 : outputs._reference);
          batch.add_publishing(outputs._definition, outputs, previous);
          batch.commit();
        }
        const auto evidence = raw_image();
        expect_error(Session::attach(_directory), ErrorCode::IoError);
        expect_error(actor.find_publishing_set("Outputs"), ErrorCode::IoError);
        expect_error(actor.activate_publishing_set(value._publishing), ErrorCode::IoError);
        EXPECT_EQ(raw_image(), evidence);
      }
      fs::remove_all(_directory);
    }
  }

  // Empty active rings are resources, not pending deliveries. Task lifetime
  // replacement still uses reserved state when the activation filled the log;
  // exact topology retries likewise require no extra bytes or generation.
  TEST_F(TopologyActivationTest, ActiveResourcesAllowExactRetriesAndTaskReplacementAtLogFull)
  {
    std::uint64_t required{};
    {
      auto created = create();
      const auto value = graph(created._creator.registry());
      activate(created._creator.registry(), value);
      required = observe()._snapshot._tail;
    }
    fs::remove_all(_directory);
    auto created = create(required);
    auto session = std::move(created._session);
    ActivationGraph value;
    {
      auto actor = created._creator.registry();
      value = graph(actor);
      activate(actor, value);
      const auto before = observe();
      activate(actor, value);
      require_value(actor.adopt_queue(value._queue, TaskId{1}));
      require_value(actor.adopt_source_set(value._source, TaskId{1}));
      require_value(actor.adopt_publishing_set(value._publishing, TaskId{1}));
      require_value(actor.assign_source(value._queue, value._source, 1));
      const std::array destinations{value._queue};
      require_value(actor.set_destinations(value._publishing, destinations));
      EXPECT_EQ(observe(), before);
    }
    const auto before = observe();
    const auto initial_queue = loaded_queue(value._queue);
    const auto initial_ring_state = queue_state(initial_queue);
    {
      auto closing = std::move(created._creator);
    }
    EXPECT_FALSE(require_value(session.registry().task_status(TaskId{1}))._attached);
    auto replacement = require_value(session.attach_task(TaskId{1}));
    EXPECT_EQ(replacement.claim()._incarnation, 2u);
    // This fixture qualifies administrative activation only. Real writer
    // admission additionally needs a declared exact schema (public tests).
    expect_error(replacement.next_publication_id(), ErrorCode::InvalidState);
    const auto after = observe();
    EXPECT_EQ(after._snapshot._tail, required);
    EXPECT_EQ(after._snapshot._accounted, before._snapshot._accounted);
    EXPECT_EQ(after._snapshot._revision, before._snapshot._revision);
    EXPECT_EQ(queue_state(loaded_queue(value._queue)), initial_ring_state);
    EXPECT_EQ(loaded_queue(value._queue)._reference, initial_queue._reference);
    EXPECT_EQ(require_value(session.registry().state(value._source)), EntityState::Active);
    EXPECT_EQ(require_value(session.registry().state(value._publishing)), EntityState::Active);
    EXPECT_EQ(ring_count(), 1u);
  }
} // namespace
