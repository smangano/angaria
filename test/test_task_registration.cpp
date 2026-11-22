// Public Task registration examples: a live Task supplies an actor Registry,
// registration creates a logical participant, and attachment later claims it.
// Private gate-bound observations below verify history and no-write guarantees;
// direct counter/slot edits are deliberately labeled fault fixtures.

#include "angaria/runtime.h"
#include "native_atomic.h"
#include "runtime_state.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
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

  TaskDescriptor process_descriptor(std::string name, std::string doc = {})
  {
    TaskDescriptor descriptor;
    descriptor._name = std::move(name);
    descriptor._doc = std::move(doc);
    return descriptor;
  }

  TaskDescriptor thread_descriptor(std::string name, TaskId parent, std::string doc = {})
  {
    auto descriptor = process_descriptor(std::move(name), std::move(doc));
    descriptor._kind = TaskKind::Thread;
    descriptor._process = parent;
    return descriptor;
  }

  struct Observation
  {
    mapped::JournalSnapshot _snapshot;
    std::vector<std::byte> _image;
    std::vector<std::string> _claimFiles;

    bool operator==(const Observation&) const = default;
  };

  class TaskRegistrationTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-task-registry-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
      _directory = _root / "session";
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    SessionOptions options(std::uint32_t capacity = 8, std::uint64_t limit = 1048576,
                           const fs::path& directory = {}) const
    {
      SessionOptions result;
      result._name = "Task registration";
      result._applicationVersion = "Registry examples";
      result._directory = directory.empty() ? _directory : directory;
      result._limits = CapacityLimits::empty().tasks(capacity);
      result._maxLogBytes = limit;
      return result;
    }

    static TaskDescriptor creator()
    {
      return process_descriptor("Creator", "Registers the application participants");
    }

    CreatedSession create(std::uint32_t capacity = 8, std::uint64_t limit = 1048576,
                          const fs::path& directory = {}) const
    {
      return require_value(Session::create(options(capacity, limit, directory), creator()));
    }

    std::shared_ptr<runtime::SessionState> state(const fs::path& directory = {}) const
    {
      auto result = std::make_shared<runtime::SessionState>();
      result->_directory = fs::canonical(directory.empty() ? _directory : directory);
      result->_controller = require_value(mapped::SessionController::open_existing(result->_directory));
      return result;
    }

    // Observe all storage while holding the same gate as public operations.
    // Loading words atomically also avoids copying live journal/readiness words
    // through an ordinary byte read merely to compare before/after images.
    Observation observe(const fs::path& directory = {}) const
    {
      runtime::RuntimeAccess access(state(directory));
      Observation result;
      result._snapshot = access._snapshot;
      const auto words = access.read_words(0, static_cast<std::size_t>(access._snapshot._fileBytes / 8));
      result._image.resize(words.size() * 8);
      for (std::size_t index = 0; index < words.size(); ++index)
      {
        for (unsigned byte = 0; byte < 8; ++byte)
        {
          result._image[index * 8 + byte] = static_cast<std::byte>(words[index] >> (8 * byte));
        }
      }
      for (const auto& entry : fs::directory_iterator(access._session->_directory / "claims"))
      {
        result._claimFiles.push_back(entry.path().filename().string());
      }
      std::sort(result._claimFiles.begin(), result._claimFiles.end());
      return result;
    }

    void set_word(std::uint64_t offset, std::uint64_t value) const
    {
      runtime::RuntimeAccess access(state());
      auto region = require_value(access._file->map_read_write(offset, 8));
      require_value(mapped::AtomicWord::bind(require_value(region.writable_bytes()), 0)).store_release(value);
    }

    fs::path _root;
    fs::path _directory;
  };

  // Start here: the creator already owns a Task and can register the remaining
  // logical participants. Registration records a descriptor but does not attach
  // its process/thread or manufacture PID diagnostics for an offline participant.
  TEST_F(TaskRegistrationTest, RegistersOfflineParticipantsBeforeTheyAttach)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto process = process_descriptor("StrategyHost", "Process hosting strategy threads");
    const auto host = require_value(actor.register_task(process));
    const auto child_descriptor = thread_descriptor("Strategy", host, "Consumes market data");
    const auto child = require_value(actor.register_task(child_descriptor));
    EXPECT_EQ(host, TaskId{2});
    EXPECT_EQ(child, TaskId{3});
    EXPECT_EQ(require_value(created._session.registry().find_task("Strategy")), child);
    const auto description = require_value(actor.describe_task(child));
    EXPECT_EQ(description._name, child_descriptor._name);
    EXPECT_EQ(description._kind, TaskKind::Thread);
    EXPECT_EQ(description._process, host);
    EXPECT_EQ(description._doc, child_descriptor._doc);
    const auto registration = require_value(actor.metadata(child));
    EXPECT_EQ(registration._createdBy, TaskId{1});
    EXPECT_EQ(registration._revision, 3u);
    const auto offline = require_value(actor.task_status(child));
    EXPECT_FALSE(offline._attached);
    EXPECT_EQ(offline._incarnation, 0u);
    EXPECT_EQ(offline._processId, 0u);
    EXPECT_EQ(offline._threadId, 0u);
    EXPECT_EQ(offline._restartCount, 0u);
    EXPECT_FALSE(offline._lastAttachedAt);

    // The separately registered parent must be attached locally before its
    // Thread Task can attach; the creator is not that Thread's parent.
    expect_error(created._session.attach_task(child), ErrorCode::WrongOwner);
    auto host_task = require_value(created._session.attach_task(host));
    auto child_task = require_value(created._session.attach_task(child));
    EXPECT_EQ(child_task.claim()._task, child);
    EXPECT_EQ(child_task.claim()._incarnation, 1u);
    EXPECT_TRUE(require_value(actor.task_status(child))._attached);
  }

  // Exact re-registration returns the existing identity, including the creator
  // bootstrapped by Session::create. It consumes neither a revision/generation,
  // more log bytes, nor another coordination file, and preserves attribution.
  TEST_F(TaskRegistrationTest, ExactRegistrationIsAnUnchangedLookupIncludingTheCreator)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto descriptor = process_descriptor("Risk", "Checks order limits");
    const auto id = require_value(actor.register_task(descriptor));
    const auto registration = require_value(actor.metadata(id));
    const auto before = observe();
    EXPECT_EQ(require_value(actor.register_task(creator())), TaskId{1});
    EXPECT_EQ(require_value(actor.register_task(descriptor)), id);
    EXPECT_EQ(observe(), before);
    const auto unchanged = require_value(actor.metadata(id));
    EXPECT_EQ(unchanged._createdBy, registration._createdBy);
    EXPECT_EQ(unchanged._createdAt, registration._createdAt);
    EXPECT_EQ(unchanged._revision, registration._revision);
  }

  // A name identifies one immutable Task descriptor. Changing its documentation,
  // kind, or parent is a conflict rather than an implicit edit or a new identity;
  // applications must choose a different name for a different participant.
  TEST_F(TaskRegistrationTest, ConflictingDescriptorsPreserveTheExistingRegistration)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto process = process_descriptor("Worker", "Original documentation");
    const auto process_id = require_value(actor.register_task(process));
    const auto child = thread_descriptor("Child", TaskId{1}, "Original child");
    const auto child_id = require_value(actor.register_task(child));
    auto changed_doc = process;
    changed_doc._doc += " changed";
    const auto changed_kind = thread_descriptor("Worker", TaskId{1}, process._doc);
    auto changed_parent = child;
    changed_parent._process = process_id;
    auto changed_child_doc = child;
    changed_child_doc._doc += " changed";
    const auto before = observe();
    for (const auto& conflict : {changed_doc, changed_kind, changed_parent, changed_child_doc})
    {
      expect_error(actor.register_task(conflict), ErrorCode::Conflict);
      EXPECT_EQ(observe(), before);
    }
    EXPECT_EQ(require_value(actor.find_task("Worker")), process_id);
    EXPECT_EQ(require_value(actor.find_task("Child")), child_id);
    EXPECT_EQ(require_value(actor.describe_task(child_id))._process, child._process);
  }

  // Names are case-sensitive UTF-8 values rather than legacy fixed-size labels.
  // Documentation is length delimited and may contain NUL bytes. Both consume
  // arena history, independently of the disabled application string pool.
  TEST_F(TaskRegistrationTest, PreservesCaseLongUtf8NamesAndLengthDelimitedDocumentation)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto lower = require_value(actor.register_task(process_descriptor("worker")));
    const auto upper = require_value(actor.register_task(process_descriptor("Worker")));
    EXPECT_NE(lower, upper);
    auto descriptor = process_descriptor(std::string(256, 'n') + "\xf0\x9f\x93\x88",
                                         std::string(4096, 'd') + std::string{"a\0b", 3});
    const auto id = require_value(actor.register_task(descriptor));
    const auto read = require_value(actor.describe_task(id));
    EXPECT_EQ(read._name, descriptor._name);
    EXPECT_EQ(read._doc, descriptor._doc);
    EXPECT_EQ(require_value(actor.find_task(descriptor._name)), id);
    EXPECT_EQ(require_value(actor.register_task(descriptor)), id);
    EXPECT_EQ(require_value(created._session.options())._limits._stringPoolBytes, 0u);
  }

  // Validate descriptors and registered parent kinds before reserving any ID,
  // claim file, or history. A Thread may name an offline Process, but never an
  // unknown Task or another Thread; it cannot omit its parent or use zero.
  TEST_F(TaskRegistrationTest, InvalidDescriptorsAndParentReferencesLeaveNoNewResources)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto child = require_value(actor.register_task(thread_descriptor("ExistingThread", TaskId{1})));
    std::vector<TaskDescriptor> invalid;
    for (const auto& name : {std::string{}, std::string{"bad\0name", 8}, std::string{"\xc0\xaf", 2}})
    {
      invalid.push_back(process_descriptor(name));
    }
    invalid.push_back(process_descriptor("BadDocumentation", std::string{"\xff", 1}));
    auto bad_kind = process_descriptor("UnknownKind");
    bad_kind._kind = static_cast<TaskKind>(99);
    invalid.push_back(bad_kind);
    auto process_parent = process_descriptor("ProcessWithParent");
    process_parent._process = TaskId{1};
    invalid.push_back(process_parent);
    auto missing_parent = process_descriptor("MissingParent");
    missing_parent._kind = TaskKind::Thread;
    invalid.push_back(missing_parent);
    invalid.push_back(thread_descriptor("ZeroParent", TaskId::Invalid));
    invalid.push_back(thread_descriptor("ThreadAsParent", child));
    const auto before = observe();
    for (const auto& descriptor : invalid)
    {
      SCOPED_TRACE(descriptor._name);
      expect_error(actor.register_task(descriptor), ErrorCode::InvalidArgument);
      EXPECT_EQ(observe(), before);
    }
    expect_error(actor.register_task(thread_descriptor("UnknownParent", TaskId{7})), ErrorCode::NotFound);
    EXPECT_EQ(observe(), before);
    EXPECT_EQ(require_value(actor.register_task(process_descriptor("NextValid"))), TaskId{3});
  }

  // Mutation requires an actor-bound Registry, not merely a Session inspection
  // view. A retained actor view remains usable after its Task wrapper closes;
  // releasing that last view ends the claim while registrations remain intact.
  TEST_F(TaskRegistrationTest, ReadOnlyViewsCannotMutateAndActorViewsRetainTheirClaim)
  {
    auto created = create();
    auto actor = created._creator.registry();
    auto reader = created._session.registry();
    const auto before = observe();
    expect_error(reader.register_task(process_descriptor("Forbidden")), ErrorCode::ReadOnlyRegistry);
    expect_error(Registry{}.register_task(process_descriptor("Unbound")), ErrorCode::InvalidState);
    EXPECT_EQ(observe(), before);
    {
      auto closing = std::move(created._creator);
    }
    const auto id = require_value(actor.register_task(process_descriptor("RetainedActor")));
    EXPECT_EQ(require_value(reader.metadata(id))._createdBy, TaskId{1});
    expect_error(created._session.attach_task(TaskId{1}), ErrorCode::OwnershipBusy);
    actor = Registry{};
    auto replacement = require_value(created._session.attach_task(TaskId{1}));
    EXPECT_EQ(replacement.claim()._incarnation, 2u);
    EXPECT_EQ(require_value(reader.find_task("RetainedActor")), id);
  }

  // A Thread's Registry is an actor only on its attaching thread. It also
  // retains the parent Process claim, so closing the original wrappers cannot
  // change registration attribution or permit premature parent replacement.
  TEST_F(TaskRegistrationTest, ThreadActorsEnforceAffinityAndRetainTheirParent)
  {
    auto created = create();
    const auto thread_id = require_value(created._creator.registry().register_task(
        thread_descriptor("RegistrarThread", TaskId{1})));
    std::optional<Task> thread_task(require_value(created._session.attach_task(thread_id)));
    auto actor = thread_task->registry();
    const auto before = observe();
    std::optional<Result<TaskId>> wrong_thread;
    std::thread caller([&]
    {
      wrong_thread.emplace(actor.register_task(process_descriptor("WrongThread")));
    });
    caller.join();
    ASSERT_TRUE(wrong_thread);
    expect_error(*wrong_thread, ErrorCode::WrongOwner);
    EXPECT_EQ(observe(), before);
    {
      auto closing = std::move(created._creator);
    }
    thread_task.reset();
    const auto registered = require_value(actor.register_task(process_descriptor("FromThread")));
    EXPECT_EQ(require_value(created._session.registry().metadata(registered))._createdBy, thread_id);
    expect_error(created._session.attach_task(TaskId{1}), ErrorCode::OwnershipBusy);
    actor = Registry{};
    auto parent = require_value(created._session.attach_task(TaskId{1}));
    EXPECT_EQ(parent.claim()._incarnation, 2u);
  }

  // Registration shares the mutation gate across coordinated application
  // threads. Identical requests create one identity; distinct names receive
  // different IDs without racing to publish the same index slot or revision.
  TEST_F(TaskRegistrationTest, CoordinatedThreadsSerializeIdenticalAndDistinctRequests)
  {
    for (const auto identical : {false, true})
    {
      SCOPED_TRACE(identical);
      const auto directory = _root / (identical ? "identical" : "distinct");
      auto created = create(8, 1048576, directory);
      auto actor = created._creator.registry();
      const auto before = observe(directory);
      std::barrier start(3);
      std::array<Result<TaskId>, 2> results;
      std::array<std::thread, 2> callers;
      for (std::size_t index = 0; index != callers.size(); ++index)
      {
        callers[index] = std::thread([&, index]
        {
          start.arrive_and_wait();
          results[index] = actor.register_task(process_descriptor(
              identical || index == 0 ? "First" : "Second", "Thread request"));
        });
      }
      start.arrive_and_wait();
      for (auto& caller : callers)
      {
        caller.join();
      }
      const auto first = require_value(results[0]);
      const auto second = require_value(results[1]);
      const auto after = observe(directory);
      EXPECT_EQ(after._snapshot._revision, before._snapshot._revision + (identical ? 1 : 2));
      EXPECT_EQ(after._snapshot._generation, before._snapshot._generation + (identical ? 1 : 2));
      if (identical)
      {
        EXPECT_EQ(first, second);
      }
      else
      {
        EXPECT_NE(first, second);
        const std::array ids{first, second};
        EXPECT_NE(std::find(ids.begin(), ids.end(), TaskId{2}), ids.end());
        EXPECT_NE(std::find(ids.begin(), ids.end(), TaskId{3}), ids.end());
      }
    }
  }

  // A full Task table rejects only new identities. Exact re-registration and
  // conflict diagnosis still use the committed descriptors, and failed attempts
  // must not allocate an extra claim file or consume any operation generation.
  TEST_F(TaskRegistrationTest, CapacityExhaustionPreservesStateAndAllowsExistingIdentities)
  {
    auto created = create(2);
    auto actor = created._creator.registry();
    const auto descriptor = process_descriptor("LastSlot", "Original");
    const auto id = require_value(actor.register_task(descriptor));
    const auto before = observe();
    EXPECT_EQ(require_value(actor.register_task(creator())), TaskId{1});
    EXPECT_EQ(require_value(actor.register_task(descriptor)), id);
    expect_error(actor.register_task(process_descriptor("Overflow")), ErrorCode::CapacityExceeded);
    expect_error(actor.register_task(process_descriptor("LastSlot", "Changed")), ErrorCode::Conflict);
    EXPECT_EQ(observe(), before);
    EXPECT_FALSE(fs::exists(_directory / "claims/task-3.lock"));
  }

  // Recreate at the exact measured bootstrap size so no Append can fit. The
  // creator's identical registration is still a lookup; a new Task reports
  // LogFull before creating even its stable claim coordination file.
  TEST_F(TaskRegistrationTest, LogFullRejectsNewRegistrationBeforeCoordinationSetup)
  {
    std::uint64_t required{};
    {
      auto created = create();
      required = fs::file_size(_directory / "session.ang");
    }
    fs::remove_all(_directory);
    auto created = create(8, required);
    auto actor = created._creator.registry();
    const auto before = observe();
    EXPECT_EQ(require_value(actor.register_task(creator())), TaskId{1});
    expect_error(actor.register_task(process_descriptor("CannotFit")), ErrorCode::LogFull);
    EXPECT_EQ(observe(), before);
    EXPECT_FALSE(fs::exists(_directory / "claims/task-2.lock"));
  }

  // Measure a real complete registration rather than duplicating format math.
  // One byte less must reject before creating its claim file, catching budgets
  // that count the Task body but omit the manifest or alignment. The exact byte
  // ceiling must succeed and still permit an unchanged idempotent retry.
  TEST_F(TaskRegistrationTest, CompleteAppendBudgetIncludesManifestAndPaddingAtTheExactLimit)
  {
    const auto descriptor = process_descriptor("BoundaryTask", "Includes its complete append manifest");
    std::uint64_t required{};
    {
      auto created = create();
      EXPECT_EQ(require_value(created._creator.registry().register_task(descriptor)), TaskId{2});
      required = fs::file_size(_directory / "session.ang");
    }
    fs::remove_all(_directory);
    {
      auto created = create(8, required - 1);
      auto actor = created._creator.registry();
      const auto before = observe();
      expect_error(actor.register_task(descriptor), ErrorCode::LogFull);
      EXPECT_EQ(observe(), before);
      EXPECT_FALSE(fs::exists(_directory / "claims/task-2.lock"));
    }
    fs::remove_all(_directory);
    {
      auto created = create(8, required);
      auto actor = created._creator.registry();
      EXPECT_EQ(require_value(actor.register_task(descriptor)), TaskId{2});
      EXPECT_EQ(fs::file_size(_directory / "session.ang"), required);
      const auto accepted = observe();
      EXPECT_EQ(accepted._snapshot._tail, required);
      EXPECT_EQ(accepted._snapshot._accounted, required);
      EXPECT_EQ(require_value(actor.register_task(descriptor)), TaskId{2});
      EXPECT_EQ(observe(), accepted);
    }
  }

  // This private fixture advances only the completed operation generation to
  // its maximum, representing exhausted FixedState history without corrupting
  // Registry revision chains. An exact registration must remain idempotent.
  TEST_F(TaskRegistrationTest, ExhaustedGenerationRejectsNewWorkWithoutBlockingExactRegistration)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto descriptor = process_descriptor("Existing");
    const auto id = require_value(actor.register_task(descriptor));
    const auto generation = observe()._snapshot._generation;
    set_word(120, std::numeric_limits<std::uint64_t>::max());
    const auto before = observe();
    EXPECT_EQ(require_value(actor.register_task(descriptor)), id);
    expect_error(actor.register_task(process_descriptor("NoGeneration")), ErrorCode::CapacityExceeded);
    EXPECT_EQ(observe(), before);
    EXPECT_FALSE(fs::exists(_directory / "claims/task-3.lock"));
    set_word(120, generation);
    EXPECT_EQ(require_value(actor.register_task(process_descriptor("AfterRestoration"))), TaskId{3});
  }

  // Keep old immutable definitions and Append manifests while adding new Tasks.
  // Registry revision describes registration history, not the actor's attachment
  // count; closing/replacing an actor does not rewrite registered metadata.
  TEST_F(TaskRegistrationTest, NewRevisionsRetainDefinitionsAndIdentityAcrossActorRestart)
  {
    auto created = create();
    const auto alpha = process_descriptor("Alpha", "First registered participant");
    const auto alpha_id = require_value(created._creator.registry().register_task(alpha));
    const auto alpha_metadata = require_value(created._session.registry().metadata(alpha_id));
    std::uint64_t reference{};
    std::uint64_t manifest{};
    std::vector<std::byte> original;
    {
      runtime::RuntimeAccess access(state());
      reference = runtime::runtime_task(access, alpha_id)._reference;
      manifest = access._snapshot._manifest;
      original = access.immutable_record(reference, 2);
    }
    {
      auto closing = std::move(created._creator);
    }
    auto actor = require_value(created._session.attach_task(TaskId{1}));
    const auto acting_claim = actor.claim();
    const auto beta_id = require_value(actor.registry().register_task(process_descriptor("Beta")));
    EXPECT_EQ(beta_id, TaskId{3});
    EXPECT_EQ(require_value(actor.registry().register_task(alpha)), alpha_id);
    const auto unchanged = require_value(actor.registry().metadata(alpha_id));
    EXPECT_EQ(unchanged._createdBy, alpha_metadata._createdBy);
    EXPECT_EQ(unchanged._createdAt, alpha_metadata._createdAt);
    EXPECT_EQ(unchanged._revision, alpha_metadata._revision);
    {
      runtime::RuntimeAccess access(state());
      EXPECT_EQ(runtime::runtime_task(access, alpha_id)._reference, reference);
      EXPECT_EQ(access.immutable_record(reference, 2), original);
      EXPECT_EQ(access.record_envelope(reference, 2)._previous, 0u);
      EXPECT_EQ(access.record_envelope(access._snapshot._manifest, 11)._previous, manifest);
      const auto batch = require_value(mapped::decode_manifest_body(
          access.immutable_record(access._snapshot._manifest, 11), access._snapshot._manifest));
      EXPECT_EQ(batch._baseRevision, 2u);
      EXPECT_EQ(batch._newRevision, 3u);
      EXPECT_EQ(batch._actor, TaskId{1});
      EXPECT_EQ(batch._actorIncarnation, acting_claim._incarnation);
      ASSERT_EQ(batch._records.size(), 1u);
      EXPECT_EQ(batch._records[0], runtime::runtime_task(access, beta_id)._reference);
    }
    auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.registry().find_task("Alpha")), alpha_id);
  }

  // Import one committed sparse identity into a different allocation slot to
  // exercise file geometry that fresh contiguous registrations would hide.
  // The private setup is not an application registration recipe: public calls
  // must fill the next free slot while allocating the lowest unused logical ID.
  TEST_F(TaskRegistrationTest, SparseIdentitiesDoNotTurnIdsIntoAllocationSlotNumbers)
  {
    auto created = create();
    const auto actor_claim = created._creator.claim();
    std::uint64_t sparse_reference{};
    std::vector<std::byte> sparse_body;
    {
      runtime::RuntimeAccess access(state());
      mapped::TaskDefinition definition;
      definition._id = TaskId{5};
      definition._descriptor = process_descriptor("ImportedSparse", "Existing sparse participant");
      definition._registration = RegistrationMetadata{TaskId{1}, UtcTimestamp{std::chrono::nanoseconds{123}},
                                                      access._snapshot._revision + 1};
      sparse_reference = access._snapshot._tail;
      mapped::JournalRecord record;
      record._kind = 2;
      record._immutable = require_value(mapped::encode_task_definition(definition, sparse_reference));
      mapped::TransactionPlan plan;
      plan._base = access._snapshot;
      plan._kind = mapped::JournalKind::Append;
      plan._actor = actor_claim._task;
      plan._actorIncarnation = actor_claim._incarnation;
      plan._newRevision = definition._registration._revision;
      plan._records.push_back(std::move(record));
      const std::array<std::uint64_t, 4> row{5, sparse_reference, plan._newRevision, 0};
      plan._commands.push_back(runtime::runtime_write_words(
          access.section_offset(mapped::SectionKind::TaskIndex) + mapped::index_entry_bytes, row));
      require_value(access._session->_controller->create_task_claim_file(access._guard, TaskId{5}));
      require_value(access._journal.execute(access._guard, plan));
    }
    {
      runtime::RuntimeAccess access(state());
      sparse_body = access.immutable_record(sparse_reference, 2);
    }
    const auto id = require_value(created._creator.registry().register_task(process_descriptor("FillGap")));
    EXPECT_EQ(id, TaskId{2});
    {
      runtime::RuntimeAccess access(state());
      const auto states = access.section_offset(mapped::SectionKind::TaskState);
      const auto sparse = runtime::runtime_task(access, TaskId{5});
      const auto added = runtime::runtime_task(access, id);
      EXPECT_EQ(sparse._stateOffset, states + mapped::task_state_bytes);
      EXPECT_EQ(added._stateOffset, states + 2 * mapped::task_state_bytes);
      EXPECT_EQ(sparse._reference, sparse_reference);
      EXPECT_EQ(access.immutable_record(sparse_reference, 2), sparse_body);
      const auto untouched = access.read_words(sparse._stateOffset, 32);
      EXPECT_TRUE(std::all_of(untouched.begin(), untouched.end(), [](auto value)
      {
        return value == 0;
      }));
    }
    auto attached = require_value(created._session.attach_task(id));
    EXPECT_EQ(attached.claim()._incarnation, 1u);
    EXPECT_EQ(require_value(created._session.registry().task_status(TaskId{5}))._incarnation, 0u);
    EXPECT_EQ(require_value(created._session.registry().find_task("ImportedSparse")), TaskId{5});
  }

  // A bad coordination path cannot result in a committed unattachable Task.
  // After the test removes its deliberate obstruction, a preexisting ordinary
  // claim file is validated and reused without replacing its stable identity.
  TEST_F(TaskRegistrationTest, CoordinationFailureDoesNotPublishAndRetryKeepsAStableFile)
  {
    auto created = create();
    auto actor = created._creator.registry();
    const auto path = _directory / "claims/task-2.lock";
    fs::create_directory(path);
    const auto before = observe();
    const auto descriptor = process_descriptor("Retryable");
    expect_error(actor.register_task(descriptor), ErrorCode::IoError);
    EXPECT_EQ(observe(), before);
    expect_error(actor.find_task(descriptor._name), ErrorCode::NotFound);
    fs::remove(path); // Remove only this test's artificial directory obstruction.
    auto file = require_value(mapped::NativeFile::create_exclusive(path));
    const auto identity = require_value(file.identity());
    const auto id = require_value(actor.register_task(descriptor));
    EXPECT_EQ(id, TaskId{2});
    auto reopened = require_value(mapped::NativeFile::open_existing(path, mapped::FileAccess::ReadWrite));
    EXPECT_EQ(require_value(reopened.identity()), identity);
    auto task = require_value(created._session.attach_task(id));
    EXPECT_EQ(task.claim()._incarnation, 1u);
  }
} // namespace
