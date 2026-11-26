// Private storage qualification: committed Publication payloads are mapped
// while later journal appends fail or stop in another executable. These are
// framing-only fixtures, not examples of the still-planned public delivery API.

#include "journal_fixture.h"
#include "mapped_payload.h"
#include "process_support.h"
#include "publication_storage_fixture.h"
#include "result_assertions.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
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
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;
  namespace support = angaria::test_support;

  // One-shot precommit I/O failure allows execute's own recovery to finish,
  // proving rollback without deliberately obstructing the repair itself.
  class PayloadAppendFailure final : public mapped::JournalFaultInjector
  {
  public:
    explicit PayloadAppendFailure(mapped::JournalCheckpoint point) : _point(point)
    {
    }

    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t) override
    {
      if (!_fired && point == _point)
      {
        _fired = true;
        return Error{ErrorCode::IoError, "Injected append failure with an older payload mapped", "test.payload"};
      }
      return std::monostate{};
    }

    bool _fired{};

  private:
    mapped::JournalCheckpoint _point;
  };

  // This owner selects the existing journal worker's command/checkpoint. Shared
  // pipe/deadline helpers handle transport; only process lifetime stays local.
  class PayloadJournalWorker
  {
  public:
    PayloadJournalWorker(const std::filesystem::path& directory, const char* mode,
                           mapped::JournalCheckpoint point)
    {
      std::array<int, 2> output{};
      if (::pipe(output.data()) == -1)
      {
        throw std::runtime_error("Cannot create payload journal worker pipe");
      }
      for (const auto descriptor : output)
      {
        if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == -1)
        {
          ::close(output[0]);
          ::close(output[1]);
          throw std::runtime_error("Cannot mark payload journal worker pipe close-on-exec");
        }
      }
      posix_spawn_file_actions_t actions;
      auto error = ::posix_spawn_file_actions_init(&actions);
      if (error == 0)
      {
        error = ::posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
        error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, output[0]) : error;
        error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, output[1]) : error;
        const std::array<std::string, 5> values{
            ANGARIA_NATIVE_JOURNAL_WORKER, mode, directory.string(),
            std::to_string(static_cast<unsigned>(point)), "0"};
        std::array<char*, 6> arguments{};
        for (std::size_t index = 0; index < values.size(); ++index)
        {
          arguments[index] = const_cast<char*>(values[index].c_str());
        }
        error = error == 0 ? ::posix_spawn(&_pid, values[0].c_str(), &actions, nullptr,
                                           arguments.data(), environ) : error;
        ::posix_spawn_file_actions_destroy(&actions);
      }
      ::close(output[1]);
      if (error != 0)
      {
        ::close(output[0]);
        throw std::system_error(error, std::generic_category(), "Spawn payload journal worker");
      }
      _output = output[0];
    }

    ~PayloadJournalWorker()
    {
      if (_pid > 0)
      {
        ::kill(_pid, SIGKILL);
        int status{};
        while (::waitpid(_pid, &status, 0) == -1 && errno == EINTR)
        {
        }
      }
      support::close_descriptor(_output);
    }

    PayloadJournalWorker(const PayloadJournalWorker&) = delete;
    PayloadJournalWorker& operator=(const PayloadJournalWorker&) = delete;

    void await_checkpoint()
    {
      char reached{};
      support::receive_exact(_output, &reached, 1,
          {"Payload worker checkpoint deadline expired", "Payload worker checkpoint unavailable",
           "Payload worker closed before its checkpoint"});
      if (reached != 'R')
      {
        throw std::runtime_error("Payload worker reported an unexpected checkpoint");
      }
    }

    void kill_at_checkpoint()
    {
      support::terminate(_pid,
          {"Cannot terminate payload journal worker", "Cannot reap payload journal worker",
           "Payload journal worker termination deadline expired", "Payload worker was not killed"});
    }

  private:
    pid_t _pid{-1};
    int _output{-1};
  };

  void expect_payload(std::span<const std::byte> bytes, const std::byte* address)
  {
    ASSERT_EQ(bytes.size(), 32771u);
    EXPECT_EQ(bytes.data(), address);
    EXPECT_EQ(bytes.front(), std::byte{0x41});
    EXPECT_EQ(bytes.back(), std::byte{0x5a});
    EXPECT_TRUE(std::all_of(bytes.begin() + 1, bytes.end() - 1,
        [](std::byte value)
        {
          return value == std::byte{0xa5};
        }));
  }

  // A committed mapping ends before every later append's rollback floor.
  // Neither its lease nor partial provisional materialization may force an
  // uncertain outcome or invalidate a previously borrowed application span.
  TEST(PayloadRollbackTest, PrecommitFailuresPreserveMappedCommittedBytes)
  {
    const std::array cuts{mapped::JournalCheckpoint::Prepared, mapped::JournalCheckpoint::FileGrowth,
                          mapped::JournalCheckpoint::RecordBodyWord, mapped::JournalCheckpoint::RecordReady};
    for (const auto cut : cuts)
    {
      SCOPED_TRACE(static_cast<unsigned>(cut));
      support::PublicationStorageFixture fixture(32771);
      auto payload = [&]
      {
        runtime::RuntimeAccess access(fixture._state, false);
        return support::require_value(runtime::map_publication_payload(access, fixture._publication._reference));
      }();
      const auto borrowed = support::require_value(payload.bytes());
      const auto* address = borrowed.data();
      PayloadAppendFailure failure(cut);
      {
        runtime::RuntimeAccess access(fixture._state, false, &failure);
        const auto plan = support::make_plan(access._snapshot, fixture._configuration, true);
        support::expect_error(access._journal.execute(access._guard, plan), ErrorCode::IoError);
        EXPECT_TRUE(failure._fired);
        EXPECT_EQ(support::require_value(access._journal.snapshot(access._guard)), fixture._committedSnapshot);
      }
      expect_payload(borrowed, address);
      expect_payload(support::require_value(payload.bytes()), address);
    }
  }

  // The exec worker cannot see the parent's process-local mapping leases.
  // Safety therefore comes from the validated committed allocation floor:
  // growth, termination, and repeated Prepared recovery remove only new bytes.
  TEST(PayloadRollbackTest, IndependentAppendAndRepeatedRollbackPreserveBorrowedPayload)
  {
    support::PublicationStorageFixture fixture(32771);
    auto payload = [&]
    {
      runtime::RuntimeAccess access(fixture._state, false);
      return support::require_value(runtime::map_publication_payload(access, fixture._publication._reference));
    }();
    const auto borrowed = support::require_value(payload.bytes());
    const auto* address = borrowed.data();
    {
      PayloadJournalWorker writer(fixture._directory, "append", mapped::JournalCheckpoint::FileGrowth);
      writer.await_checkpoint();
      // No RuntimeAccess/gate is held here; the writer still owns the gate.
      expect_payload(borrowed, address);
      writer.kill_at_checkpoint();
    }
    for (unsigned repeat = 0; repeat < 2; ++repeat)
    {
      PayloadJournalWorker recovery(fixture._directory, "recover", mapped::JournalCheckpoint::Rollback);
      recovery.await_checkpoint();
      expect_payload(borrowed, address);
      recovery.kill_at_checkpoint();
    }
    {
      runtime::RuntimeAccess access(fixture._state, false);
      EXPECT_EQ(access._snapshot, fixture._committedSnapshot);
    }
    expect_payload(borrowed, address);
    expect_payload(support::require_value(payload.bytes()), address);
  }

  // Once the writer commits, recovery retains its later append rather than
  // rolling it back. Older immutable addresses remain unchanged in either case.
  TEST(PayloadRollbackTest, IndependentCommittedAppendPreservesOlderBorrowedPayload)
  {
    support::PublicationStorageFixture fixture(32771);
    auto payload = [&]
    {
      runtime::RuntimeAccess access(fixture._state, false);
      return support::require_value(runtime::map_publication_payload(access, fixture._publication._reference));
    }();
    const auto borrowed = support::require_value(payload.bytes());
    const auto* address = borrowed.data();
    {
      PayloadJournalWorker writer(fixture._directory, "append", mapped::JournalCheckpoint::Committed);
      writer.await_checkpoint();
      expect_payload(borrowed, address);
      writer.kill_at_checkpoint();
    }
    {
      runtime::RuntimeAccess access(fixture._state, false);
      EXPECT_GT(access._snapshot._tail, fixture._committedSnapshot._tail);
      EXPECT_EQ(access._snapshot._generation, fixture._committedSnapshot._generation + 1);
    }
    expect_payload(borrowed, address);
    expect_payload(support::require_value(payload.bytes()), address);
  }
} // namespace
