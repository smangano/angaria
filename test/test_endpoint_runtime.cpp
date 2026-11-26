// Public endpoint identity/lifetime examples. Activation supplies resources;
// this step opens handles and retains ownership, without sending or receiving.
// Raw snapshots and fork checks below are private qualification infrastructure.

#include "result_helpers.h"
#include "angaria/runtime.h"
#include "native_atomic.h"
#include "runtime_state.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <latch>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include <sys/wait.h>
#include <unistd.h>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace fs = std::filesystem;

  using angaria::test_support::require_value;

  template <typename T>
  void expect_error(const Result<T>& result, ErrorCode code)
  {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    EXPECT_EQ(std::get<Error>(result)._code, code) << std::get<Error>(result)._message;
  }

  struct Endpoints
  {
    QueueId _queue{};
    SourceQueueSetId _source{};
    PublishingQueueSetId _publishing{};
  };

  class EndpointRuntimeTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-endpoints-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    CreatedSession create(const std::string& name = "session")
    {
      SessionOptions options;
      options._name = "Endpoint ownership";
      options._directory = _root / name;
      options._maxLogBytes = 2097152;
      options._limits = CapacityLimits::empty().tasks(8).queues(8).source_sets(8)
          .publishing_sets(8).memberships_per_set(4);
      options._defaults._ringCapacity = 3;
      TaskDescriptor creator;
      creator._name = "Creator";
      return require_value(Session::create(options, creator));
    }

    static Endpoints graph(Registry registry, TaskId owner = TaskId{1}, const std::string& prefix = "", bool active = true)
    {
      SourceQueueSetDescriptor source;
      source._name = prefix + "Inputs";
      source._owner = owner;
      Endpoints ids;
      ids._source = require_value(registry.register_source_set(source));
      QueueDescriptor queue;
      queue._name = prefix + "Feed";
      queue._producer = owner;
      queue._sourceSet = ids._source;
      ids._queue = require_value(registry.register_queue(queue));
      PublishingQueueSetDescriptor publishing;
      publishing._name = prefix + "Updates";
      publishing._owner = owner;
      publishing._destinations = {ids._queue};
      ids._publishing = require_value(registry.register_publishing_set(publishing));
      if (active)
      {
        require_value(registry.activate_source_set(ids._source));
        require_value(registry.activate_queue(ids._queue));
        require_value(registry.activate_publishing_set(ids._publishing));
      }
      return ids;
    }

    runtime::mapped::JournalSnapshot snapshot(const std::string& name = "session")
    {
      auto state = std::make_shared<runtime::SessionState>();
      state->_directory = fs::canonical(_root / name);
      state->_controller = require_value(runtime::mapped::SessionController::open_existing(state->_directory));
      runtime::RuntimeAccess access(state);
      return access._snapshot;
    }

    fs::path _root;
  };

  // Opening requires an Active entity and transfers through moves. Plain id()
  // follows Task::claim(): a moved-from or invalidly owned handle throws.
  TEST_F(EndpointRuntimeTest, ActiveOwnedHandlesExposeStableIdentitiesAcrossMoves)
  {
    auto created = create();
    const auto ids = graph(created._creator.registry(), TaskId{1}, "", false);
    expect_error(created._creator.open_writer(ids._queue), ErrorCode::NotActive);
    expect_error(created._creator.open_source_set(ids._source), ErrorCode::NotActive);
    expect_error(created._creator.open_publishing_set(ids._publishing), ErrorCode::NotActive);
    auto registry = created._creator.registry();
    require_value(registry.activate_source_set(ids._source));
    require_value(registry.activate_queue(ids._queue));
    require_value(registry.activate_publishing_set(ids._publishing));
    auto writer = require_value(created._creator.open_writer(ids._queue));
    auto source = require_value(created._creator.open_source_set(ids._source));
    auto publisher = require_value(created._creator.open_publishing_set(ids._publishing));
    auto moved_writer = std::move(writer);
    auto moved_source = std::move(source);
    auto moved_publisher = std::move(publisher);
    EXPECT_THROW(writer.id(), std::logic_error);
    EXPECT_THROW(source.id(), std::logic_error);
    EXPECT_THROW(publisher.id(), std::logic_error);
    EXPECT_EQ(moved_writer.id(), ids._queue);
    EXPECT_EQ(moved_source.id(), ids._source);
    EXPECT_EQ(moved_publisher.id(), ids._publishing);
    expect_error(created._creator.open_source_set(ids._source), ErrorCode::OwnershipBusy);
  }

  // Names are conveniences over Registry lookup, not a different ownership
  // path. Every opening still checks the same active entity and Task claim.
  TEST_F(EndpointRuntimeTest, NamedOpeningsResolveAllEndpointKindsAndPreserveOwnershipErrors)
  {
    auto created = create();
    const auto ids = graph(created._creator.registry());
    auto writer = require_value(created._creator.open_writer("Feed"));
    auto source = require_value(created._creator.open_source_set("Inputs"));
    auto publisher = require_value(created._creator.open_publishing_set("Updates"));
    EXPECT_EQ(writer.id(), ids._queue);
    EXPECT_EQ(source.id(), ids._source);
    EXPECT_EQ(publisher.id(), ids._publishing);
    const auto before = snapshot();
    expect_error(created._creator.open_source_set("Inputs"), ErrorCode::OwnershipBusy);
    expect_error(created._creator.open_writer("Missing"), ErrorCode::NotFound);
    expect_error(created._creator.open_source_set("Missing"), ErrorCode::NotFound);
    expect_error(created._creator.open_publishing_set("Missing"), ErrorCode::NotFound);
    expect_error(created._session.attach_task("Creator"), ErrorCode::OwnershipBusy);
    EXPECT_EQ(snapshot(), before);
  }

  TEST_F(EndpointRuntimeTest, NamedOpeningsPreserveMovedFromAndWrongThreadErrors)
  {
    auto created = create();
    TaskDescriptor descriptor;
    descriptor._name = "WorkerThread";
    descriptor._kind = TaskKind::Thread;
    descriptor._process = TaskId{1};
    const auto owner = require_value(created._creator.registry().register_task(descriptor));
    const auto ids = graph(created._creator.registry(), owner);
    auto task = require_value(created._session.attach_task("WorkerThread"));
    auto moved_task = std::move(task);
    auto moved_session = std::move(created._session);
    const auto before = snapshot();
    expect_error(task.open_writer("Feed"), ErrorCode::InvalidState);
    expect_error(task.open_source_set("Inputs"), ErrorCode::InvalidState);
    expect_error(task.open_publishing_set("Updates"), ErrorCode::InvalidState);
    expect_error(created._session.attach_task("WorkerThread"), ErrorCode::InvalidState);
    std::thread other([&]
    {
      // Lookup itself must reject this thread before endpoint construction.
      expect_error(moved_task.open_writer("Feed"), ErrorCode::WrongOwner);
      expect_error(moved_task.open_source_set("Inputs"), ErrorCode::WrongOwner);
      expect_error(moved_task.open_publishing_set("Updates"), ErrorCode::WrongOwner);
    });
    other.join();
    EXPECT_EQ(snapshot(), before);
    EXPECT_EQ(require_value(moved_task.open_writer("Feed")).id(), ids._queue);
    expect_error(moved_session.attach_task("WorkerThread"), ErrorCode::OwnershipBusy);
  }

  // Handles retain the Task claim independently of the original Task variable.
  // Each endpoint kind alone prevents replacement until its last owner closes.
  TEST_F(EndpointRuntimeTest, EveryEndpointKindRetainsItsTaskClaimUntilClosure)
  {
    for (unsigned kind = 0; kind != 3; ++kind)
    {
      SCOPED_TRACE(kind);
      const auto name = std::to_string(kind);
      auto created = create(name);
      const auto ids = graph(created._creator.registry());
      std::optional<QueueWriter> writer;
      std::optional<SourceQueueSet> source;
      std::optional<PublishingQueueSet> publisher;
      if (kind == 0)
      {
        writer.emplace(require_value(created._creator.open_writer(ids._queue)));
      }
      else if (kind == 1)
      {
        source.emplace(require_value(created._creator.open_source_set(ids._source)));
      }
      else
      {
        publisher.emplace(require_value(created._creator.open_publishing_set(ids._publishing)));
      }
      {
        auto closing = std::move(created._creator);
      }
      expect_error(created._session.attach_task(TaskId{1}), ErrorCode::OwnershipBusy);
      EXPECT_TRUE(require_value(created._session.registry().task_status(TaskId{1}))._attached);
      const auto before = snapshot(name);
      writer.reset();
      source.reset();
      publisher.reset();
      EXPECT_FALSE(require_value(created._session.registry().task_status(TaskId{1}))._attached);
      auto replacement = require_value(created._session.attach_task(TaskId{1}));
      EXPECT_EQ(replacement.claim()._incarnation, 2u);
      EXPECT_EQ(snapshot(name)._tail, before._tail);
      EXPECT_EQ(snapshot(name)._revision, before._revision);
    }
  }

  // Source exclusivity belongs to the logical set/claim. Move assignment closes
  // its old opening before taking the new one; closure changes no persisted data.
  TEST_F(EndpointRuntimeTest, SourceMoveAssignmentReleasesThePreviousExclusiveOpening)
  {
    auto created = create();
    const auto first = graph(created._creator.registry(), TaskId{1}, "First");
    const auto second = graph(created._creator.registry(), TaskId{1}, "Second");
    const auto before = snapshot();
    auto source = require_value(created._creator.open_source_set(first._source));
    auto next = require_value(created._creator.open_source_set(second._source));
    source = std::move(next);
    EXPECT_EQ(source.id(), second._source);
    EXPECT_THROW(next.id(), std::logic_error);
    auto reopened = require_value(created._creator.open_source_set(first._source));
    EXPECT_EQ(reopened.id(), first._source);
    expect_error(created._creator.open_source_set(second._source), ErrorCode::OwnershipBusy);
    EXPECT_EQ(snapshot(), before);
  }

  // Ownership uses typed registered identities. Another Task can discover the
  // topology but cannot open its endpoints; the actual owner may still attach.
  TEST_F(EndpointRuntimeTest, WrongOwnersKindsAndInvalidHandlesRejectWithoutWrites)
  {
    auto created = create();
    TaskDescriptor owner;
    owner._name = "Owner";
    const auto owner_id = require_value(created._creator.registry().register_task(owner));
    const auto ids = graph(created._creator.registry(), owner_id);
    const auto before = snapshot();
    expect_error(created._creator.open_writer(ids._queue), ErrorCode::WrongOwner);
    expect_error(created._creator.open_source_set(ids._source), ErrorCode::WrongOwner);
    expect_error(created._creator.open_publishing_set(ids._publishing), ErrorCode::WrongOwner);
    expect_error(created._creator.open_writer(QueueId::Invalid), ErrorCode::InvalidArgument);
    expect_error(created._creator.open_source_set(static_cast<SourceQueueSetId>(ids._publishing)), ErrorCode::NotFound);
    expect_error(created._creator.open_publishing_set(static_cast<PublishingQueueSetId>(ids._source)), ErrorCode::NotFound);
    EXPECT_EQ(snapshot(), before);
    auto owned = require_value(created._session.attach_task(owner_id));
    auto writer = require_value(owned.open_writer(ids._queue));
    auto source = require_value(owned.open_source_set(ids._source));
    auto publisher = require_value(owned.open_publishing_set(ids._publishing));
    auto moved = std::move(owned);
    expect_error(owned.open_writer(ids._queue), ErrorCode::InvalidState);
    expect_error(owned.open_source_set(ids._source), ErrorCode::InvalidState);
    expect_error(owned.open_publishing_set(ids._publishing), ErrorCode::InvalidState);
    EXPECT_EQ(writer.id(), ids._queue);
    EXPECT_EQ(source.id(), ids._source);
    EXPECT_EQ(publisher.id(), ids._publishing);
  }

  // Process Tasks support application threads. Both contenders remain alive at
  // a barrier so one successful opening cannot close before the other attempts.
  TEST_F(EndpointRuntimeTest, ConcurrentSourceOpensPermitExactlyOneLiveHandle)
  {
    auto created = create();
    const auto ids = graph(created._creator.registry());
    std::latch attempted(2);
    std::latch release(1);
    std::array<bool, 2> accepted{};
    std::array<ErrorCode, 2> errors{};
    const auto run = [&](std::size_t index)
    {
      auto result = created._creator.open_source_set(ids._source);
      if (const auto* error = std::get_if<Error>(&result))
      {
        errors[index] = error->_code;
      }
      else
      {
        accepted[index] = true;
      }
      attempted.count_down();
      release.wait();
    };
    std::thread first(run, 0);
    std::thread second(run, 1);
    attempted.wait();
    EXPECT_NE(accepted[0], accepted[1]);
    EXPECT_EQ(accepted[0] ? errors[1] : errors[0], ErrorCode::OwnershipBusy);
    release.count_down();
    first.join();
    second.join();
    auto reopened = require_value(created._creator.open_source_set(ids._source));
    EXPECT_EQ(reopened.id(), ids._source);
  }

  // Thread-owned handles share their attaching thread's affinity. Failed use on
  // another thread does not invalidate the original handle or its retained parent.
  TEST_F(EndpointRuntimeTest, ThreadEndpointIdentityAndOpeningEnforceThreadAffinity)
  {
    auto created = create();
    TaskDescriptor thread;
    thread._name = "Thread";
    thread._kind = TaskKind::Thread;
    thread._process = TaskId{1};
    const auto owner = require_value(created._creator.registry().register_task(thread));
    const auto ids = graph(created._creator.registry(), owner);
    auto task = require_value(created._session.attach_task(owner));
    auto writer = require_value(task.open_writer(ids._queue));
    auto source = require_value(task.open_source_set(ids._source));
    auto publisher = require_value(task.open_publishing_set(ids._publishing));
    std::thread other([&]
    {
      EXPECT_THROW(writer.id(), std::logic_error);
      EXPECT_THROW(source.id(), std::logic_error);
      EXPECT_THROW(publisher.id(), std::logic_error);
      expect_error(task.open_writer(ids._queue), ErrorCode::WrongOwner);
    });
    other.join();
    EXPECT_EQ(writer.id(), ids._queue);
    EXPECT_EQ(source.id(), ids._source);
    EXPECT_EQ(publisher.id(), ids._publishing);
  }

  // The child only checks forbidden inherited handles and exits. It never opens
  // native Angaria state; inherited Source closure must not release the parent's
  // local opening or acquire a potentially inherited mutex.
  TEST_F(EndpointRuntimeTest, InheritedEndpointUseRejectsWithoutReleasingParentOwnership)
  {
    auto created = create();
    const auto ids = graph(created._creator.registry());
    auto writer = require_value(created._creator.open_writer(ids._queue));
    std::optional<SourceQueueSet> source;
    source.emplace(require_value(created._creator.open_source_set(ids._source)));
    auto publisher = require_value(created._creator.open_publishing_set(ids._publishing));
    const auto child = ::fork();
    ASSERT_NE(child, -1);
    if (child == 0)
    {
      unsigned rejected{};
      try
      {
        writer.id();
      }
      catch (const std::logic_error&)
      {
        ++rejected;
      }
      try
      {
        source->id();
      }
      catch (const std::logic_error&)
      {
        ++rejected;
      }
      try
      {
        publisher.id();
      }
      catch (const std::logic_error&)
      {
        ++rejected;
      }
      source.reset();
      using angaria::test_support::has_error;
      rejected += has_error(created._creator.open_writer("Feed"), ErrorCode::WrongOwner);
      rejected += has_error(created._creator.open_source_set("Inputs"), ErrorCode::WrongOwner);
      rejected += has_error(created._creator.open_publishing_set("Updates"), ErrorCode::WrongOwner);
      rejected += has_error(created._session.attach_task("Creator"), ErrorCode::WrongOwner);
      ::_exit(rejected == 7 ? 0 : 1);
    }
    int status{};
    pid_t waited;
    do
    {
      waited = ::waitpid(child, &status, 0);
    } while (waited == -1 && errno == EINTR);
    ASSERT_EQ(waited, child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(source->id(), ids._source);
    expect_error(created._creator.open_source_set(ids._source), ErrorCode::OwnershipBusy);
  }

  // Capacities 3 and 4 have the same rounded ring allocation. Corrupting only
  // the runtime capacity must still fail against the immutable definition when
  // an already-held Session tries to install a successor Task claim.
  TEST_F(EndpointRuntimeTest, EqualAllocationCapacityCorruptionCannotInstallATaskClaim)
  {
    auto created = create();
    graph(created._creator.registry());
    {
      auto closing = std::move(created._creator);
    }
    auto state = std::make_shared<runtime::SessionState>();
    state->_directory = fs::canonical(_root / "session");
    state->_controller = require_value(runtime::mapped::SessionController::open_existing(state->_directory));
    std::uint64_t capacity_offset{};
    {
      runtime::RuntimeAccess access(state);
      capacity_offset = access.section_offset(runtime::mapped::SectionKind::QueueState) + 24;
    }
    const auto store = [&](std::uint64_t capacity)
    {
      auto file = require_value(runtime::mapped::NativeFile::open_existing(state->_directory / "session.ang",
          runtime::mapped::FileAccess::ReadWrite));
      auto region = require_value(file.map_read_write(capacity_offset, 8));
      require_value(runtime::mapped::AtomicWord::bind(require_value(region.writable_bytes()), 0)).store_release(capacity);
    };
    const auto image = [&]
    {
      runtime::RuntimeAccess access(state);
      return access.read_words(0, static_cast<std::size_t>(access._snapshot._fileBytes / 8));
    };
    store(4);
    const auto evidence = image();
    expect_error(created._session.attach_task(TaskId{1}), ErrorCode::IoError);
    EXPECT_EQ(image(), evidence);
    store(3);
    auto replacement = require_value(created._session.attach_task(TaskId{1}));
    EXPECT_EQ(replacement.claim()._incarnation, 2u);
  }

  // Private journal qualification: ring words are generic mutable targets, but
  // Task-only claim cleanup cannot introduce delivery state in the same intent.
  // Validate the complete command stream before detaching or writing a slot.
  TEST_F(EndpointRuntimeTest, TaskOnlyReleaseCannotIntroduceRingDeliveryState)
  {
    {
      auto created = create();
      graph(created._creator.registry());
    }
    // This is the legacy Task-only cleanup contract. Build its predecessor
    // profile offline, with no publications and every public owner closed.
    // Core delivery cleanup independently rejects the same mixed writes under
    // its stronger replay-discovery validation in the claim-cleanup suite.
    {
      auto file = require_value(runtime::mapped::NativeFile::open_existing(
          _root / "session" / "session.ang", runtime::mapped::FileAccess::ReadWrite));
      const std::array<std::byte, 2> minor{std::byte{1}};
      const std::array<std::byte, 8> features{std::byte{3}};
      require_value(file.write_exact(10, minor));
      require_value(file.write_exact(56, features));
    }
    auto session = require_value(Session::attach(_root / "session"));
    auto creator = require_value(session.attach_task(TaskId{1}));
    const auto claim = creator.claim();
    auto state = std::make_shared<runtime::SessionState>();
    state->_directory = fs::canonical(_root / "session");
    state->_controller = require_value(runtime::mapped::SessionController::open_existing(state->_directory));
    runtime::RuntimeAccess access(state);
    const auto rings = access.record_references(8);
    ASSERT_EQ(rings.size(), 1u);
    runtime::mapped::TransactionPlan plan;
    plan._base = access._snapshot;
    plan._kind = runtime::mapped::JournalKind::FixedState;
    plan._actor = claim._task;
    plan._actorIncarnation = claim._incarnation;
    plan._newRevision = access._snapshot._revision;
    const std::array<std::uint64_t, 2> ending{claim._incarnation,
        std::bit_cast<std::uint64_t>(runtime::runtime_now_ns())};
    auto release = runtime::runtime_write_words(access.section_offset(runtime::mapped::SectionKind::TaskState), ending);
    release._opcode = runtime::mapped::RedoOpcode::ReleaseClaim;
    plan._commands.push_back(std::move(release));
    const std::array<std::uint64_t, 1> sequence{1};
    plan._commands.push_back(runtime::runtime_write_words(rings[0] + runtime::mapped::record_envelope_bytes, sequence));
    const auto evidence = access.read_words(0, static_cast<std::size_t>(access._snapshot._fileBytes / 8));
    expect_error(access._journal.execute(access._guard, plan), ErrorCode::UnsupportedPolicy);
    EXPECT_EQ(access.read_words(0, evidence.size()), evidence);
  }
} // namespace
