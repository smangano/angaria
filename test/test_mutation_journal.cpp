// Private transaction/recovery examples behind the public Session API. These
// fixtures deliberately build provisional storage, synthetic records, and corrupt
// bytes to isolate journal guarantees; applications should use Session/Task methods.
// SIGKILL cases cover process termination on a running host, not power-loss durability.

#include "journal_fixture.h"
#include "result_assertions.h"
#include "mutation_journal.h"
#include "native_atomic.h"
#include "session_metadata.h"
#include "angaria/detail/append_layout.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ANGARIA_NATIVE_JOURNAL_WORKER
#error ANGARIA_NATIVE_JOURNAL_WORKER must name the journal crash worker
#endif

extern char** environ;

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;

  using angaria::test_support::require_value;

  using angaria::test_support::expect_error;

  void put_integer(std::span<std::byte> bytes, std::size_t offset,
                    std::uint64_t value, std::size_t width = 8)
  {
    if (offset > bytes.size() || width > bytes.size() - offset)
    {
      throw std::runtime_error("test integer does not fit");
    }
    for (std::size_t index = 0; index < width; ++index)
    {
      bytes[offset + index] = static_cast<std::byte>((value >> (8 * index)) & 0xff);
    }
  }

  std::uint64_t get_integer(std::span<const std::byte> bytes, std::size_t offset)
  {
    if (offset > bytes.size() || 8 > bytes.size() - offset)
    {
      throw std::runtime_error("test integer does not fit");
    }
    std::uint64_t value{};
    for (std::size_t index = 0; index < 8; ++index)
    {
      value |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + index])} << (8 * index);
    }
    return value;
  }

  using angaria::test_support::section_offset;

  using angaria::test_support::write_word;

  // The two recognizable word values make partial redo visible. This minimal
  // draft tests journal framing/atomicity and intentionally omits the complete
  // Session bootstrap and Task-claim semantics required by the public runtime.
  using angaria::test_support::make_plan;

  // Install a real committed Process definition and well-formed claim state
  // (incarnation 7 by default), so rejection cannot pass merely because its target was
  // never registered. This is private journal setup, not public Task attachment.
  mapped::TransactionPlan registered_claim_plan(const mapped::JournalSnapshot& base,
                                                const mapped::SuperblockConfiguration& configuration,
                                                std::uint64_t incarnation = 7)
  {
    auto plan = make_plan(base, configuration, true);
    auto index = std::move(plan._commands.back());
    plan._commands.clear();
    plan._commands.push_back(std::move(index));
    const auto state = section_offset(configuration, mapped::SectionKind::TaskState);
    auto attached = write_word(state, 0);
    attached._payload.resize(80);
    const std::array<std::uint64_t, 10> words{incarnation, 0, incarnation - 1, 111, 42, 100, 3, 0, 0, 0};
    for (std::size_t word = 0; word != words.size(); ++word)
    {
      put_integer(attached._payload, word * 8, words[word]);
    }
    plan._commands.push_back(std::move(attached));
    return plan;
  }

  // Synthetic two-step replacement: a partial application can leave incarnation
  // 9 while the first ReleaseClaim still expects 7 or 8. Public attachment emits
  // only one release; the journal must reject this broader command stream.
  mapped::TransactionPlan chained_claim_plan(const mapped::JournalSnapshot& base,
                                             const mapped::SuperblockConfiguration& configuration)
  {
    mapped::TransactionPlan plan;
    plan._base = base;
    plan._newRevision = base._revision;
    const auto state = section_offset(configuration, mapped::SectionKind::TaskState);
    for (const auto ending : {std::uint64_t{7}, std::uint64_t{8}})
    {
      mapped::RedoCommand release{mapped::RedoOpcode::ReleaseClaim, state, std::vector<std::byte>(16)};
      put_integer(release._payload, 0, ending);
      put_integer(release._payload, 8, 200 + ending);
      plan._commands.push_back(std::move(release));
      plan._commands.push_back(write_word(state, ending + 1));
    }
    return plan;
  }

  using angaria::test_support::make_publication_plan;

  // Spawn a fresh executable rather than inheriting a live controller through
  // fork. Its 'R' handshake identifies an exact checkpoint before SIGKILL;
  // deadlines turn an unreachable checkpoint into a failure instead of a hang.
  class CrashWorker
  {
  public:
    CrashWorker(const fs::path& directory, const char* mode,
                mapped::JournalCheckpoint point, std::uint64_t index = 0)
    {
      std::array<int, 2> output{};
      if (::pipe(output.data()) == -1)
      {
        throw std::runtime_error("journal worker pipe failed");
      }
      for (const int fd : output)
      {
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
        {
          ::close(output[0]);
          ::close(output[1]);
          throw std::runtime_error("journal worker pipe flags failed");
        }
      }
      posix_spawn_file_actions_t actions;
      int error = ::posix_spawn_file_actions_init(&actions);
      if (error == 0)
      {
        error = ::posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
        error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, output[0]) : error;
        error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, output[1]) : error;
        const std::array<std::string, 5> values{
            ANGARIA_NATIVE_JOURNAL_WORKER, mode, directory.string(),
            std::to_string(static_cast<unsigned>(point)), std::to_string(index)};
        std::array<char*, 6> arguments{};
        for (std::size_t position = 0; position < values.size(); ++position)
        {
          arguments[position] = const_cast<char*>(values[position].c_str());
        }
        error = error == 0 ? ::posix_spawn(&_pid, values[0].c_str(), &actions, nullptr,
                                          arguments.data(), environ) : error;
        ::posix_spawn_file_actions_destroy(&actions);
      }
      ::close(output[1]);
      if (error != 0)
      {
        ::close(output[0]);
        throw std::system_error(error, std::generic_category(), "spawn journal crash worker");
      }
      _output = output[0];
    }

    ~CrashWorker()
    {
      if (_pid > 0)
      {
        ::kill(_pid, SIGKILL);
        int status{};
        while (::waitpid(_pid, &status, 0) == -1 && errno == EINTR)
        {
        }
      }
      if (_output != -1)
      {
        ::close(_output);
      }
    }

    CrashWorker(const CrashWorker&) = delete;
    CrashWorker& operator=(const CrashWorker&) = delete;

    void kill_at_checkpoint()
    {
      pollfd event{_output, POLLIN, 0};
      int result;
      do
      {
        result = ::poll(&event, 1, 3000);
      } while (result == -1 && errno == EINTR);
      char reached{};
      if (result <= 0 || ::read(_output, &reached, 1) != 1 || reached != 'R')
      {
        throw std::runtime_error("journal worker did not report its checkpoint");
      }
      if (::kill(_pid, SIGKILL) == -1)
      {
        throw std::runtime_error("journal worker could not be terminated");
      }
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      for (;;)
      {
        int status{};
        const auto waited = ::waitpid(_pid, &status, WNOHANG);
        if (waited == _pid)
        {
          _pid = -1;
          if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
          {
            throw std::runtime_error("journal worker did not die at the selected boundary");
          }
          return;
        }
        if (waited == -1 && errno != EINTR)
        {
          throw std::runtime_error("journal worker wait failed");
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
          throw std::runtime_error("journal worker termination deadline expired");
        }
        std::this_thread::sleep_for(1ms);
      }
    }

  private:
    pid_t _pid{-1};
    int _output{-1};
  };

  // Keep the controller, gate, and journal in dependency order for one test
  // operation. set_word/image are deliberate corruption/observation seams:
  // application code must not bypass journaled updates this way.
  struct Access
  {
    explicit Access(const fs::path& directory, mapped::JournalFaultInjector* injector = nullptr)
        : _controller(require_value(mapped::SessionController::open_existing(directory))),
          _guard(require_value(_controller->acquire_mutation())),
          _journal(require_value(mapped::MutationJournal::open(_controller, _guard, injector)))
    {
    }

    mapped::NativeFile& file()
    {
      return *require_value(_controller->file(_guard));
    }

    std::uint64_t word(std::uint64_t offset)
    {
      auto region = require_value(file().map_read_write(offset, 8));
      return require_value(mapped::AtomicWord::bind(require_value(region.writable_bytes()), 0)).load_acquire();
    }

    void set_word(std::uint64_t offset, std::uint64_t value)
    {
      auto region = require_value(file().map_read_write(offset, 8));
      require_value(mapped::AtomicWord::bind(require_value(region.writable_bytes()), 0)).publish_control(value);
    }

    std::vector<std::byte> image()
    {
      const auto size = require_value(file().size());
      if (size % 8 != 0)
      {
        throw std::runtime_error("test image is not word-aligned");
      }
      auto region = require_value(file().map_read_write(0, size));
      const auto bytes = require_value(region.writable_bytes());
      std::vector<std::byte> image(bytes.size());
      // Capture live control/readiness and root words through atomic accessors.
      for (std::size_t offset = 0; offset < image.size(); offset += 8)
      {
        put_integer(image, offset, require_value(mapped::AtomicWord::bind(bytes, offset)).load_acquire());
      }
      return image;
    }

    std::shared_ptr<mapped::SessionController> _controller;
    mapped::MutationGuard _guard;
    mapped::MutationJournal _journal;
  };

  // A one-shot error lets execute's own recovery succeed; a persistent error
  // also obstructs that repair, exercising the OutcomeUnknown path separately.
  class FailingInjector : public mapped::JournalFaultInjector
  {
  public:
    FailingInjector(mapped::JournalCheckpoint point, bool persistent = false, std::uint64_t index = 0)
        : _point(point), _persistent(persistent), _index(index)
    {
    }

    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t index) override
    {
      if (_active && point == _point && index == _index && (_persistent || !_fired))
      {
        _fired = true;
        return Error{ErrorCode::IoError, "Injected native journal I/O failure", "test.journal"};
      }
      return std::monostate{};
    }

    bool _active{true};
    bool _fired{};

  private:
    mapped::JournalCheckpoint _point;
    bool _persistent{};
    std::uint64_t _index{};
  };

  class CountingInjector : public mapped::JournalFaultInjector
  {
  public:
    OperationResult checkpoint(mapped::JournalCheckpoint, std::uint64_t) override
    {
      ++_calls;
      return std::monostate{};
    }

    std::uint64_t _calls{};
  };

  class MutationJournalTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-journal-XXXXXX").string();
      const auto directory = ::mkdtemp(pattern.data());
      ASSERT_NE(directory, nullptr);
      _root = directory;
      _configuration._session = {0x123456789abcdef0, 0xfedcba9876543210};
      _configuration._liveAbiProfile = require_value(mapped::qualify_native_profile())._liveAbiProfile;
      _configuration._maxLogBytes = 65536;
      _configuration._limits = CapacityLimits::empty().tasks(2).queues(2).source_sets(1)
                                  .publishing_sets(1).memberships_per_set(2).string_pool_bytes(64);
    }

    void TearDown() override
    {
      std::error_code ignored;
      if (!_root.empty())
      {
        fs::remove_all(_root, ignored);
      }
    }

    // Create only the provisional native resources needed by MutationJournal.
    // The resulting fixture is not a bootstrapped public Session. Setting full
    // reserves exactly the fixed arena so any Append lacks allocation space.
    fs::path make_session(const std::string& name = "session", bool full = false)
    {
      const auto directory = _root / name;
      fs::create_directories(directory / "claims");
      auto configuration = _configuration;
      const auto layout = mapped::make_fixed_layout(configuration._limits);
      if (!layout)
      {
        throw std::runtime_error("invalid journal fixture layout");
      }
      if (full)
      {
        configuration._maxLogBytes = layout->_arenaBegin;
      }
      auto file = require_value(mapped::NativeFile::create_exclusive(directory / "session.ang",
                                                                    configuration._maxLogBytes));
      require_value(file.resize(layout->_arenaBegin));
      require_value(file.write_exact(0, require_value(mapped::encode_initial_superblock(configuration))));
      require_value(mapped::NativeFile::create_exclusive(directory / "mutation.lock"));
      return directory;
    }

    // Private framing fixture, not registered application topology. A filler
    // ring puts a Task body across a 64 KiB boundary; a later ring's large redo
    // command also makes the final manifest span more than one scan window.
    mapped::TransactionPlan window_crossing_plan(const mapped::JournalSnapshot& base)
    {
      constexpr std::uint64_t window_bytes = 64 * 1024;
      constexpr std::uint64_t task_bytes = 192;
      auto plan = make_plan(base, _configuration, true);
      const auto boundary = ((base._tail + task_bytes + 192) / window_bytes + 1) * window_bytes;
      const auto crossing_reference = boundary - 128;
      const auto filler_reference = base._tail + task_bytes;

      mapped::JournalRecord filler;
      filler._kind = 8;
      filler._mutable.resize(crossing_reference - filler_reference - mapped::record_envelope_bytes);
      plan._records.push_back(std::move(filler));

      auto crossing = plan._records.front();
      crossing._previous = base._tail;
      put_integer(crossing._immutable, 32, crossing_reference + mapped::record_envelope_bytes + 72);
      plan._records.push_back(std::move(crossing));

      mapped::JournalRecord ring;
      ring._kind = 8;
      ring._mutable.resize(window_bytes);
      plan._records.push_back(std::move(ring));

      mapped::RedoCommand large_redo;
      large_redo._target = crossing_reference + task_bytes + mapped::record_envelope_bytes;
      large_redo._payload.resize(window_bytes);
      put_integer(large_redo._payload, 0, 0x0123456789abcdef);
      put_integer(large_redo._payload, large_redo._payload.size() - 8, 0xfedcba9876543210);
      plan._commands.push_back(std::move(large_redo));
      return plan;
    }

    std::uint64_t task_state() const
    {
      return section_offset(_configuration, mapped::SectionKind::TaskState);
    }

    void assert_recovered(const fs::path& directory, bool append, bool committed)
    {
      Access access(directory);
      const auto result = require_value(access._journal.snapshot(access._guard));
      const auto arena = mapped::make_fixed_layout(_configuration._limits)->_arenaBegin;
      EXPECT_EQ(result._generation, committed ? 1u : 0u);
      EXPECT_EQ(result._revision, append && committed ? 1u : 0u);
      EXPECT_EQ(result._accounted, result._tail);
      EXPECT_EQ(result._fileBytes, result._tail);
      EXPECT_EQ(access.word(task_state()), committed ? 11u : 0u);
      EXPECT_EQ(access.word(task_state() + 8), committed ? 22u : 0u);
      EXPECT_EQ(access.word(mapped::journal_offset), 0u);
      if (append && committed)
      {
        EXPECT_GT(result._tail, arena);
        EXPECT_GT(result._manifest, arena);
        EXPECT_EQ(access.word(section_offset(_configuration, mapped::SectionKind::TaskIndex) + 8), arena);
        const auto image = access.image();
        const auto manifest = require_value(mapped::decode_record_envelope(std::span{image}.subspan(result._manifest, 64)));
        EXPECT_EQ(manifest._kind, 11u);
        EXPECT_EQ(manifest._readiness, 2u);
        const auto record = require_value(mapped::decode_record_envelope(std::span{image}.subspan(arena, 64)));
        EXPECT_EQ(record._readiness, 2u);
      }
      else
      {
        EXPECT_EQ(result._tail, arena);
        EXPECT_EQ(result._manifest, 0u);
      }
      const auto before = access.image();
      require_value(access._journal.recover(access._guard));
      EXPECT_EQ(access.image(), before);
    }

    void assert_publication_recovered(const fs::path& directory, const mapped::JournalSnapshot& base)
    {
      Access access(directory);
      const auto snapshot = require_value(access._journal.snapshot(access._guard));
      EXPECT_EQ(snapshot._generation, base._generation + 1);
      EXPECT_EQ(snapshot._revision, base._revision);
      EXPECT_EQ(snapshot._accounted, snapshot._tail);
      EXPECT_EQ(snapshot._fileBytes, snapshot._tail);
      EXPECT_GT(snapshot._manifest, base._tail);
      EXPECT_EQ(access.word(mapped::journal_offset), 0u);
      const auto plan = make_publication_plan(base);
      const auto receipts = base._tail + 64 + plan._records[0]._immutable.size();
      auto expected = plan._records[0]._mutable;
      put_integer(expected, 0, 2);
      put_integer(expected, 8, 0x0000000100010002);
      put_integer(expected, 16, 7);
      put_integer(expected, 24, 13);
      put_integer(expected, 32, 1002);
      const auto image = access.image();
      ASSERT_GE(image.size(), base._tail + 576);
      const auto envelope = require_value(mapped::decode_record_envelope(
          std::span{image}.subspan(base._tail, 64)));
      EXPECT_EQ(envelope._kind, 9u);
      EXPECT_EQ(envelope._readiness, 2u);
      EXPECT_EQ(envelope._generation, base._generation + 1);
      EXPECT_EQ(envelope._registryRevision, base._revision);
      EXPECT_EQ(envelope._immutableBytes, 232u);
      EXPECT_EQ(envelope._totalBytes, 576u);
      ASSERT_GE(envelope._totalBytes, 64u + 232u + expected.size());
      ASSERT_LE(base._tail + envelope._totalBytes, image.size());
      EXPECT_TRUE(std::equal(plan._records[0]._immutable.begin(), plan._records[0]._immutable.end(),
                             image.begin() + base._tail + 64));
      EXPECT_TRUE(std::equal(expected.begin(), expected.end(), image.begin() + receipts));
      EXPECT_TRUE(std::all_of(image.begin() + receipts + expected.size(),
                              image.begin() + base._tail + envelope._totalBytes,
                              [](std::byte value)
                              {
                                return value == std::byte{};
                              }));
      EXPECT_EQ(access.word(receipts + 128), 1u); // Untouched initialized receipt survives.
      const auto manifest_envelope = require_value(mapped::decode_record_envelope(
          std::span{image}.subspan(snapshot._manifest, 64)));
      EXPECT_EQ(manifest_envelope._readiness, 2u);
      const auto manifest = require_value(mapped::decode_manifest_body(
          std::span{image}.subspan(snapshot._manifest + 64, manifest_envelope._immutableBytes), snapshot._manifest));
      EXPECT_EQ(manifest._subtype, mapped::TransactionSubtype::Publication);
      EXPECT_EQ(manifest._records, (std::vector<std::uint64_t>{base._tail}));
      EXPECT_EQ(manifest._commands, plan._commands);
      EXPECT_EQ(manifest._baseRevision, base._revision);
      EXPECT_EQ(manifest._newRevision, base._revision);
      require_value(access._journal.recover(access._guard));
      EXPECT_EQ(access.image(), image);
    }

    fs::path _root;
    mapped::SuperblockConfiguration _configuration;
  };

  // Reserve no append headroom, then compare a successful fixed-state update with a rejected Append.
  // Claim/issuance progress must not depend on allocating history; a known LogFull rejection changes no bytes.
  TEST_F(MutationJournalTest, FixedStateUsesReservedJournalEvenAtTheExactLogLimit)
  {
    const auto directory = make_session("full", true);
    Access access(directory);
    const auto base = require_value(access._journal.snapshot(access._guard));
    const auto result = require_value(access._journal.execute(access._guard,
        make_plan(base, access._controller->configuration())));
    EXPECT_EQ(result._generation, 1u);
    EXPECT_EQ(result._tail, base._tail);
    EXPECT_TRUE(result._records.empty());
    EXPECT_EQ(require_value(access.file().size()), base._fileBytes);
    EXPECT_EQ(access.word(task_state()), 11u);
    EXPECT_EQ(access.word(task_state() + 8), 22u);
    const auto current = require_value(access._journal.snapshot(access._guard));
    const auto before = access.image();
    expect_error(access._journal.execute(access._guard,
        make_plan(current, access._controller->configuration(), true)), ErrorCode::LogFull);
    EXPECT_EQ(access.image(), before);
  }

  TEST_F(MutationJournalTest, CompleteAppendPlanAndExecutionAgreeAtTheExactByteCeiling)
  {
    // This independent fixture costs record192 + manifest256. The latter owns
    // fixed64, one reference8, and redo96, in addition to its envelope/padding.
    const auto arena = mapped::make_fixed_layout(_configuration._limits)->_arenaBegin;
    _configuration._maxLogBytes = arena + 448;
    Access access(make_session());
    const auto base = require_value(access._journal.snapshot(access._guard));
    auto plan = make_plan(base, _configuration, true);
    mapped::AppendLayoutPlanner planner(base._tail);
    ASSERT_TRUE(planner.add_record(plan._records[0]._immutable.size()));
    const auto layout = planner.finish(96, _configuration._maxLogBytes);
    ASSERT_TRUE(layout);
    EXPECT_EQ(layout->_manifestOffset, arena + 192);
    EXPECT_EQ(layout->_manifestBytes, 256u);
    EXPECT_EQ(layout->_tail, _configuration._maxLogBytes);
    const auto result = require_value(access._journal.execute(access._guard, plan));
    EXPECT_EQ(result._manifest, layout->_manifestOffset);
    EXPECT_EQ(result._tail, layout->_tail);
    EXPECT_EQ(require_value(access.file().size()), _configuration._maxLogBytes);
  }

  TEST_F(MutationJournalTest, FittingRecordWithoutManifestHeadroomRejectsBeforeJournalPreparation)
  {
    const auto arena = mapped::make_fixed_layout(_configuration._limits)->_arenaBegin;
    _configuration._maxLogBytes = arena + 192;
    CountingInjector injector;
    Access access(make_session(), &injector);
    const auto base = require_value(access._journal.snapshot(access._guard));
    const auto before = access.image();
    expect_error(access._journal.execute(access._guard, make_plan(base, _configuration, true)), ErrorCode::LogFull);
    EXPECT_EQ(injector._calls, 0u);
    EXPECT_EQ(access.image(), before);
    EXPECT_EQ(require_value(access._journal.snapshot(access._guard)), base);
  }

  TEST_F(MutationJournalTest, LateOversizedRecordRejectsTheWholeBatchWithoutStorageEffects)
  {
    CountingInjector injector;
    Access access(make_session(), &injector);
    const auto base = require_value(access._journal.snapshot(access._guard));
    auto plan = make_plan(base, _configuration, true);
    // A fitting first definition cannot be prepared independently. The final
    // draft makes the complete request exceed the ceiling, so neither is stored.
    auto later = plan._records.front();
    later._immutable.resize(_configuration._maxLogBytes);
    plan._records.push_back(std::move(later));
    const auto before = access.image();
    expect_error(access._journal.execute(access._guard, plan), ErrorCode::LogFull);
    EXPECT_EQ(injector._calls, 0u);
    EXPECT_EQ(access.image(), before);
    EXPECT_EQ(require_value(access._journal.snapshot(access._guard)), base);
  }

  // A definition and its index update belong to one Append, whose final manifest records the entire batch.
  // Reopening verifies that accepted records, roots, revision, and generation form one recoverable result.
  TEST_F(MutationJournalTest, AppendCommitsDefinitionsManifestAndCanonicalRootsTogether)
  {
    const auto directory = make_session();
    {
      Access access(directory);
      const auto base = require_value(access._journal.snapshot(access._guard));
      const auto result = require_value(access._journal.execute(access._guard,
          make_plan(base, _configuration, true)));
      ASSERT_EQ(result._records.size(), 1u);
      EXPECT_EQ(result._records.front(), base._tail);
      EXPECT_GT(result._manifest, result._records.front());
      const auto image = access.image();
      EXPECT_EQ(get_integer(image, result._manifest + 32), 0u);
      const auto envelope = require_value(mapped::decode_record_envelope(std::span{image}.subspan(result._manifest, 64)));
      const auto manifest = require_value(mapped::decode_manifest_body(
          std::span{image}.subspan(result._manifest + 64, envelope._immutableBytes), result._manifest));
      EXPECT_EQ(manifest._records, result._records);
      EXPECT_EQ(manifest._baseRevision, 0u);
      EXPECT_EQ(manifest._newRevision, 1u);
      EXPECT_EQ(manifest._commands, make_plan(base, _configuration, true)._commands);
    }
    assert_recovered(directory, true, true);
  }

  // The runtime may reuse the record boundaries already validated by a
  // snapshot. Every export still describes a fresh, complete prefix: fixed
  // progress changes its generation, and later Appends add both records and
  // their final manifests without losing the earlier boundaries.
  TEST_F(MutationJournalTest, SnapshotExportsCompleteRecordBoundariesAcrossCompletedBatches)
  {
    Access access(make_session());
    std::vector<mapped::JournalRecordRange> records;
    const auto initial = require_value(access._journal.snapshot(access._guard, records));
    EXPECT_TRUE(records.empty());

    const auto first = require_value(access._journal.execute(access._guard,
        make_plan(initial, _configuration, true)));
    const auto after_first = require_value(access._journal.snapshot(access._guard, records));
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0]._offset, first._records.at(0));
    EXPECT_EQ(records[1]._offset, first._manifest);
    const auto original = records[0]._envelope;

    const auto fixed = require_value(access._journal.execute(access._guard,
        make_plan(after_first, _configuration)));
    const auto after_fixed = require_value(access._journal.snapshot(access._guard, records));
    EXPECT_EQ(after_fixed._generation, fixed._generation);
    EXPECT_EQ(after_fixed._tail, after_first._tail);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0]._envelope, original);

    const auto second = require_value(access._journal.execute(access._guard,
        make_plan(after_fixed, _configuration, true)));
    const auto after_second = require_value(access._journal.snapshot(access._guard, records));
    EXPECT_EQ(after_second._generation, second._generation);
    EXPECT_EQ(after_second._manifest, second._manifest);
    EXPECT_EQ(after_second._tail, second._tail);
    EXPECT_EQ(require_value(access._journal.snapshot(access._guard)), after_second);
    ASSERT_EQ(records.size(), 4u);
    const std::array offsets{first._records.at(0), first._manifest,
                             second._records.at(0), second._manifest};
    const std::array<std::uint16_t, 4> kinds{2, 11, 2, 11};
    for (std::size_t index = 0; index != records.size(); ++index)
    {
      EXPECT_EQ(records[index]._offset, offsets[index]);
      EXPECT_EQ(records[index]._envelope._kind, kinds[index]);
      EXPECT_EQ(records[index]._envelope._readiness, 2u);
      EXPECT_EQ(records[index]._envelope._generation,
                index < 2 ? first._generation : second._generation);
      EXPECT_EQ(records[index]._mutableBytes, 0u);
    }
    EXPECT_EQ(records[0]._envelope, original);
  }

  // A failed export must expose neither a previous successful result nor the
  // valid early records visited before a later corrupted batch. Restoring the
  // bytes lets the same journal validate them anew; no long-lived cache masks
  // the deliberately injected damage or poisons later inspection.
  TEST_F(MutationJournalTest, FailedSnapshotExportLeavesNoStaleOrPartialRecordBoundaries)
  {
    Access access(make_session());
    const auto first = require_value(access._journal.execute(access._guard,
        make_plan(require_value(access._journal.snapshot(access._guard)), _configuration, true)));
    const auto second = require_value(access._journal.execute(access._guard,
        make_plan(require_value(access._journal.snapshot(access._guard)), _configuration, true)));
    std::vector<mapped::JournalRecordRange> records;
    const auto before = require_value(access._journal.snapshot(access._guard, records));
    ASSERT_EQ(records.size(), 4u);
    const auto revision_offset = second._records.at(0) + 40;
    std::array<std::byte, 8> revision{};
    put_integer(revision, 0, before._revision + 1);
    require_value(access.file().write_exact(revision_offset, revision));
    const auto evidence = access.image();

    expect_error(access._journal.snapshot(access._guard, records), ErrorCode::IoError);
    EXPECT_TRUE(records.empty());
    EXPECT_EQ(access.image(), evidence);
    EXPECT_EQ(access.word(mapped::journal_offset), static_cast<std::uint64_t>(mapped::JournalControl::Idle));

    put_integer(revision, 0, before._revision);
    require_value(access.file().write_exact(revision_offset, revision));
    EXPECT_EQ(require_value(access._journal.snapshot(access._guard, records)), before);
    ASSERT_EQ(records.size(), 4u);
    EXPECT_EQ(records.front()._offset, first._records.at(0));
    EXPECT_EQ(records.back()._offset, second._manifest);
  }

  // The scan must assemble metadata across window boundaries, including a
  // manifest whose redo stream is larger than one window. Later corruption in
  // its final padding must still be seen by a fresh scan under the same guard.
  TEST_F(MutationJournalTest, WindowedPrefixValidatesCrossingMetadataAndLargeManifestsFreshly)
  {
    constexpr std::uint64_t window_bytes = 64 * 1024;
    _configuration._maxLogBytes = 1024 * 1024;
    Access access(make_session());
    const auto plan = window_crossing_plan(require_value(access._journal.snapshot(access._guard)));
    const auto accepted = require_value(access._journal.execute(access._guard, plan));
    std::vector<mapped::JournalRecordRange> records;
    const auto before = require_value(access._journal.snapshot(access._guard, records));
    ASSERT_EQ(records.size(), 5u);
    ASSERT_EQ(accepted._records.size(), 4u);
    EXPECT_EQ(records[2]._offset % window_bytes, window_bytes - 128);
    EXPECT_EQ(records[2]._envelope._kind, 2u);
    EXPECT_EQ(records[2]._envelope._previous, accepted._records.front());
    EXPECT_EQ(records[2]._envelope._immutableBytes, 128u);
    for (std::size_t index = 0; index != accepted._records.size(); ++index)
    {
      EXPECT_EQ(records[index]._offset, accepted._records[index]);
      EXPECT_EQ(records[index]._envelope._readiness, 2u);
    }
    ASSERT_EQ(records.back()._offset, accepted._manifest);
    const auto manifest_bytes = records.back()._envelope._immutableBytes;
    ASSERT_GT(manifest_bytes, window_bytes);
    EXPECT_NE(before._tail % window_bytes, 0u);

    const auto image = access.image();
    const auto manifest = require_value(mapped::decode_manifest_body(
        std::span{image}.subspan(accepted._manifest + mapped::record_envelope_bytes, manifest_bytes),
        accepted._manifest));
    EXPECT_EQ(manifest._records, accepted._records);
    EXPECT_EQ(manifest._commands, plan._commands);
    EXPECT_EQ(access.word(plan._commands.back()._target), 0x0123456789abcdefu);
    EXPECT_EQ(access.word(plan._commands.back()._target + window_bytes - 8), 0xfedcba9876543210u);

    const auto padding_offset = accepted._manifest + mapped::record_envelope_bytes + manifest_bytes - 1;
    ASSERT_GT(padding_offset, 3 * window_bytes);
    ASSERT_EQ(image[padding_offset], std::byte{});
    const std::array corrupt{std::byte{1}};
    require_value(access.file().write_exact(padding_offset, corrupt));
    const auto evidence = access.image();
    expect_error(access._journal.snapshot(access._guard, records), ErrorCode::IoError);
    EXPECT_TRUE(records.empty());
    EXPECT_EQ(access.image(), evidence);

    const std::array repaired{std::byte{0}};
    require_value(access.file().write_exact(padding_offset, repaired));
    EXPECT_EQ(require_value(access._journal.snapshot(access._guard, records)), before);
    ASSERT_EQ(records.size(), 5u);
    EXPECT_EQ(records.back()._offset, accepted._manifest);
  }

  // One scan grows its manifest scratch storage for the middle batch, then
  // decodes a small final batch. Only each requested body belongs to the
  // decoder, and exported record ranges must own their values after the scan.
  TEST_F(MutationJournalTest, PrefixManifestScratchHandlesSmallLargeSmallBatches)
  {
    constexpr std::uint64_t window_bytes = 64 * 1024;
    _configuration._maxLogBytes = 1024 * 1024;
    Access access(make_session());
    const auto first = require_value(access._journal.execute(access._guard,
        make_plan(require_value(access._journal.snapshot(access._guard)), _configuration, true)));
    const auto large = require_value(access._journal.execute(access._guard,
        window_crossing_plan(require_value(access._journal.snapshot(access._guard)))));
    const auto last = require_value(access._journal.execute(access._guard,
        make_plan(require_value(access._journal.snapshot(access._guard)), _configuration, true)));

    std::vector<mapped::JournalRecordRange> records;
    const auto before = require_value(access._journal.snapshot(access._guard, records));
    ASSERT_EQ(records.size(), 9u);
    EXPECT_EQ(before._manifest, last._manifest);
    EXPECT_EQ(before._tail, last._tail);
    EXPECT_EQ(records[1]._offset, first._manifest);
    EXPECT_EQ(records[6]._offset, large._manifest);
    EXPECT_EQ(records[8]._offset, last._manifest);
    ASSERT_LT(records[1]._envelope._immutableBytes, window_bytes);
    ASSERT_GT(records[6]._envelope._immutableBytes, window_bytes);
    ASSERT_EQ(records[8]._envelope._immutableBytes, records[1]._envelope._immutableBytes);
    const auto retained = records;

    // The smaller body's final padding remains part of its exact decoder
    // input, even after a much larger body occupied the same scratch storage.
    const auto padding_offset = last._manifest + mapped::record_envelope_bytes +
                                records.back()._envelope._immutableBytes - 1;
    ASSERT_EQ(access.image()[padding_offset], std::byte{});
    const std::array corrupt{std::byte{1}};
    require_value(access.file().write_exact(padding_offset, corrupt));
    const auto evidence = access.image();
    expect_error(access._journal.snapshot(access._guard, records), ErrorCode::IoError);
    EXPECT_TRUE(records.empty());
    EXPECT_EQ(access.image(), evidence);

    const std::array repaired{std::byte{0}};
    require_value(access.file().write_exact(padding_offset, repaired));
    EXPECT_EQ(require_value(access._journal.snapshot(access._guard, records)), before);
    ASSERT_EQ(records.size(), retained.size());
    for (std::size_t index = 0; index != records.size(); ++index)
    {
      EXPECT_EQ(records[index]._offset, retained[index]._offset);
      EXPECT_EQ(records[index]._envelope, retained[index]._envelope);
      EXPECT_EQ(records[index]._mutableBegin, retained[index]._mutableBegin);
      EXPECT_EQ(records[index]._mutableBytes, retained[index]._mutableBytes);
    }
  }

  // Deliberately shorten a committed Task's declared immutable body below its
  // 72-byte fixed header. The scan must pass only the declared bytes to the
  // validator, reject the prefix without writes, and inspect repaired bytes anew.
  TEST_F(MutationJournalTest, PrefixScanRejectsAShortDeclaredFixedBodyWithoutWrites)
  {
    Access access(make_session());
    const auto accepted = require_value(access._journal.execute(access._guard,
        make_plan(require_value(access._journal.snapshot(access._guard)), _configuration, true)));
    std::vector<mapped::JournalRecordRange> records;
    const auto before = require_value(access._journal.snapshot(access._guard, records));
    const auto immutable_bytes = records.front()._envelope._immutableBytes;
    std::array<std::byte, 8> length{};
    put_integer(length, 0, 64);
    require_value(access.file().write_exact(accepted._records.front() + 16, length));
    const auto evidence = access.image();

    expect_error(access._journal.snapshot(access._guard, records), ErrorCode::IoError);
    EXPECT_TRUE(records.empty());
    EXPECT_EQ(access.image(), evidence);

    put_integer(length, 0, immutable_bytes);
    require_value(access.file().write_exact(accepted._records.front() + 16, length));
    EXPECT_EQ(require_value(access._journal.snapshot(access._guard, records)), before);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records.front()._envelope._immutableBytes, immutable_bytes);
  }

  // A scan of a large committed base ends at its partial final window, not at
  // an unaccepted append's longer physical EOF. Real FileGrowth failure must
  // therefore roll back without a scan mapping lease obstructing the shrink.
  TEST_F(MutationJournalTest, LargePrefixWindowsDoNotObstructPreparedAppendRollback)
  {
    _configuration._maxLogBytes = 1024 * 1024;
    FailingInjector injector(mapped::JournalCheckpoint::FileGrowth);
    injector._active = false;
    Access access(make_session(), &injector);
    require_value(access._journal.execute(access._guard,
        window_crossing_plan(require_value(access._journal.snapshot(access._guard)))));
    const auto base = require_value(access._journal.snapshot(access._guard));
    const auto before = access.image();
    auto plan = make_plan(base, _configuration, true);
    put_integer(plan._commands[0]._payload, 0, 33);
    put_integer(plan._commands[1]._payload, 0, 44);
    injector._active = true;

    expect_error(access._journal.execute(access._guard, plan), ErrorCode::IoError);
    EXPECT_TRUE(injector._fired);
    EXPECT_EQ(require_value(access._journal.snapshot(access._guard)), base);
    EXPECT_EQ(require_value(access.file().size()), base._fileBytes);
    EXPECT_EQ(access.word(mapped::journal_offset), static_cast<std::uint64_t>(mapped::JournalControl::Idle));
    EXPECT_EQ(access.word(task_state()), 11u);
    EXPECT_EQ(access.word(task_state() + 8), 22u);
    const auto after = access.image();
    const auto arena = mapped::make_fixed_layout(_configuration._limits)->_arenaBegin;
    ASSERT_EQ(after.size(), before.size());
    EXPECT_TRUE(std::equal(before.begin() + arena, before.end(), after.begin() + arena));

    const auto accepted = require_value(access._journal.execute(access._guard, plan));
    std::vector<mapped::JournalRecordRange> records;
    const auto current = require_value(access._journal.snapshot(access._guard, records));
    EXPECT_EQ(current._generation, base._generation + 1);
    EXPECT_EQ(current._tail, accepted._tail);
    ASSERT_EQ(records.size(), 7u);
    EXPECT_EQ(records.back()._offset, accepted._manifest);
    EXPECT_EQ(access.word(task_state()), 33u);
    EXPECT_EQ(access.word(task_state() + 8), 44u);
  }

  // Publication storage includes preallocated mutable receipt slots as well as immutable bytes.
  // Initial receipt state and its redo must both survive reopening; this is storage coverage, not send semantics.
  TEST_F(MutationJournalTest, PublicationAppendMaterializesItsInitializedMutableReceipts)
  {
    const auto directory = make_session();
    mapped::JournalSnapshot base;
    {
      Access access(directory);
      base = require_value(access._journal.snapshot(access._guard));
      const auto result = require_value(access._journal.execute(access._guard, make_publication_plan(base)));
      ASSERT_EQ(result._records.size(), 1u);
      EXPECT_EQ(result._records[0], base._tail);
      EXPECT_GT(result._manifest, base._tail);
    }
    assert_publication_recovered(directory, base);
  }

  // Receipt count, allocated suffix, and reserved bytes must agree before preparing an Append.
  // These deliberately malformed drafts show that input rejection leaves both storage and the base snapshot intact.
  TEST_F(MutationJournalTest, InvalidPublicationReceiptStorageAndReservedBytesLeaveTheFileUnchanged)
  {
    Access access(make_session());
    const auto base = require_value(access._journal.snapshot(access._guard));
    const auto before = access.image();
    for (unsigned invalid = 0; invalid != 7; ++invalid)
    {
      SCOPED_TRACE(invalid);
      auto plan = make_publication_plan(base);
      auto& record = plan._records[0];
      switch (invalid)
      {
      case 0:
        record._mutable.resize(128);
        break;
      case 1:
        record._mutable.resize(384);
        break;
      case 2:
        record._mutable.clear();
        break;
      case 3:
        put_integer(record._immutable, 104, 1);
        break;
      case 4:
        put_integer(record._immutable, 104, std::numeric_limits<std::uint64_t>::max());
        break;
      case 5:
        put_integer(record._immutable, 104, 0);
        break;
      case 6:
        record._immutable.resize(127);
        break;
      }
      expect_error(access._journal.execute(access._guard, plan), ErrorCode::InvalidArgument);
      EXPECT_EQ(access.image(), before);
      EXPECT_EQ(require_value(access._journal.snapshot(access._guard)), base);
    }
    for (const auto reserved : {56u, 64u, 127u, 184u, 192u, 255u})
    {
      SCOPED_TRACE(reserved);
      auto plan = make_publication_plan(base);
      plan._records[0]._mutable[reserved] = std::byte{1};
      expect_error(access._journal.execute(access._guard, plan), ErrorCode::InvalidArgument);
      EXPECT_EQ(access.image(), before);
      EXPECT_EQ(require_value(access._journal.snapshot(access._guard)), base);
    }
  }

  // Kill an accepted publication writer and then kill two successive recovery processes.
  // Eventually recovering must preserve the untouched pending receipt and finish the partially issued receipt.
  TEST_F(MutationJournalTest, PublicationReceiptInitializationSurvivesRepeatedInterruptedRecovery)
  {
    const std::array<std::pair<mapped::JournalCheckpoint, std::uint64_t>, 3> cuts{{
        {mapped::JournalCheckpoint::Committed, 0},
        {mapped::JournalCheckpoint::RedoWord, 0},
        {mapped::JournalCheckpoint::RedoWord, 2},
    }};
    for (std::size_t position = 0; position != cuts.size(); ++position)
    {
      SCOPED_TRACE(position);
      const auto directory = make_session("publication-cut-" + std::to_string(position));
      mapped::JournalSnapshot base;
      {
        Access access(directory);
        base = require_value(access._journal.snapshot(access._guard));
      }
      CrashWorker writer(directory, "publication", cuts[position].first, cuts[position].second);
      writer.kill_at_checkpoint();
      {
        Access access(directory);
        EXPECT_EQ(access.word(base._tail + 64 + 232 + 128), 1u);
      }
      CrashWorker partial_redo(directory, "recover", mapped::JournalCheckpoint::RedoWord, 1);
      partial_redo.kill_at_checkpoint();
      CrashWorker partial_records(directory, "recover", mapped::JournalCheckpoint::RecordCommitted, 0);
      partial_records.kill_at_checkpoint();
      assert_publication_recovered(directory, base);
    }
  }

  // A captured plan becomes stale after any completed operation, including one that appends nothing.
  // The generation check prevents overwriting newer fixed-state progress merely because tail/revision stayed equal.
  TEST_F(MutationJournalTest, FixedStateGenerationMakesOlderPlansStaleWithoutChangingTheirTail)
  {
    Access access(make_session());
    const auto base = require_value(access._journal.snapshot(access._guard));
    const auto stale = make_plan(base, _configuration);
    require_value(access._journal.execute(access._guard, stale));
    const auto current = require_value(access._journal.snapshot(access._guard));
    EXPECT_EQ(current._tail, base._tail);
    EXPECT_EQ(current._revision, base._revision);
    EXPECT_EQ(current._generation, base._generation + 1);
    const auto before = access.image();
    expect_error(access._journal.execute(access._guard, stale), ErrorCode::Conflict);
    EXPECT_EQ(access.image(), before);
  }

  // Interleave Append and FixedState so operation generation differs from Registry revision.
  // A later interrupted Append must accept mixed old/new final roots and still link to the previous Append manifest.
  TEST_F(MutationJournalTest, AppendAfterFixedStateRecoversMixedRootsAndRetainsItsManifestPredecessor)
  {
    const auto directory = make_session();
    mapped::JournalSnapshot base;
    std::uint64_t first_manifest{};
    std::uint64_t first_record{};
    {
      Access access(directory);
      const auto first = require_value(access._journal.execute(access._guard,
          make_plan(require_value(access._journal.snapshot(access._guard)), _configuration, true)));
      first_manifest = first._manifest;
      first_record = first._records.at(0);
      require_value(access._journal.execute(access._guard,
          make_plan(require_value(access._journal.snapshot(access._guard)), _configuration)));
      base = require_value(access._journal.snapshot(access._guard));
      ASSERT_EQ(base._generation, 2u);
      ASSERT_EQ(base._revision, 1u);
    }
    // Tail/accounting/revision have their proposed values, while the manifest
    // and completed generation still have their prior values at this boundary.
    CrashWorker interrupted(directory, "append", mapped::JournalCheckpoint::EpilogueWord, 2);
    interrupted.kill_at_checkpoint();
    Access access(directory);
    const auto recovered = require_value(access._journal.snapshot(access._guard));
    EXPECT_EQ(recovered._generation, 3u);
    EXPECT_EQ(recovered._revision, 2u);
    EXPECT_GT(recovered._tail, base._tail);
    EXPECT_GT(recovered._manifest, first_manifest);
    const auto image = access.image();
    EXPECT_EQ(get_integer(image, recovered._manifest + 32), first_manifest);
    EXPECT_EQ(require_value(mapped::decode_record_envelope(std::span{image}.subspan(first_record, 64)))._readiness, 2u);
    EXPECT_EQ(get_integer(image, section_offset(_configuration, mapped::SectionKind::TaskIndex) + 8), base._tail);
  }

  // Validate targets, inline capacity, alignment, and compact-command prerequisites before publishing intent.
  // The byte-for-byte comparison makes the difference between known rejection and interrupted work explicit.
  TEST_F(MutationJournalTest, InvalidPreflightChangesNoSessionBytes)
  {
    Access access(make_session());
    const auto base = require_value(access._journal.snapshot(access._guard));
    const std::array<std::uint64_t, 8> forbidden{
        0, mapped::journal_offset, 80, 88, 120, task_state() + 80,
        section_offset(_configuration, mapped::SectionKind::StringPool), base._tail + 8};
    const auto before = access.image();
    for (const auto target : forbidden)
    {
      SCOPED_TRACE(target);
      auto plan = make_plan(base, _configuration);
      plan._commands[0]._target = target;
      expect_error(access._journal.execute(access._guard, plan), ErrorCode::InvalidArgument);
      EXPECT_EQ(access.image(), before);
    }
    auto unaligned = make_plan(base, _configuration);
    ++unaligned._commands[0]._target;
    expect_error(access._journal.execute(access._guard, unaligned), ErrorCode::InvalidArgument);
    auto oversized = make_plan(base, _configuration);
    oversized._commands.assign(80, write_word(task_state(), 1));
    expect_error(access._journal.execute(access._guard, oversized), ErrorCode::CapacityExceeded);
    auto compact = make_plan(base, _configuration);
    compact._commands.resize(1);
    compact._commands[0]._opcode = mapped::RedoOpcode::ReleaseClaim;
    compact._commands[0]._payload.resize(16);
    put_integer(compact._commands[0]._payload, 0, 1);
    expect_error(access._journal.execute(access._guard, compact), ErrorCode::InvalidArgument);
    EXPECT_EQ(access.image(), before);
  }

  // Idle is the authority that no pending operation exists; its reusable body may contain stale or partial bytes.
  // Trying to decode that body would turn a harmless interrupted preparation into false corruption.
  TEST_F(MutationJournalTest, IdleIgnoresAnInterruptedOrGarbageJournalBody)
  {
    Access access(make_session());
    const auto before = require_value(access._journal.snapshot(access._guard));
    std::vector<std::byte> garbage(mapped::journal_bytes - 8, std::byte{0x5a});
    require_value(access.file().write_exact(mapped::journal_offset + 8, garbage));
    require_value(access._journal.recover(access._guard));
    EXPECT_EQ(require_value(access._journal.snapshot(access._guard)), before);
  }

  // Stop real exec workers on both sides of the sole acceptance point, Committed.
  // Recovery must expose the old state before that point or finish the complete accepted state after it, never a mixture.
  TEST_F(MutationJournalTest, NativeFixedStateCrashesKeepExactlyThePrecommitOrCommittedState)
  {
    const std::array<std::pair<mapped::JournalCheckpoint, std::uint64_t>, 10> cuts{{
        {mapped::JournalCheckpoint::JournalBodyWord, 0},
        {mapped::JournalCheckpoint::Prepared, 0},
        {mapped::JournalCheckpoint::Committed, 0},
        {mapped::JournalCheckpoint::RedoWord, 0},
        {mapped::JournalCheckpoint::RedoWord, 1},
        {mapped::JournalCheckpoint::EpilogueWord, 0},
        {mapped::JournalCheckpoint::EpilogueWord, 2},
        {mapped::JournalCheckpoint::EpilogueWord, 4},
        {mapped::JournalCheckpoint::Applied, 0},
        {mapped::JournalCheckpoint::Idle, 0},
    }};
    for (std::size_t position = 0; position < cuts.size(); ++position)
    {
      const auto [point, index] = cuts[position];
      SCOPED_TRACE(position);
      const auto directory = make_session("fixed-cut-" + std::to_string(position));
      CrashWorker worker(directory, "fixed", point, index);
      worker.kill_at_checkpoint();
      assert_recovered(directory, false, position >= 2);
    }
  }

  // Append adds growth and record-materialization boundaries to the same acceptance rule.
  // Ready records before Committed are discardable; accepted records and their manifest must survive every later cut.
  TEST_F(MutationJournalTest, NativeAppendCrashesRecoverWithoutLosingCommittedRecords)
  {
    const std::array<std::pair<mapped::JournalCheckpoint, std::uint64_t>, 15> cuts{{
        {mapped::JournalCheckpoint::JournalBodyWord, 0},
        {mapped::JournalCheckpoint::Prepared, 0},
        {mapped::JournalCheckpoint::FileGrowth, 0},
        {mapped::JournalCheckpoint::RecordBodyWord, 0},
        {mapped::JournalCheckpoint::RecordReady, 0},
        {mapped::JournalCheckpoint::RecordReady, 1},
        {mapped::JournalCheckpoint::Committed, 0},
        {mapped::JournalCheckpoint::RedoWord, 0},
        {mapped::JournalCheckpoint::RecordCommitted, 0},
        {mapped::JournalCheckpoint::RecordCommitted, 1},
        {mapped::JournalCheckpoint::EpilogueWord, 0},
        {mapped::JournalCheckpoint::EpilogueWord, 2},
        {mapped::JournalCheckpoint::EpilogueWord, 4},
        {mapped::JournalCheckpoint::Applied, 0},
        {mapped::JournalCheckpoint::Idle, 0},
    }};
    for (std::size_t position = 0; position < cuts.size(); ++position)
    {
      const auto [point, index] = cuts[position];
      SCOPED_TRACE(position);
      const auto directory = make_session("append-cut-" + std::to_string(position));
      CrashWorker worker(directory, "append", point, index);
      worker.kill_at_checkpoint();
      assert_recovered(directory, true, position >= 6);
    }
  }

  // Recovery is itself an interruptible operation, so restart it at each major repair boundary.
  // Absolute redo must preserve accepted work, while repeated precommit rollback must remain harmless.
  TEST_F(MutationJournalTest, RepeatedRecoveryCrashesRedoAcceptedWorkAndRepeatRollbackSafely)
  {
    const std::array<std::pair<mapped::JournalCheckpoint, std::uint64_t>, 7> cuts{{
        {mapped::JournalCheckpoint::RedoWord, 0},
        {mapped::JournalCheckpoint::RecordCommitted, 0},
        {mapped::JournalCheckpoint::EpilogueWord, 0},
        {mapped::JournalCheckpoint::EpilogueWord, 2},
        {mapped::JournalCheckpoint::EpilogueWord, 4},
        {mapped::JournalCheckpoint::Applied, 0},
        {mapped::JournalCheckpoint::Idle, 0},
    }};
    for (std::size_t position = 0; position < cuts.size(); ++position)
    {
      SCOPED_TRACE(position);
      const auto directory = make_session("redo-cut-" + std::to_string(position));
      CrashWorker original(directory, "append", mapped::JournalCheckpoint::Committed);
      original.kill_at_checkpoint();
      CrashWorker recovery(directory, "recover", cuts[position].first, cuts[position].second);
      recovery.kill_at_checkpoint();
      assert_recovered(directory, true, true);
    }
    const auto directory = make_session("rollback-cut");
    CrashWorker original(directory, "append", mapped::JournalCheckpoint::FileGrowth);
    original.kill_at_checkpoint();
    for (unsigned repeat = 0; repeat < 2; ++repeat)
    {
      CrashWorker recovery(directory, "recover", mapped::JournalCheckpoint::Rollback);
      recovery.kill_at_checkpoint();
    }
    assert_recovered(directory, true, false);
  }

  // A prepared FixedState operation has no legitimate file-growth effect.
  // An unexplained suffix is therefore evidence to retain and report, not space recovery may silently truncate.
  TEST_F(MutationJournalTest, PreparedFixedStateNeverTruncatesAnUnexpectedSuffix)
  {
    const auto directory = make_session();
    CrashWorker original(directory, "fixed", mapped::JournalCheckpoint::Prepared);
    original.kill_at_checkpoint();
    Access access(directory);
    const auto size = require_value(access.file().size());
    require_value(access.file().resize(size + 64));
    const auto evidence = access.image();
    expect_error(access._journal.recover(access._guard), ErrorCode::IoError);
    EXPECT_EQ(access.image(), evidence);
    EXPECT_EQ(require_value(access.file().size()), size + 64);
  }

  // Prepared means canonical state was not yet changed by this operation.
  // Unexpected roots or malformed header bytes invalidate that assumption and must prevent destructive rollback.
  TEST_F(MutationJournalTest, PreparedCanonicalChangesAndMalformedHeadersRetainEvidence)
  {
    for (unsigned variant = 0; variant < 2; ++variant)
    {
      SCOPED_TRACE(variant);
      const auto directory = make_session("prepared-corrupt-" + std::to_string(variant));
      CrashWorker original(directory, "append", mapped::JournalCheckpoint::Prepared);
      original.kill_at_checkpoint();
      Access access(directory);
      access.set_word(variant == 0 ? 80 : mapped::journal_offset + 88, 42);
      const auto evidence = access.image();
      expect_error(access._journal.recover(access._guard), ErrorCode::IoError);
      EXPECT_EQ(access.image(), evidence);
    }
  }

  // Even unaccepted inline intent must be valid before recovery clears its evidence.
  // Inject malformed framing, targets, compact prerequisites, and padding to demonstrate failure without writes.
  TEST_F(MutationJournalTest, PreparedFixedStateValidatesItsEntireInlineIntentBeforeDiscardingIt)
  {
    for (unsigned variant = 0; variant < 5; ++variant)
    {
      SCOPED_TRACE(variant);
      const auto directory = make_session("prepared-inline-" + std::to_string(variant));
      CrashWorker original(directory, "fixed", mapped::JournalCheckpoint::Prepared);
      original.kill_at_checkpoint();
      Access access(directory);
      const auto commands = mapped::journal_offset + mapped::journal_header_bytes;
      if (variant == 0)
      {
        std::array<std::byte, 2> unknown_opcode{};
        put_integer(unknown_opcode, 0, 99, 2);
        require_value(access.file().write_exact(commands, unknown_opcode));
      }
      else if (variant == 1 || variant == 3)
      {
        std::vector<std::byte> inline_body(mapped::inline_command_bytes);
        std::size_t length{};
        if (variant == 1)
        {
          mapped::RedoCommand compact;
          compact._opcode = mapped::RedoOpcode::ReleaseClaim;
          compact._target = task_state();
          compact._payload.resize(16);
          put_integer(compact._payload, 0, 1);
          const std::array compact_commands{compact};
          const auto encoded = require_value(mapped::encode_commands(compact_commands));
          std::copy(encoded.begin(), encoded.end(), inline_body.begin());
          length = encoded.size();
        }
        require_value(access.file().write_exact(commands, inline_body));
        std::array<std::byte, 4> inline_length{};
        put_integer(inline_length, 0, length, 4);
        require_value(access.file().write_exact(mapped::journal_offset + 64, inline_length));
      }
      else if (variant == 2)
      {
        std::array<std::byte, 8> immutable_target{};
        require_value(access.file().write_exact(commands + 24 + 8, immutable_target));
      }
      else
      {
        // The original two 24-byte commands leave the remaining inline area
        // reserved; a nonzero byte there cannot be silently discarded.
        const std::array nonzero_padding{std::byte{1}};
        require_value(access.file().write_exact(commands + 48, nonzero_padding));
      }
      const auto evidence = access.image();
      expect_error(access._journal.recover(access._guard), ErrorCode::IoError);
      EXPECT_EQ(access.image(), evidence);
      EXPECT_EQ(access.word(mapped::journal_offset), static_cast<std::uint64_t>(mapped::JournalControl::Prepared));
      EXPECT_EQ(access.word(task_state()), 0u);
      EXPECT_EQ(access.word(task_state() + 8), 0u);
    }
  }

  // A prepared Append must describe a feasible record batch and a legal Registry revision transition.
  // An impossible plan cannot justify truncating storage, even though it has not reached acceptance.
  TEST_F(MutationJournalTest, ImpossiblePreparedAppendPlansRetainTheirEvidence)
  {
    for (unsigned variant = 0; variant < 2; ++variant)
    {
      SCOPED_TRACE(variant);
      const auto directory = make_session("prepared-plan-" + std::to_string(variant));
      CrashWorker original(directory, "append", mapped::JournalCheckpoint::Prepared);
      original.kill_at_checkpoint();
      Access access(directory);
      std::vector<std::byte> value(variant == 0 ? 4 : 8);
      put_integer(value, 0, variant == 0 ? std::numeric_limits<std::uint32_t>::max() : 3, value.size());
      // The proposed byte extent cannot hold this record count, and the
      // Registry revision cannot jump from zero to three in one operation.
      require_value(access.file().write_exact(mapped::journal_offset + (variant == 0 ? 128 : 144), value));
      const auto evidence = access.image();
      expect_error(access._journal.recover(access._guard), ErrorCode::IoError);
      EXPECT_EQ(access.image(), evidence);
      EXPECT_EQ(access.word(mapped::journal_offset), static_cast<std::uint64_t>(mapped::JournalControl::Prepared));
    }
  }

  // An Idle journal does not make previously committed history trustworthy without validation.
  // Corrupt a retained record revision and show that both inspection and the next mutation preserve the evidence.
  TEST_F(MutationJournalTest, HistoricalRecordRevisionCorruptionBlocksSnapshotsAndLaterMutations)
  {
    Access access(make_session());
    const auto accepted = require_value(access._journal.execute(access._guard,
        make_plan(require_value(access._journal.snapshot(access._guard)), _configuration, true)));
    const auto base = require_value(access._journal.snapshot(access._guard));
    std::array<std::byte, 8> wrong_revision{};
    put_integer(wrong_revision, 0, 2);
    require_value(access.file().write_exact(accepted._records.at(0) + 40, wrong_revision));
    const auto evidence = access.image();
    expect_error(access._journal.snapshot(access._guard), ErrorCode::IoError);
    expect_error(access._journal.execute(access._guard, make_plan(base, _configuration)), ErrorCode::IoError);
    EXPECT_EQ(access.image(), evidence);
    EXPECT_EQ(access.word(mapped::journal_offset), static_cast<std::uint64_t>(mapped::JournalControl::Idle));
  }

  // Accepted intent is not permission to apply a malformed command stream.
  // Corrupt the control word or second command and verify validation prevents even the first otherwise valid write.
  TEST_F(MutationJournalTest, InvalidControlAndCommittedCommandsFailBeforeAnyRedoWrites)
  {
    for (unsigned variant = 0; variant < 2; ++variant)
    {
      SCOPED_TRACE(variant);
      const auto directory = make_session("fixed-corrupt-" + std::to_string(variant));
      CrashWorker original(directory, "fixed", mapped::JournalCheckpoint::Committed);
      original.kill_at_checkpoint();
      Access access(directory);
      if (variant == 0)
      {
        access.set_word(mapped::journal_offset, 99);
      }
      else
      {
        // Two separate 24-byte commands: make the SECOND command immutable.
        // Validation must reject it before installing the first valid command.
        std::array<std::byte, 8> target{};
        put_integer(target, 0, 0);
        require_value(access.file().write_exact(mapped::journal_offset + mapped::journal_header_bytes + 24 + 8, target));
      }
      const auto evidence = access.image();
      expect_error(access._journal.recover(access._guard), ErrorCode::IoError);
      EXPECT_EQ(access.image(), evidence);
      EXPECT_EQ(access.word(task_state()), 0u);
      EXPECT_EQ(access.word(task_state() + 8), 0u);
    }
  }

  // Once accepted, a damaged record or manifest cannot be treated as an uncommitted tail.
  // Recovery fails without applying redo or truncating the accepted allocation, leaving evidence for diagnosis.
  TEST_F(MutationJournalTest, CorruptCommittedRecordAndManifestNeverDiscardAcceptedEvidence)
  {
    for (unsigned variant = 0; variant < 2; ++variant)
    {
      SCOPED_TRACE(variant);
      const auto directory = make_session("append-corrupt-" + std::to_string(variant));
      CrashWorker original(directory, "append", mapped::JournalCheckpoint::Committed);
      original.kill_at_checkpoint();
      Access access(directory);
      const auto image = access.image();
      const auto header = require_value(mapped::decode_journal_header(
          std::span{image}.subspan(mapped::journal_offset, mapped::journal_header_bytes)));
      if (variant == 0)
      {
        access.set_word(header._firstRecord + 48, 0);
      }
      else
      {
        std::array<std::byte, 8> self_reference{};
        put_integer(self_reference, 0, header._manifest);
        require_value(access.file().write_exact(header._manifest + 128, self_reference));
      }
      const auto evidence = access.image();
      expect_error(access._journal.recover(access._guard), ErrorCode::IoError);
      EXPECT_EQ(access.image(), evidence);
      EXPECT_EQ(access.word(task_state()), 0u);
      EXPECT_EQ(require_value(access.file().size()), header._newFileBytes);
    }
  }

  // A one-shot growth failure occurs before Committed, so execute can prove rollback.
  // It returns the original I/O error with the old snapshot and no new logical effects.
  TEST_F(MutationJournalTest, OneShotPrecommitIoFailureRollsBackWithoutAcceptance)
  {
    FailingInjector injector(mapped::JournalCheckpoint::FileGrowth);
    Access access(make_session(), &injector);
    const auto base = require_value(access._journal.snapshot(access._guard));
    expect_error(access._journal.execute(access._guard, make_plan(base, _configuration, true)), ErrorCode::IoError);
    EXPECT_TRUE(injector._fired);
    EXPECT_EQ(require_value(access._journal.snapshot(access._guard)), base);
    EXPECT_EQ(access.word(task_state()), 0u);
  }

  // The same kind of failure after Committed has a different meaning: the operation was accepted.
  // If in-call repair succeeds, execute returns its accepted result instead of a false rejection.
  TEST_F(MutationJournalTest, PostcommitIoFailureRecoversAcceptanceRatherThanReportingRejection)
  {
    FailingInjector injector(mapped::JournalCheckpoint::Committed);
    Access access(make_session(), &injector);
    const auto base = require_value(access._journal.snapshot(access._guard));
    const auto accepted = require_value(access._journal.execute(access._guard, make_plan(base, _configuration, true)));
    EXPECT_TRUE(injector._fired);
    EXPECT_EQ(accepted._generation, 1u);
    EXPECT_EQ(access.word(task_state()), 11u);
    EXPECT_EQ(access.word(task_state() + 8), 22u);
    EXPECT_EQ(require_value(access._journal.snapshot(access._guard))._manifest, accepted._manifest);
  }

  // One transaction may release each Task at most once, and every incarnation
  // write must remain in that release's ending/immediate-successor window. A
  // transient jump outside it is unsafe even when another write later restores
  // a valid value: a crash between those writes must still be replayable.
  TEST_F(MutationJournalTest, RepeatedOrSkippedClaimIncarnationsRejectBeforePreparingAnyIntent)
  {
    for (unsigned variant = 0; variant != 5; ++variant)
    {
      SCOPED_TRACE(variant);
      const auto incarnation = variant == 4 ? std::numeric_limits<std::uint64_t>::max() : 7;
      Access access(make_session("unsafe-release-" + std::to_string(variant)));
      require_value(access._journal.execute(access._guard, registered_claim_plan(
          require_value(access._journal.snapshot(access._guard)), _configuration, incarnation)));
      const auto base = require_value(access._journal.snapshot(access._guard));
      auto plan = chained_claim_plan(base, _configuration);
      const auto release = plan._commands.front();
      if (variant == 0)
      {
        plan._commands = {release, release};
      }
      else if (variant == 2 || variant == 3)
      {
        plan._commands = {release, write_word(task_state(), 9)};
        if (variant == 3)
        {
          plan._commands.push_back(write_word(task_state(), 8));
        }
      }
      else if (variant == 4)
      {
        // The greatest incarnation has no successor; adding one must not wrap.
        auto last_release = release;
        put_integer(last_release._payload, 0, incarnation);
        plan._commands = {std::move(last_release), write_word(task_state(), 0)};
      }
      const auto before = access.image();
      expect_error(access._journal.execute(access._guard, plan), ErrorCode::InvalidArgument);
      EXPECT_EQ(access.image(), before);
      EXPECT_EQ(require_value(access._journal.snapshot(access._guard)), base);
      EXPECT_EQ(access.word(task_state()), incarnation);
      EXPECT_EQ(access.word(mapped::journal_offset), static_cast<std::uint64_t>(mapped::JournalControl::Idle));
    }
  }

  // Deliberately install unsupported repeated/jumping streams as stored intent,
  // bypassing the public preflight that now prevents it. Prepared recovery must
  // not discard the evidence, and Committed recovery must reject the whole stream
  // before writing even its first valid release, including after partial apply.
  TEST_F(MutationJournalTest, PersistedUnsafeClaimTransitionsPreserveEvidenceBeforeAnyRecoveryWrites)
  {
    for (unsigned variant = 0; variant != 6; ++variant)
    {
      SCOPED_TRACE(variant);
      const auto stage = variant % 3;
      Access access(make_session("persisted-releases-" + std::to_string(variant)));
      require_value(access._journal.execute(access._guard, registered_claim_plan(
          require_value(access._journal.snapshot(access._guard)), _configuration)));
      const auto base = require_value(access._journal.snapshot(access._guard));
      auto plan = chained_claim_plan(base, _configuration);
      if (variant >= 3)
      {
        const auto release = plan._commands.front();
        plan._commands = {release, write_word(task_state(), 9)};
      }
      const auto commands = require_value(mapped::encode_commands(plan._commands));
      mapped::JournalHeader header;
      header._control = stage == 0 ? mapped::JournalControl::Prepared : mapped::JournalControl::Committed;
      header._generation = base._generation + 1;
      header._baseTail = header._newTail = base._tail;
      header._baseAccounted = header._newAccounted = base._accounted;
      header._inlineBytes = static_cast<std::uint32_t>(commands.size());
      header._originalFileBytes = header._newFileBytes = base._fileBytes;
      header._baseRevision = header._newRevision = base._revision;
      const auto encoded = require_value(mapped::encode_journal_header(header));
      std::array<std::byte, mapped::journal_bytes> body{};
      std::copy(encoded.begin(), encoded.end(), body.begin());
      std::copy(commands.begin(), commands.end(), body.begin() + mapped::journal_header_bytes);
      require_value(access.file().write_exact(mapped::journal_offset + 8, std::span{body}.subspan(8)));
      if (stage == 2)
      {
        // Reproduce a crash after both successor incarnation words were stored.
        access.set_word(task_state(), 9);
      }
      access.set_word(mapped::journal_offset, static_cast<std::uint64_t>(header._control));
      const auto evidence = access.image();
      expect_error(access._journal.recover(access._guard), ErrorCode::IoError);
      EXPECT_EQ(access.image(), evidence);
      EXPECT_EQ(access.word(task_state()), stage == 2 ? 9u : 7u);
      EXPECT_EQ(access.word(mapped::journal_offset), static_cast<std::uint64_t>(header._control));
    }
  }

  // A replacement ends the previous Process claim and its Thread claims before installing the successor.
  // Interrupt every absolute write to show compact cleanup is repeatable, preserves child discovery tags,
  // and needs no new records even while issuance and retirement-floor words are temporarily mixed.
  TEST_F(MutationJournalTest, TaskReplacementReplaysEveryCleanupAndAttachmentWord)
  {
    // Two detach words per old claim, then ten absolute replacement words.
    // In particular, stop after resetting issuance but before its retired floor.
    for (std::uint64_t cut = 0; cut != 14; ++cut)
    {
      SCOPED_TRACE(cut);
      FailingInjector injector(mapped::JournalCheckpoint::RedoWord, false, cut);
      injector._active = false;
      Access access(make_session("claim-cut-" + std::to_string(cut)), &injector);
      const auto base = require_value(access._journal.snapshot(access._guard));
      mapped::TransactionPlan registration;
      registration._base = base;
      registration._kind = mapped::JournalKind::Append;
      registration._newRevision = 1;
      auto offset = base._tail;
      for (std::uint64_t id = 1; id != 3; ++id)
      {
        mapped::TaskDefinition definition;
        definition._id = static_cast<TaskId>(id);
        definition._descriptor._name = id == 1 ? "Process" : "Thread";
        definition._descriptor._kind = id == 1 ? TaskKind::Process : TaskKind::Thread;
        if (id == 2)
        {
          definition._descriptor._process = TaskId{1};
        }
        definition._registration._createdBy = TaskId{1};
        definition._registration._revision = 1;
        mapped::JournalRecord record;
        record._kind = 2;
        record._immutable = require_value(mapped::encode_task_definition(definition, offset));
        auto index = write_word(section_offset(_configuration, mapped::SectionKind::TaskIndex) + (id - 1) * 64, id);
        index._payload.resize(32);
        put_integer(index._payload, 8, offset);
        put_integer(index._payload, 16, 1);
        registration._commands.push_back(std::move(index));
        const auto next = mapped::checked_align(offset + 64 + record._immutable.size(), 64);
        ASSERT_TRUE(next);
        offset = *next;
        registration._records.push_back(std::move(record));
      }
      const std::array<std::uint64_t, 10> process{7, 20, 3, 111, 42, 100, 7, 0, 10, 50};
      const std::array<std::uint64_t, 10> thread{2, 10, 1, 111, 43, 120, 3, 7, 3, 0};
      for (const auto& [target, values] : std::array{
               std::pair{task_state(), process}, std::pair{task_state() + 256, thread}})
      {
        auto command = write_word(target, 0);
        command._payload.resize(80);
        for (std::size_t word = 0; word != values.size(); ++word)
        {
          put_integer(command._payload, word * 8, values[word]);
        }
        registration._commands.push_back(std::move(command));
      }
      require_value(access._journal.execute(access._guard, registration));
      const auto before = require_value(access._journal.snapshot(access._guard));
      mapped::TransactionPlan replacement;
      replacement._base = before;
      replacement._newRevision = before._revision;
      mapped::RedoCommand release{mapped::RedoOpcode::ReleaseClaim, task_state(), std::vector<std::byte>(16)};
      put_integer(release._payload, 0, 7);
      put_integer(release._payload, 8, 200);
      replacement._commands.push_back(std::move(release));
      auto attached = write_word(task_state(), 0);
      attached._payload.resize(80);
      const std::array<std::uint64_t, 10> successor{8, 0, 4, 222, 44, 200, 7, 0, 0, 200};
      for (std::size_t word = 0; word != successor.size(); ++word)
      {
        put_integer(attached._payload, word * 8, successor[word]);
      }
      replacement._commands.push_back(std::move(attached));
      injector._active = true;
      require_value(access._journal.execute(access._guard, replacement));
      EXPECT_TRUE(injector._fired);
      for (std::size_t word = 0; word != successor.size(); ++word)
      {
        EXPECT_EQ(access.word(task_state() + word * 8), successor[word]);
      }
      EXPECT_EQ(access.word(task_state() + 256 + 48), 6u);
      EXPECT_EQ(access.word(task_state() + 256 + 56), 7u);
      EXPECT_EQ(access.word(task_state() + 256 + 72), 200u);
      const auto after = require_value(access._journal.snapshot(access._guard));
      EXPECT_EQ(after._tail, before._tail);
      EXPECT_EQ(after._accounted, before._accounted);
      EXPECT_EQ(after._fileBytes, before._fileBytes);
      EXPECT_EQ(after._generation, before._generation + 1);
      const auto settled = access.image();
      require_value(access._journal.recover(access._guard));
      EXPECT_EQ(access.image(), settled);
    }
  }

  // Keep failing accepted repair so execute cannot establish the final state.
  // OutcomeUnknown preserves that uncertainty; later recovery resolves the committed operation instead of retrying it as new.
  TEST_F(MutationJournalTest, UnfinishedPostcommitRepairReportsOutcomeUnknownUntilRecoverySucceeds)
  {
    FailingInjector injector(mapped::JournalCheckpoint::EpilogueWord, true);
    Access access(make_session(), &injector);
    const auto base = require_value(access._journal.snapshot(access._guard));
    expect_error(access._journal.execute(access._guard, make_plan(base, _configuration, true)), ErrorCode::OutcomeUnknown);
    EXPECT_EQ(access.word(mapped::journal_offset), static_cast<std::uint64_t>(mapped::JournalControl::Committed));
    injector._active = false;
    require_value(access._journal.recover(access._guard));
    const auto resolved = require_value(access._journal.snapshot(access._guard));
    EXPECT_EQ(resolved._generation, 1u);
    EXPECT_EQ(resolved._revision, 1u);
    EXPECT_EQ(access.word(task_state()), 11u);
    EXPECT_EQ(access.word(task_state() + 8), 22u);
  }
} // namespace
