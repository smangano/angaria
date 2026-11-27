// Private lookup and read-window qualification beneath Session/Registry
// inspection. These fixtures append definitions directly to control batch
// geometry and exercise storage independently of public registration; usage
// examples live in test_task_registration.cpp.

#include "native_atomic.h"
#include "result_assertions.h"
#include "runtime_state.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;
  namespace fs = std::filesystem;

  using angaria::test_support::expect_error;
  using angaria::test_support::require_value;

  constexpr std::uint64_t window_bytes = 64 * 1024;

  template <typename Operation>
  void expect_io_error(Operation operation)
  {
    try
    {
      operation();
      FAIL() << "Expected the invalid metadata read to fail";
    }
    catch (const runtime::RuntimeFailure& failure)
    {
      EXPECT_EQ(failure._error._code, ErrorCode::IoError);
    }
  }

  class RuntimeAccessTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-boundaries-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
      SessionOptions options;
      options._name = "Boundary lookups";
      options._directory = _root / "session";
      options._maxLogBytes = 2097152;
      options._limits = CapacityLimits::empty().tasks(65);
      TaskDescriptor creator;
      creator._name = "Creator";
      _created.emplace(require_value(Session::create(options, creator)));
      _state = std::make_shared<runtime::SessionState>();
      _state->_directory = fs::canonical(options._directory);
      _state->_controller = require_value(mapped::SessionController::open_existing(_state->_directory));
      runtime::RuntimeAccess access(_state);
      _references.push_back(access.read_words(access.section_offset(mapped::SectionKind::TaskIndex) + 8, 1)[0]);
    }

    void TearDown() override
    {
      _state.reset();
      _created.reset();
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    mapped::TransactionPlan append_plan(runtime::RuntimeAccess& access) const
    {
      mapped::TransactionPlan plan;
      plan._base = access._snapshot;
      plan._kind = mapped::JournalKind::Append;
      plan._actor = TaskId{1};
      plan._actorIncarnation = access.read_words(access.section_offset(mapped::SectionKind::TaskState), 1)[0];
      plan._newRevision = access._snapshot._revision + 1;
      return plan;
    }

    void append_threads(unsigned count)
    {
      runtime::RuntimeAccess access(_state);
      auto plan = append_plan(access);
      auto offset = access._snapshot._tail;
      const auto index = access.section_offset(mapped::SectionKind::TaskIndex);
      for (unsigned thread = 0; thread < count; ++thread)
      {
        const auto id = static_cast<std::uint16_t>(_references.size() + thread + 1);
        mapped::TaskDefinition definition;
        definition._id = static_cast<TaskId>(id);
        definition._descriptor._name = "Thread" + std::to_string(id);
        definition._descriptor._kind = TaskKind::Thread;
        definition._descriptor._process = TaskId{1};
        definition._descriptor._doc = std::string(257, 'd');
        definition._registration = RegistrationMetadata{TaskId{1}, UtcTimestamp{std::chrono::nanoseconds{123}},
                                                        plan._newRevision};
        mapped::JournalRecord record;
        record._kind = 2;
        record._immutable = require_value(mapped::encode_task_definition(definition, offset));
        const std::array<std::uint64_t, 4> row{id, offset, plan._newRevision, 0};
        plan._commands.push_back(runtime::runtime_write_words(index + (id - 1) * mapped::index_entry_bytes, row));
        offset += *mapped::checked_align(64 + *mapped::checked_align(record._immutable.size(), 8), 64);
        plan._records.push_back(std::move(record));
      }
      const auto result = require_value(access._journal.execute(access._guard, plan));
      _references.insert(_references.end(), result._records.begin(), result._records.end());
    }

    // Only framing/lookup qualification uses this synthetic ring. Its end is
    // deliberately 4 KiB into a window so the final manifest and a mutable word
    // share the recovered file's last partial window.
    mapped::TransactionResult append_window_geometry(const std::string& document)
    {
      runtime::RuntimeAccess access(_state);
      auto plan = append_plan(access);
      const auto reference = access._snapshot._tail;
      mapped::TaskDefinition definition;
      definition._id = TaskId{2};
      definition._descriptor._name = "LargeThread";
      definition._descriptor._kind = TaskKind::Thread;
      definition._descriptor._process = TaskId{1};
      definition._descriptor._doc = document;
      definition._registration = RegistrationMetadata{TaskId{1}, UtcTimestamp{std::chrono::nanoseconds{123}},
                                                      plan._newRevision};
      mapped::JournalRecord task;
      task._kind = 2;
      task._immutable = require_value(mapped::encode_task_definition(definition, reference));
      const auto taskBytes = *mapped::checked_align(mapped::record_envelope_bytes +
          *mapped::checked_align(task._immutable.size(), 8), mapped::section_alignment);
      const auto ringBody = reference + taskBytes + mapped::record_envelope_bytes;
      mapped::JournalRecord ring;
      ring._kind = 8;
      ring._mutable.resize(2 * window_bytes + 4096 - ringBody % window_bytes);
      plan._records.push_back(std::move(task));
      plan._records.push_back(std::move(ring));
      const std::array<std::uint64_t, 4> row{2, reference, plan._newRevision, 0};
      plan._commands.push_back(runtime::runtime_write_words(
          access.section_offset(mapped::SectionKind::TaskIndex) + mapped::index_entry_bytes, row));
      auto result = require_value(access._journal.execute(access._guard, plan));
      _references.push_back(result._records.front());
      return result;
    }

    std::string window_document() const
    {
      std::string result(2 * window_bytes + 257, 'a');
      for (std::size_t index = 0; index != result.size(); ++index)
      {
        result[index] = static_cast<char>('a' + index % 23);
      }
      return result;
    }

    std::uint64_t append_ring_with_interior_envelope()
    {
      runtime::RuntimeAccess access(_state);
      auto plan = append_plan(access);
      const auto interior = access._snapshot._tail + mapped::record_envelope_bytes;
      mapped::RecordEnvelope envelope;
      envelope._kind = 2;
      envelope._totalBytes = 192;
      envelope._immutableBytes = 128;
      envelope._generation = access._snapshot._generation + 1;
      envelope._registryRevision = plan._newRevision;
      envelope._readiness = 2;
      const auto encoded = require_value(mapped::encode_record_envelope(envelope));
      mapped::TaskDefinition definition;
      definition._id = TaskId{65};
      definition._descriptor._name = "Forged";
      definition._registration = RegistrationMetadata{TaskId{1}, UtcTimestamp{}, plan._newRevision};
      const auto body = require_value(mapped::encode_task_definition(definition, interior));
      mapped::JournalRecord ring;
      ring._kind = 8;
      ring._mutable.resize(512);
      std::copy(encoded.begin(), encoded.end(), ring._mutable.begin());
      std::copy(body.begin(), body.end(), ring._mutable.begin() + mapped::record_envelope_bytes);
      plan._records.push_back(std::move(ring));
      const std::array ready{runtime::session_ready_flag};
      plan._commands.push_back(runtime::runtime_write_words(112, ready));
      require_value(access._journal.execute(access._guard, plan));
      return interior;
    }

    fs::path _root;
    std::optional<CreatedSession> _created;
    std::shared_ptr<runtime::SessionState> _state;
    std::vector<std::uint64_t> _references;
  };

  // One operation can inspect many Task definitions and validate their shared
  // Process parent. The same owning envelope entries survive repeated forward,
  // reverse, and complete-table lookups; no timing threshold is needed to verify reuse.
  TEST_F(RuntimeAccessTest, DefinitionLookupsReuseOneGateBoundEnvelopeIndex)
  {
    append_threads(48);
    runtime::RuntimeAccess access(_state);
    std::vector<const mapped::RecordEnvelope*> envelopes;
    for (const auto reference : _references)
    {
      envelopes.push_back(&access.record_envelope(reference, 2));
    }
    for (unsigned pass = 0; pass < 3; ++pass)
    {
      const auto tasks = runtime::runtime_tasks(access);
      ASSERT_EQ(tasks.size(), _references.size());
      for (std::size_t remaining = _references.size(); remaining != 0; --remaining)
      {
        const auto index = remaining - 1;
        const auto definition = require_value(mapped::decode_task_definition(
            access.immutable_record(_references[index], 2), _references[index]));
        EXPECT_EQ(definition._id, static_cast<TaskId>(index + 1));
        EXPECT_EQ(definition._registration._revision, envelopes[index]->_registryRevision);
        EXPECT_EQ(&access.record_envelope(_references[index], 2), envelopes[index]);
      }
    }
  }

  // A plausible aligned header inside another record is still not a record
  // boundary. This synthetic ring carries a complete fake Task envelope/body;
  // neither its apparent kind nor readiness can authorize following that address.
  TEST_F(RuntimeAccessTest, IndexedLookupRejectsAlignedInteriorAndWrongKindReferences)
  {
    const auto interior = append_ring_with_interior_envelope();
    runtime::RuntimeAccess access(_state);
    expect_io_error([&]
    {
      static_cast<void>(access.immutable_record(interior, 2));
    });
    expect_io_error([&]
    {
      static_cast<void>(access.immutable_record(_references[0], 1));
    });
    for (const auto reference : {std::uint64_t{0}, _references[0] + 1, access._snapshot._tail})
    {
      expect_io_error([&]
      {
        static_cast<void>(access.immutable_record(reference, 2));
      });
    }
    EXPECT_EQ(require_value(mapped::decode_task_definition(
                  access.immutable_record(_references[0], 2), _references[0]))._id, TaskId{1});
  }

  // A finite receive first obtains a nonblocking guard. Adopting it must not
  // recursively acquire the same gate, and still provides recovered lookups.
  TEST_F(RuntimeAccessTest, AdoptsATryAcquiredGuardWithoutReenteringTheGate)
  {
    auto guard = require_value(_state->_controller->try_acquire_mutation());
    runtime::RuntimeAccess access(_state, std::move(guard));
    EXPECT_EQ(runtime::runtime_task(access, TaskId{1})._reference, _references.front());
    const auto busy = _state->_controller->try_acquire_mutation();
    ASSERT_TRUE(std::holds_alternative<Error>(busy));
    EXPECT_EQ(std::get<Error>(busy)._code, ErrorCode::OwnershipBusy);
  }

  TEST_F(RuntimeAccessTest, RejectsAGuardFromAnotherSessionBeforeReadingItsJournal)
  {
    SessionOptions options;
    options._name = "Other Session";
    options._directory = _root / "other";
    options._maxLogBytes = 1048576;
    options._limits = CapacityLimits::empty().tasks(1);
    TaskDescriptor creator;
    creator._name = "OtherCreator";
    auto other = require_value(Session::create(options, creator));
    auto controller = require_value(mapped::SessionController::open_existing(options._directory));
    auto guard = require_value(controller->try_acquire_mutation());
    try
    {
      runtime::RuntimeAccess access(_state, std::move(guard));
      FAIL() << "A guard from another Session must not authorize access";
    }
    catch (const runtime::RuntimeFailure& failure)
    {
      EXPECT_EQ(failure._error._code, ErrorCode::WrongOwner);
    }
  }

  // Cache scope follows the recovered operation, not the process or controller.
  // After an Append completes and its gate owner closes, the next access must
  // index the new definition while preserving the older committed references.
  TEST_F(RuntimeAccessTest, ANewAccessIndexesDefinitionsAddedAfterThePreviousSnapshot)
  {
    mapped::RecordEnvelope original;
    {
      runtime::RuntimeAccess access(_state);
      original = access.record_envelope(_references[0], 2);
    }
    append_threads(1);
    runtime::RuntimeAccess access(_state);
    EXPECT_EQ(access.record_envelope(_references[0], 2), original);
    ASSERT_EQ(_references.size(), 2u);
    const auto definition = require_value(mapped::decode_task_definition(
        access.immutable_record(_references[1], 2), _references[1]));
    EXPECT_EQ(definition._id, TaskId{2});
    EXPECT_EQ(runtime::runtime_task(access, TaskId{2})._reference, _references[1]);
  }
  // Runtime read windows are private storage machinery. A large, valid Task
  // body crosses several windows; its returned copy survives eviction and the
  // access/guard lifetime, just like the small owning definition copies.
  TEST_F(RuntimeAccessTest, ImmutableCopiesCrossWindowsAndOutliveTheirOperation)
  {
    const auto document = window_document();
    const auto accepted = append_window_geometry(document);
    const auto reference = accepted._records.front();
    std::vector<std::byte> retained;
    {
      runtime::RuntimeAccess access(_state);
      const auto& envelope = access.record_envelope(reference, 2);
      ASSERT_GT(envelope._immutableBytes, 2 * window_bytes);
      retained = access.immutable_record(reference, 2);
      EXPECT_EQ(retained.size(), envelope._immutableBytes);
      EXPECT_EQ(require_value(mapped::decode_task_definition(retained, reference))._descriptor._doc, document);

      // Visit older and newer windows after obtaining the copy. No returned
      // vector may borrow the reader's currently retained mapping.
      static_cast<void>(access.immutable_record(_references.front(), 2));
      static_cast<void>(access.immutable_record(accepted._manifest, 11));
      EXPECT_EQ(access.immutable_record(reference, 2), retained);

      // Registry factories may move an access with both windows populated.
      // Ownership must follow the guard without leaving dangling cached spans.
      auto moved = std::move(access);
      EXPECT_EQ(moved.immutable_record(reference, 2), retained);
      EXPECT_EQ(moved.read_words(112, 1)[0], runtime::session_ready_flag);
    }
    const auto decoded = require_value(mapped::decode_task_definition(retained, reference));
    EXPECT_EQ(decoded._id, TaskId{2});
    EXPECT_EQ(decoded._descriptor._doc, document);
  }

  // A cached mapping must not turn mutable words into cached values. Cross an
  // exact 64 KiB boundary, publish through the native atomic adapter, and read
  // again while the same operation still owns the gate.
  TEST_F(RuntimeAccessTest, ReusedWordWindowsObserveAtomicChangesAndEnforceSnapshotBounds)
  {
    const auto accepted = append_window_geometry(window_document());
    runtime::RuntimeAccess access(_state);
    const auto ringBody = accepted._records.at(1) + mapped::record_envelope_bytes;
    const auto boundary = (ringBody / window_bytes + 1) * window_bytes;
    const auto offset = boundary - 8;
    auto writer = require_value(access._file->map_read_write(offset, 24));
    auto bytes = require_value(writer.writable_bytes());
    const std::array<std::uint64_t, 3> first{11, 22, 33};
    for (std::size_t index = 0; index != first.size(); ++index)
    {
      require_value(mapped::AtomicWord::bind(bytes, index * 8)).store_release(first[index]);
    }
    const auto retained = access.read_words(offset, first.size());
    EXPECT_EQ(retained, (std::vector<std::uint64_t>{11, 22, 33}));
    require_value(mapped::AtomicWord::bind(bytes, 8)).store_release(44);
    // The previous range ended in this window: this reload must be a cache hit.
    EXPECT_EQ(access.read_words(boundary, 1)[0], 44u);
    EXPECT_EQ(access.read_words(offset, first.size()), (std::vector<std::uint64_t>{11, 44, 33}));
    EXPECT_EQ(retained, (std::vector<std::uint64_t>{11, 22, 33}));

    EXPECT_TRUE(access.read_words(access._snapshot._fileBytes, 0).empty());
    EXPECT_TRUE(access.read_bytes(access._snapshot._tail, 0).empty());
    expect_io_error([&]
    {
      static_cast<void>(access.read_words(offset + 1, 1));
    });
    expect_io_error([&]
    {
      static_cast<void>(access.read_words(offset, std::numeric_limits<std::size_t>::max()));
    });
    expect_io_error([&]
    {
      static_cast<void>(access.read_words(access._snapshot._fileBytes, 1));
    });
    expect_io_error([&]
    {
      static_cast<void>(access.read_bytes(access._snapshot._tail, 1));
    });
    expect_io_error([&]
    {
      static_cast<void>(access.read_bytes(std::numeric_limits<std::uint64_t>::max(), 0));
    });
  }

  // Retained read mappings end at the recovered snapshot, not an unaccepted
  // append's larger EOF. Execute's real growth-error recovery must shrink back
  // while both readers are live, and a later operation must see a successful retry.
  TEST_F(RuntimeAccessTest, RetainedWindowsPermitAppendGrowthRollbackAndFreshRetry)
  {
    const auto accepted = append_window_geometry(window_document());
    class FailAppendGrowth final : public mapped::JournalFaultInjector
    {
    public:
      OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t) override
      {
        if (point == mapped::JournalCheckpoint::FileGrowth && !_fired)
        {
          _fired = true;
          return Error{ErrorCode::IoError, "Injected append growth failure", "test.runtime"};
        }
        return std::monostate{};
      }

      bool _fired{};
    } injector;
    std::uint64_t newReference{};
    {
      runtime::RuntimeAccess access(_state, true, &injector);
      const auto before = access._snapshot;
      const auto ringReference = accepted._records.at(1);
      const auto ringEnd = ringReference + access.record_envelope(ringReference, 8)._totalBytes;
      ASSERT_EQ((ringEnd - 8) / window_bytes, (before._fileBytes - 1) / window_bytes);
      const auto words = access.read_words(ringEnd - 8, 1);
      const auto manifest = access.immutable_record(accepted._manifest, 11);
      auto plan = append_plan(access);
      mapped::JournalRecord ring;
      ring._kind = 8;
      ring._mutable.resize(512);
      plan._records.push_back(std::move(ring));
      const std::array ready{runtime::session_ready_flag};
      plan._commands.push_back(runtime::runtime_write_words(112, ready));

      expect_error(access._journal.execute(access._guard, plan), ErrorCode::IoError);
      EXPECT_TRUE(injector._fired);
      EXPECT_EQ(require_value(access._file->size()), before._fileBytes);
      EXPECT_EQ(require_value(access._journal.snapshot(access._guard)), before);
      EXPECT_EQ(access.read_words(ringEnd - 8, 1), words);
      EXPECT_EQ(access.immutable_record(accepted._manifest, 11), manifest);
      const auto retry = require_value(access._journal.execute(access._guard, plan));
      newReference = retry._records.front();
      expect_io_error([&]
      {
        static_cast<void>(access.read_words(before._fileBytes, 1));
      });
      expect_io_error([&]
      {
        static_cast<void>(access.read_bytes(before._tail, 1));
      });
    }
    runtime::RuntimeAccess fresh(_state);
    EXPECT_EQ(fresh.record_envelope(newReference, 8)._kind, 8u);
    EXPECT_GT(fresh._snapshot._tail, newReference);
  }
} // namespace
