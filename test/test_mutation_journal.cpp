#include "mutation_journal.h"
#include "native_atomic.h"

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

  std::uint64_t section_offset(const mapped::SuperblockConfiguration& configuration,
                                mapped::SectionKind kind)
  {
    const auto layout = mapped::make_fixed_layout(configuration._limits);
    if (!layout)
    {
      throw std::runtime_error("test configuration has no layout");
    }
    for (const auto& section : layout->_sections)
    {
      if (section._kind == kind)
      {
        return section._offset;
      }
    }
    throw std::runtime_error("test section is missing");
  }

  mapped::RedoCommand write_word(std::uint64_t target, std::uint64_t value)
  {
    mapped::RedoCommand command;
    command._target = target;
    command._payload.resize(8);
    put_integer(command._payload, 0, value);
    return command;
  }

  mapped::TransactionPlan make_plan(const mapped::JournalSnapshot& base,
                                    const mapped::SuperblockConfiguration& configuration,
                                    bool append = false)
  {
    mapped::TransactionPlan plan;
    plan._base = base;
    plan._kind = append ? mapped::JournalKind::Append : mapped::JournalKind::FixedState;
    plan._newRevision = base._revision + (append ? 1 : 0);
    const auto task_state = section_offset(configuration, mapped::SectionKind::TaskState);
    plan._commands.push_back(write_word(task_state, 11));
    plan._commands.push_back(write_word(task_state + 8, 22));
    if (append)
    {
      mapped::JournalRecord record;
      record._kind = 2;
      record._immutable.resize(76);
      put_integer(record._immutable, 0, 1);
      put_integer(record._immutable, 8, 1, 2);
      put_integer(record._immutable, 24, plan._newRevision);
      put_integer(record._immutable, 32, base._tail + 64 + 72);
      put_integer(record._immutable, 40, 4);
      put_integer(record._immutable, 64, 1, 2);
      record._immutable[72] = std::byte{'t'};
      record._immutable[73] = std::byte{'a'};
      record._immutable[74] = std::byte{'s'};
      record._immutable[75] = std::byte{'k'};
      plan._records.push_back(std::move(record));
      mapped::RedoCommand index;
      index._target = section_offset(configuration, mapped::SectionKind::TaskIndex);
      index._payload.resize(32);
      put_integer(index._payload, 0, 1);
      put_integer(index._payload, 8, base._tail);
      put_integer(index._payload, 16, plan._newRevision);
      plan._commands.push_back(std::move(index));
    }
    return plan;
  }

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

  class FailingInjector : public mapped::JournalFaultInjector
  {
  public:
    FailingInjector(mapped::JournalCheckpoint point, bool persistent = false)
        : _point(point), _persistent(persistent)
    {
    }

    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t index) override
    {
      if (_active && point == _point && index == 0 && (_persistent || !_fired))
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

    fs::path _root;
    mapped::SuperblockConfiguration _configuration;
  };

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
    expect_error(access._journal.execute(access._guard, compact), ErrorCode::UnsupportedPolicy);
    EXPECT_EQ(access.image(), before);
  }

  TEST_F(MutationJournalTest, IdleIgnoresAnInterruptedOrGarbageJournalBody)
  {
    Access access(make_session());
    const auto before = require_value(access._journal.snapshot(access._guard));
    std::vector<std::byte> garbage(mapped::journal_bytes - 8, std::byte{0x5a});
    require_value(access.file().write_exact(mapped::journal_offset + 8, garbage));
    require_value(access._journal.recover(access._guard));
    EXPECT_EQ(require_value(access._journal.snapshot(access._guard)), before);
  }

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
