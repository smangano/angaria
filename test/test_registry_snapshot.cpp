// Public Registry snapshots are owning, consistent catalogs. A Registry
// revision describes definitions/topology, not historical native Task claims.
#include "result_helpers.h"
#include "angaria/runtime.h"
#include "runtime_state.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace
{
  using namespace angaria;
  namespace fs = std::filesystem;

  using angaria::test_support::require_value;

  template <typename T>
  void expect_error(const Result<T>& result, ErrorCode code)
  {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    EXPECT_EQ(std::get<Error>(result)._code, code) << std::get<Error>(result)._message;
  }

  std::string image(const fs::path& path)
  {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
  }

  template <typename Entry, typename Id>
  const Entry& by_id(const std::vector<Entry>& entries, Id id)
  {
    const auto found = std::find_if(entries.begin(), entries.end(), [id](const auto& entry)
    {
      return entry._id == id;
    });
    if (found == entries.end())
    {
      throw std::runtime_error("Expected an entity in the Registry snapshot");
    }
    return *found;
  }

  struct Connection
  {
    SourceQueueSetId _source{};
    QueueId _first{};
    QueueId _second{};
    PublishingQueueSetId _publishing{};
  };

  class RegistrySnapshotTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-snapshot-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    CreatedSession create()
    {
      SessionOptions options;
      options._name = "Registry inspection";
      options._directory = _root / "session";
      options._maxLogBytes = 2097152;
      options._limits = CapacityLimits::empty().tasks(8).queues(8).source_sets(8)
          .publishing_sets(8).memberships_per_set(4).fields(8).message_schemas(8);
      options._defaults._ringCapacity = 3;
      TaskDescriptor creator;
      creator._name = "Setup";
      return require_value(Session::create(options, creator));
    }

    static Connection connection(Registry registry, bool active = false)
    {
      SourceQueueSetDescriptor source;
      source._name = "Inputs";
      source._owner = TaskId{1};
      Connection result;
      result._source = require_value(registry.register_source_set(source));
      QueueDescriptor queue;
      queue._name = "First";
      queue._producer = TaskId{1};
      queue._sourceSet = result._source;
      queue._priority = 8;
      result._first = require_value(registry.register_queue(queue));
      queue._name = "Second";
      queue._priority = 2;
      result._second = require_value(registry.register_queue(queue));
      PublishingQueueSetDescriptor publishing;
      publishing._name = "Updates";
      publishing._owner = TaskId{1};
      publishing._destinations = {result._first, result._second};
      result._publishing = require_value(registry.register_publishing_set(publishing));
      if (active)
      {
        require_value(registry.activate_source_set(result._source));
        require_value(registry.activate_queue(result._first));
        require_value(registry.activate_queue(result._second));
        require_value(registry.activate_publishing_set(result._publishing));
      }
      return result;
    }

    fs::path _root;
  };

  TEST_F(RegistrySnapshotTest, BootstrapCatalogIncludesCreatorWithoutClaimingAnotherTask)
  {
    auto created = create();
    const auto reader = created._session.registry();
    const auto before = image(_root / "session" / "session.ang");
    const auto snapshot = require_value(reader.snapshot());
    EXPECT_EQ(snapshot._revision, 1u);
    EXPECT_EQ(snapshot._currentRevision, 1u);
    ASSERT_EQ(snapshot._tasks.size(), 1u);
    EXPECT_EQ(snapshot._tasks[0]._id, TaskId{1});
    EXPECT_EQ(snapshot._tasks[0]._descriptor._name, "Setup");
    EXPECT_EQ(snapshot._tasks[0]._registration._createdBy, TaskId{1});
    EXPECT_TRUE(snapshot._fields.empty());
    EXPECT_TRUE(snapshot._messages.empty());
    EXPECT_TRUE(snapshot._queues.empty());
    EXPECT_TRUE(snapshot._sources.empty());
    EXPECT_TRUE(snapshot._publishing.empty());
    EXPECT_EQ(image(_root / "session" / "session.ang"), before);
    expect_error(created._session.attach_task(TaskId{1}), ErrorCode::OwnershipBusy);
  }

  // Insertion order can differ from identity order. Exact schema versions and
  // repeated Fields are preserved while the catalog has deterministic ordering.
  TEST_F(RegistrySnapshotTest, EnumeratesEveryKindWithEffectivePoliciesAndExactSchemas)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto ids = connection(actor, true);
    FieldDescriptor field;
    field._id = static_cast<FieldId>(0xeeffffffu);
    field._name = "Quantity";
    field._base = EFieldBase::BASE_UINT64;
    field._size = 8;
    require_value(actor.register_field(field));
    field._id = static_cast<FieldId>(0xee000000u);
    field._name = "Price";
    const auto price = require_value(actor.register_field(field));
    MessageDescriptor message;
    message._schema = {static_cast<MessageType>(7), 2};
    message._name = "Quote";
    message._fields = {price, price};
    require_value(actor.register_message(message));
    message._schema._version = 1;
    message._fields.clear();
    require_value(actor.register_message(message));
    const auto snapshot = require_value(created._session.registry().snapshot());
    ASSERT_EQ(snapshot._fields.size(), 2u);
    EXPECT_EQ(snapshot._fields[0]._descriptor._id, price);
    ASSERT_EQ(snapshot._messages.size(), 2u);
    EXPECT_EQ(snapshot._messages[0]._descriptor._schema._version, 1u);
    EXPECT_EQ(snapshot._messages[1]._descriptor._fields, (std::vector<FieldId>{price, price}));
    ASSERT_EQ(snapshot._queues.size(), 2u);
    EXPECT_EQ(snapshot._queues[0]._id, ids._first);
    EXPECT_EQ(snapshot._queues[0]._descriptor._ringCapacity, 3u);
    EXPECT_EQ(snapshot._queues[0]._descriptor._delivery, DeliveryPolicy::RecoverFromLog);
    EXPECT_EQ(snapshot._queues[0]._state, EntityState::Active);
    ASSERT_EQ(snapshot._sources.size(), 1u);
    EXPECT_EQ(snapshot._sources[0]._members, (std::vector<QueueId>{ids._first, ids._second}));
    EXPECT_EQ(snapshot._sources[0]._descriptor._selection, SelectionPolicy::Strict);
    ASSERT_EQ(snapshot._publishing.size(), 1u);
    EXPECT_EQ(snapshot._publishing[0]._descriptor._destinations, snapshot._sources[0]._members);
    EXPECT_EQ(snapshot._publishing[0]._state, EntityState::Active);
    EXPECT_EQ(snapshot._publishing[0]._topologyRevision, 1u);
  }

  // Registering a Queue updates its inverse Source membership in one revision.
  // Inspect every retained cut so an observer cannot see half that relationship.
  TEST_F(RegistrySnapshotTest, HistoricalCutsContainOnlyCommittedEntitiesAndInverseMemberships)
  {
    auto created = create();
    const auto ids = connection(created._creator.registry());
    auto reader = created._session.registry();
    const auto latest = require_value(reader.snapshot());
    for (std::uint64_t revision = 1; revision <= latest._revision; ++revision)
    {
      SCOPED_TRACE(revision);
      const auto old = require_value(reader.snapshot(revision));
      EXPECT_EQ(old._revision, revision);
      EXPECT_EQ(old._currentRevision, latest._revision);
      for (const auto& queue : old._queues)
      {
        const auto& source = by_id(old._sources, queue._descriptor._sourceSet);
        EXPECT_EQ(std::count(source._members.begin(), source._members.end(), queue._id), 1);
        EXPECT_LE(queue._registration._revision, revision);
      }
      for (const auto& source : old._sources)
      {
        for (const auto queue : source._members)
        {
          EXPECT_EQ(by_id(old._queues, queue)._descriptor._sourceSet, source._id);
        }
      }
    }
    EXPECT_TRUE(require_value(reader.snapshot(1))._queues.empty());
    EXPECT_EQ(by_id(latest._sources, ids._source)._members.size(), 2u);
  }

  TEST_F(RegistrySnapshotTest, RetainsActivationStatesAtTheirOwnRegistryRevisions)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto ids = connection(actor);
    const auto draft = require_value(actor.snapshot())._revision;
    require_value(actor.activate_source_set(ids._source));
    const auto source_active = require_value(actor.snapshot())._revision;
    require_value(actor.activate_queue(ids._first));
    require_value(actor.activate_queue(ids._second));
    require_value(actor.activate_publishing_set(ids._publishing));
    const auto before = require_value(actor.snapshot(draft));
    const auto middle = require_value(actor.snapshot(source_active));
    EXPECT_EQ(by_id(before._sources, ids._source)._state, EntityState::Draft);
    EXPECT_EQ(by_id(middle._sources, ids._source)._state, EntityState::Active);
    EXPECT_EQ(by_id(middle._queues, ids._first)._state, EntityState::Draft);
    EXPECT_EQ(by_id(middle._publishing, ids._publishing)._state, EntityState::Draft);
    EXPECT_EQ(require_value(actor.snapshot())._publishing[0]._state, EntityState::Active);
  }

  TEST_F(RegistrySnapshotTest, RetainsBothSidesOfDraftSourceMoves)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto ids = connection(actor);
    SourceQueueSetDescriptor source;
    source._name = "OtherInputs";
    source._owner = TaskId{1};
    const auto other = require_value(actor.register_source_set(source));
    const auto before = require_value(actor.snapshot());
    require_value(actor.assign_source(ids._first, other, 17));
    const auto after = require_value(actor.snapshot());
    const auto retained = require_value(actor.snapshot(before._revision));
    EXPECT_EQ(by_id(retained._queues, ids._first)._descriptor._sourceSet, ids._source);
    EXPECT_EQ(by_id(retained._sources, ids._source)._members, (std::vector<QueueId>{ids._first, ids._second}));
    EXPECT_TRUE(by_id(retained._sources, other)._members.empty());
    EXPECT_EQ(by_id(after._queues, ids._first)._descriptor._priority, 17u);
    EXPECT_EQ(by_id(after._sources, ids._source)._members, (std::vector<QueueId>{ids._second}));
    EXPECT_EQ(by_id(after._sources, other)._members, (std::vector<QueueId>{ids._first}));
  }

  TEST_F(RegistrySnapshotTest, PreservesPublisherDestinationOrderGenerationAndCreationAttribution)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto ids = connection(actor, true);
    const auto before = require_value(actor.snapshot());
    require_value(actor.set_destinations(ids._publishing, std::vector<QueueId>{ids._second}));
    const auto after = require_value(actor.snapshot());
    const auto retained = require_value(actor.snapshot(before._revision));
    const auto& old = by_id(retained._publishing, ids._publishing);
    const auto& current = by_id(after._publishing, ids._publishing);
    EXPECT_EQ(old._descriptor._destinations, (std::vector<QueueId>{ids._first, ids._second}));
    EXPECT_EQ(current._descriptor._destinations, (std::vector<QueueId>{ids._second}));
    EXPECT_EQ(current._topologyRevision, old._topologyRevision + 1);
    EXPECT_EQ(current._registration._createdBy, old._registration._createdBy);
    EXPECT_EQ(current._registration._createdAt, old._registration._createdAt);
    EXPECT_GT(current._registration._revision, old._registration._revision);
  }

  TEST_F(RegistrySnapshotTest, OwningCopiesRemainUsableAfterHandlesCloseAndDoNotRetainClaims)
  {
    std::optional<RegistrySnapshot> captured;
    {
      auto created = create();
      connection(created._creator.registry(), true);
      captured = require_value(created._creator.registry().snapshot());
    }
    auto attached = require_value(Session::attach(_root / "session"));
    auto replacement = require_value(attached.attach_task(TaskId{1}));
    EXPECT_EQ(replacement.claim()._incarnation, 2u);
    ASSERT_TRUE(captured);
    EXPECT_EQ(captured->_tasks[0]._descriptor._name, "Setup");
    EXPECT_EQ(captured->_publishing[0]._descriptor._destinations.size(), 2u);
    captured->_tasks[0]._descriptor._name = "Local copy";
    EXPECT_EQ(require_value(attached.registry().snapshot())._tasks[0]._descriptor._name, "Setup");
  }

  TEST_F(RegistrySnapshotTest, InvalidRevisionAndEmptyOrMovedViewsFailWithoutWriting)
  {
    auto created = create();
    auto reader = created._session.registry();
    const auto before = image(_root / "session" / "session.ang");
    expect_error(reader.snapshot(0), ErrorCode::InvalidArgument);
    expect_error(reader.snapshot(2), ErrorCode::NotFound);
    Registry empty;
    expect_error(empty.snapshot(), ErrorCode::InvalidState);
    auto moved = std::move(reader);
    expect_error(reader.snapshot(), ErrorCode::InvalidState);
    EXPECT_EQ(require_value(moved.snapshot())._revision, 1u);
    TaskDescriptor forbidden;
    forbidden._name = "NotRegistered";
    expect_error(moved.register_task(forbidden), ErrorCode::ReadOnlyRegistry);
    EXPECT_EQ(image(_root / "session" / "session.ang"), before);
  }

  TEST_F(RegistrySnapshotTest, ThreadActorViewsKeepTheirAffinityWhileSessionViewsRemainInspectable)
  {
    auto created = create();
    TaskDescriptor thread;
    thread._name = "ThreadRegistrar";
    thread._kind = TaskKind::Thread;
    thread._process = TaskId{1};
    const auto id = require_value(created._creator.registry().register_task(thread));
    auto task = require_value(created._session.attach_task(id));
    auto actor = task.registry();
    auto reader = created._session.registry();
    std::optional<Result<RegistrySnapshot>> wrong_thread;
    std::optional<Result<RegistrySnapshot>> independent_reader;
    std::thread caller([&]
    {
      wrong_thread = actor.snapshot();
      independent_reader = reader.snapshot();
    });
    caller.join();
    ASSERT_TRUE(wrong_thread);
    expect_error(*wrong_thread, ErrorCode::WrongOwner);
    ASSERT_TRUE(independent_reader);
    EXPECT_TRUE(std::holds_alternative<RegistrySnapshot>(*independent_reader));
    EXPECT_EQ(require_value(actor.snapshot())._tasks.size(), 2u);
  }

  // Fork inheritance is deliberately not a new independent Angaria client.
  // The child checks a plain inherited view, then uses _exit without cleanup.
  TEST_F(RegistrySnapshotTest, InheritedViewRejectsBeforeUsingParentProcessResources)
  {
    auto created = create();
    auto reader = created._session.registry();
    const auto pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0)
    {
      ::alarm(2);
      const auto result = reader.snapshot();
      const auto* error = std::get_if<Error>(&result);
      ::_exit(error && error->_code == ErrorCode::WrongOwner ? 0 : 1);
    }
    int status{};
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(require_value(reader.snapshot())._tasks.size(), 1u);
  }

  // Private corruption fixture: erase a current Task index row while retaining
  // its committed immutable definition. Even a bootstrap historical query must
  // reject this malformed current catalog rather than hide the orphaned record.
  TEST_F(RegistrySnapshotTest, RejectsOrphanedTaskDefinitionsBeforeCurrentOrHistoricalInspection)
  {
    auto created = create();
    TaskDescriptor participant;
    participant._name = "OfflineParticipant";
    require_value(created._creator.registry().register_task(participant));
    {
      namespace runtime = angaria::detail;
      auto state = std::make_shared<runtime::SessionState>();
      state->_directory = fs::canonical(_root / "session");
      state->_controller = require_value(runtime::mapped::SessionController::open_existing(state->_directory));
      runtime::RuntimeAccess access(state);
      const auto index = access.section_offset(runtime::mapped::SectionKind::TaskIndex) +
                         runtime::mapped::index_entry_bytes;
      const std::array<std::byte, runtime::mapped::index_entry_bytes> empty{};
      require_value(access._file->write_exact(index, empty));
    }
    const auto corrupted = image(_root / "session" / "session.ang");
    auto reader = created._session.registry();
    expect_error(reader.snapshot(), ErrorCode::IoError);
    expect_error(reader.snapshot(1), ErrorCode::IoError);
    expect_error(Session::attach(_root / "session"), ErrorCode::IoError);
    EXPECT_EQ(image(_root / "session" / "session.ang"), corrupted);
  }
} // namespace
