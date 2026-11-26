// Private native qualification of reserved receipt/Source progress. A fresh
// executable stops at actual journal stores; the parent terminates it, recovers
// through the real Session gate, and validates the complete retained history.
// These fixtures are not examples of the future public receive/acknowledge API.

#include "delivery_progress.h"
#include "delivery_progress_fixture.h"
#include "process_support.h"
#include "result_assertions.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ANGARIA_NATIVE_DELIVERY_PROGRESS_WORKER
#error ANGARIA_NATIVE_DELIVERY_PROGRESS_WORKER must name the progress crash worker
#endif

extern char** environ;

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;
  namespace support = angaria::test_support;

  class ProgressWorker
  {
  public:
    ProgressWorker(const support::DeliveryProgressFixture& fixture, const std::string& mode,
                     mapped::JournalCheckpoint point, std::uint64_t index, TaskId task)
    {
      std::array<int, 2> output{};
      if (::pipe(output.data()) == -1)
      {
        throw std::runtime_error("Cannot create progress worker pipe");
      }
      for (const auto descriptor : output)
      {
        if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == -1)
        {
          ::close(output[0]);
          ::close(output[1]);
          throw std::runtime_error("Cannot mark progress worker pipe close-on-exec");
        }
      }
      posix_spawn_file_actions_t actions;
      auto error = ::posix_spawn_file_actions_init(&actions);
      if (error == 0)
      {
        error = ::posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
        error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, output[0]) : error;
        error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, output[1]) : error;
        const std::array<std::string, 8> values{ANGARIA_NATIVE_DELIVERY_PROGRESS_WORKER,
            mode, fixture._directory.string(), std::to_string(static_cast<unsigned>(point)),
            std::to_string(index), std::to_string(static_cast<unsigned>(task)),
            std::to_string(static_cast<unsigned>(fixture._source)),
            std::to_string(static_cast<unsigned>(fixture._queue))};
        std::array<char*, 9> arguments{};
        for (std::size_t argument = 0; argument != values.size(); ++argument)
        {
          arguments[argument] = const_cast<char*>(values[argument].c_str());
        }
        error = error == 0 ? ::posix_spawn(&_pid, values[0].c_str(), &actions, nullptr,
                                           arguments.data(), environ) : error;
        ::posix_spawn_file_actions_destroy(&actions);
      }
      ::close(output[1]);
      if (error != 0)
      {
        ::close(output[0]);
        throw std::system_error(error, std::generic_category(), "Spawn progress worker");
      }
      _output = output[0];
    }

    ~ProgressWorker()
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

    ProgressWorker(const ProgressWorker&) = delete;
    ProgressWorker& operator=(const ProgressWorker&) = delete;

    void stop()
    {
      char reached{};
      support::receive_exact(_output, &reached, 1,
          {"Progress checkpoint deadline expired", "Progress worker checkpoint unavailable",
           "Progress worker closed before its checkpoint"});
      if (reached != 'R')
      {
        throw std::runtime_error("Progress worker reported an unexpected checkpoint");
      }
      support::terminate(_pid,
          {"Cannot terminate progress worker", "Cannot reap progress worker",
           "Progress worker termination deadline expired", "Progress worker was not killed"});
    }

  private:
    pid_t _pid{-1};
    int _output{-1};
  };

  runtime::DeliveryProgress issue(support::DeliveryProgressFixture& fixture)
  {
    runtime::RuntimeAccess access(fixture._state);
    return support::require_value(runtime::runtime_issue_delivery(access, fixture.consumer_claim(),
        fixture._source, fixture._queue, UtcTimestamp{std::chrono::nanoseconds{-7}}));
  }

  void prepare(support::DeliveryProgressFixture& fixture, const std::string& mode)
  {
    fixture.mark_consumer_attached();
    if (mode != "issue")
    {
      (void)issue(fixture);
    }
  }

  std::vector<std::vector<std::byte>> immutable_history(const support::DeliveryProgressFixture& fixture)
  {
    std::vector<std::vector<std::byte>> result;
    for (const auto& publication : fixture._publications)
    {
      result.push_back(fixture.read(publication._reference + 64,
          static_cast<std::size_t>(publication._receipt - publication._reference - 64)));
    }
    return result;
  }

  void expect_complete(support::DeliveryProgressFixture& fixture, const std::string& mode)
  {
    fixture.validate();
    const auto receipt = fixture.receipt();
    if (mode == "issue")
    {
      EXPECT_EQ(receipt._state, mapped::ReceiptState::Issued);
      EXPECT_EQ(receipt._presence, 1u);
    }
    else if (mode == "acknowledge")
    {
      EXPECT_EQ(receipt._state, mapped::ReceiptState::Acknowledged);
      EXPECT_EQ(receipt._presence, 7u);
      EXPECT_EQ(receipt._acknowledgedAt, UtcTimestamp{std::chrono::nanoseconds{-31}});
      EXPECT_EQ(receipt._terminalAt, receipt._acknowledgedAt);
    }
    else
    {
      EXPECT_EQ(receipt._state, mapped::ReceiptState::Pending);
      EXPECT_EQ(receipt._presence, 1u);
    }
    EXPECT_EQ(receipt._consumerTask, fixture._consumer);
    EXPECT_EQ(receipt._sourceSet, fixture._source);
    EXPECT_EQ(receipt._consumerIncarnation, 1u);
    EXPECT_EQ(receipt._issuance, 1u);
    EXPECT_EQ(receipt._dequeuedAt, UtcTimestamp{std::chrono::nanoseconds{-7}});
    runtime::RuntimeAccess access(fixture._state);
    const auto source = access.read_words(fixture._sourceState, 8);
    EXPECT_EQ(source[0], 1u);
    EXPECT_EQ(source[1], 1u);
    if (mode == "issue")
    {
      EXPECT_EQ(source[5], fixture._publications.front()._receipt);
    }
    else
    {
      EXPECT_TRUE(std::all_of(source.begin() + 2, source.end(), [](std::uint64_t value)
      {
        return value == 0;
      }));
    }
  }

  void qualify_words(const std::string& mode, unsigned count)
  {
    // Every receipt, Queue-root and Source word has an independent crash cut.
    // Full topology validation after recovery also checks no pending hole was
    // skipped and no incomplete receipt/Source pair escaped the operation gate.
    for (unsigned index = 0; index != count; ++index)
    {
      SCOPED_TRACE(mode + "/word/" + std::to_string(index));
      support::DeliveryProgressFixture fixture(2);
      prepare(fixture, mode);
      const auto before = immutable_history(fixture);
      const auto base = support::journal_snapshot(fixture._directory);
      {
        ProgressWorker worker(fixture, mode, mapped::JournalCheckpoint::RedoWord, index, fixture._consumer);
        worker.stop();
      }
      expect_complete(fixture, mode);
      const auto after = support::journal_snapshot(fixture._directory);
      EXPECT_EQ(after._generation, base._generation + 1);
      EXPECT_EQ(after._tail, base._tail);
      EXPECT_EQ(after._accounted, base._accounted);
      EXPECT_EQ(after._fileBytes, base._fileBytes);
      EXPECT_EQ(immutable_history(fixture), before);
    }
  }

  TEST(DeliveryProgressRecoveryTest, IssuanceRepairsEveryReceiptAndSourceWord)
  {
    qualify_words("issue", 15);
  }

  TEST(DeliveryProgressRecoveryTest, AcknowledgmentRepairsEveryReceiptQueueAndSourceWord)
  {
    qualify_words("acknowledge", 17);
  }

  TEST(DeliveryProgressRecoveryTest, ReleaseRepairsEveryReceiptAndSourceWord)
  {
    qualify_words("release", 7);
  }

  TEST(DeliveryProgressRecoveryTest, ExactSourceCleanupRepairsEveryReceiptAndSourceWord)
  {
    qualify_words("source", 7);
  }

  TEST(DeliveryProgressRecoveryTest, PreparedProgressRollsBackAndCommittedProgressReplays)
  {
    for (const auto* mode : {"issue", "acknowledge", "release", "source"})
    {
      for (const auto point : {mapped::JournalCheckpoint::Prepared, mapped::JournalCheckpoint::Committed})
      {
        SCOPED_TRACE(std::string{mode} + "/" + std::to_string(static_cast<unsigned>(point)));
        support::DeliveryProgressFixture fixture(2);
        prepare(fixture, mode);
        const auto oldReceipt = fixture.receipt();
        const auto before = immutable_history(fixture);
        {
          ProgressWorker worker(fixture, mode, point, 0, fixture._consumer);
          worker.stop();
        }
        if (point == mapped::JournalCheckpoint::Prepared)
        {
          fixture.validate();
          EXPECT_EQ(fixture.receipt(), oldReceipt);
        }
        else
        {
          expect_complete(fixture, mode);
        }
        EXPECT_EQ(immutable_history(fixture), before);
      }
    }
  }

  TEST(DeliveryProgressRecoveryTest, RecoveryCanStopRepeatedlyInTheMiddleOfAcknowledgment)
  {
    support::DeliveryProgressFixture fixture(2);
    prepare(fixture, "acknowledge");
    {
      ProgressWorker worker(fixture, "acknowledge", mapped::JournalCheckpoint::RedoWord, 3, fixture._consumer);
      worker.stop();
    }
    for (unsigned repeat = 0; repeat != 2; ++repeat)
    {
      ProgressWorker worker(fixture, "recover", mapped::JournalCheckpoint::RedoWord, repeat, fixture._consumer);
      worker.stop();
    }
    expect_complete(fixture, "acknowledge");
  }

  class ProgressIoFailure final : public mapped::JournalFaultInjector
  {
  public:
    ProgressIoFailure(mapped::JournalCheckpoint point, bool persistent = false)
        : _point(point), _persistent(persistent)
    {
    }

    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t) override
    {
      if (point == _point && (_persistent || _failures == 0))
      {
        ++_failures;
        return Error{ErrorCode::IoError, "Injected delivery progress I/O failure", "test.progress"};
      }
      return std::monostate{};
    }

    unsigned _failures{};

  private:
    mapped::JournalCheckpoint _point;
    bool _persistent{};
  };

  TEST(DeliveryProgressRecoveryTest, ProvenPreparedRollbackReportsIoErrorWithoutCanonicalProgress)
  {
    // The operation reached Prepared but never committed. Its own one-shot
    // recovery proves rollback, so the caller receives an ordinary I/O error
    // and can trust that no new receipt or issuance has been installed.
    support::DeliveryProgressFixture fixture(2);
    prepare(fixture, "issue");
    const auto oldReceipt = fixture.receipt();
    const auto before = immutable_history(fixture);
    const auto base = support::journal_snapshot(fixture._directory);
    ProgressIoFailure failure(mapped::JournalCheckpoint::Prepared);
    {
      runtime::RuntimeAccess access(fixture._state, true, &failure);
      const auto source = access.read_words(fixture._sourceState, 8);
      support::expect_error(runtime::runtime_issue_delivery(access, fixture.consumer_claim(),
          fixture._source, fixture._queue, UtcTimestamp{std::chrono::nanoseconds{-7}}), ErrorCode::IoError);
      EXPECT_EQ(access.read_words(fixture._sourceState, 8), source);
    }
    EXPECT_EQ(failure._failures, 1u);
    fixture.validate();
    EXPECT_EQ(fixture.receipt(), oldReceipt);
    EXPECT_EQ(support::journal_snapshot(fixture._directory), base);
    EXPECT_EQ(immutable_history(fixture), before);
  }

  TEST(DeliveryProgressRecoveryTest, OneShotPostcommitFailureReturnsTheRecoveredIssuance)
  {
    // Committed is already acceptance authority. A recoverable I/O error after
    // that point returns the completed issuance; retry returns that same pin
    // rather than consuming a second counter or changing its dequeue time.
    support::DeliveryProgressFixture fixture(2);
    prepare(fixture, "issue");
    const auto base = support::journal_snapshot(fixture._directory);
    ProgressIoFailure failure(mapped::JournalCheckpoint::Committed);
    runtime::DeliveryProgress accepted;
    {
      runtime::RuntimeAccess access(fixture._state, true, &failure);
      accepted = support::require_value(runtime::runtime_issue_delivery(access, fixture.consumer_claim(),
          fixture._source, fixture._queue, UtcTimestamp{std::chrono::nanoseconds{-7}}));
    }
    EXPECT_EQ(failure._failures, 1u);
    expect_complete(fixture, "issue");
    const auto settled = support::journal_snapshot(fixture._directory);
    EXPECT_EQ(settled._generation, base._generation + 1);
    {
      runtime::RuntimeAccess access(fixture._state);
      const auto repeated = support::require_value(runtime::runtime_issue_delivery(access, fixture.consumer_claim(),
          fixture._source, fixture._queue, UtcTimestamp{std::chrono::nanoseconds{99}}));
      EXPECT_EQ(repeated, accepted);
    }
    EXPECT_EQ(support::journal_snapshot(fixture._directory), settled);
  }

  TEST(DeliveryProgressRecoveryTest, UnsettledAcknowledgmentReportsUnknownAndLaterRecoveryDoesNotDuplicateIt)
  {
    // Both the original apply and its immediate repair fail at a real redo
    // store. The caller must treat OutcomeUnknown as uncertain progress and
    // stop using the old view until a later gate owner settles the journal.
    support::DeliveryProgressFixture fixture(2);
    fixture.mark_consumer_attached();
    const auto delivered = issue(fixture);
    const auto before = immutable_history(fixture);
    const auto base = support::journal_snapshot(fixture._directory);
    ProgressIoFailure failure(mapped::JournalCheckpoint::RedoWord, true);
    {
      runtime::RuntimeAccess access(fixture._state, true, &failure);
      support::expect_error(runtime::runtime_acknowledge_delivery(access, delivered._token,
          UtcTimestamp{std::chrono::nanoseconds{-31}}), ErrorCode::OutcomeUnknown);
    }
    EXPECT_GE(failure._failures, 2u);
    expect_complete(fixture, "acknowledge");
    const auto settled = support::journal_snapshot(fixture._directory);
    EXPECT_EQ(settled._generation, base._generation + 1);
    EXPECT_EQ(settled._tail, base._tail);
    const auto image = fixture.image();
    {
      runtime::RuntimeAccess access(fixture._state);
      support::expect_error(runtime::runtime_acknowledge_delivery(access, delivered._token,
          UtcTimestamp{std::chrono::nanoseconds{100}}), ErrorCode::StaleDelivery);
    }
    EXPECT_EQ(support::journal_snapshot(fixture._directory), settled);
    EXPECT_EQ(fixture.image(), image);
    EXPECT_EQ(immutable_history(fixture), before);
  }

  class CountRedo final : public mapped::JournalFaultInjector
  {
  public:
    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t) override
    {
      if (point == mapped::JournalCheckpoint::RedoWord)
      {
        ++_words;
      }
      return std::monostate{};
    }

    unsigned _words{};
  };

  TEST(DeliveryProgressRecoveryTest, ReplacementCleanupRepairsEveryOldIdentityAndSuccessorWord)
  {
    unsigned count{};
    {
      support::DeliveryProgressFixture control(2);
      prepare(control, "replace");
      CountRedo counter;
      runtime::RuntimeAccess access(control._state, true, &counter);
      support::require_value(access._journal.execute(access._guard,
          support::progress_claim_plan(access, control._consumer, true)));
      count = counter._words;
    }
    ASSERT_GT(count, 10u);
    ASSERT_LE(count, 32u);
    for (unsigned index = 0; index != count; ++index)
    {
      SCOPED_TRACE(index);
      support::DeliveryProgressFixture fixture(2);
      prepare(fixture, "replace");
      const auto before = immutable_history(fixture);
      {
        ProgressWorker worker(fixture, "replace", mapped::JournalCheckpoint::RedoWord, index, fixture._consumer);
        worker.stop();
      }
      expect_complete(fixture, "replace");
      runtime::RuntimeAccess access(fixture._state);
      EXPECT_EQ(access.read_words(fixture._consumerState, 1)[0], 2u);
      EXPECT_EQ(immutable_history(fixture), before);
    }
  }

  TEST(DeliveryProgressRecoveryTest, ParentClaimCleanupAndRepeatedReplayWorkAtTheExactLogCeiling)
  {
    support::DeliveryProgressFixture fixture(2, TaskKind::Thread);
    prepare(fixture, "claim");
    fixture.fill_log();
    const auto before = immutable_history(fixture);
    const auto base = support::journal_snapshot(fixture._directory);
    ASSERT_EQ(base._tail, fixture._configuration._maxLogBytes);
    {
      ProgressWorker worker(fixture, "claim", mapped::JournalCheckpoint::Committed, 0, fixture._parent);
      worker.stop();
    }
    for (unsigned repeat = 0; repeat != 2; ++repeat)
    {
      ProgressWorker worker(fixture, "recover", mapped::JournalCheckpoint::RedoWord, 0, fixture._parent);
      worker.stop();
    }
    expect_complete(fixture, "claim");
    runtime::RuntimeAccess access(fixture._state);
    EXPECT_EQ(access._snapshot._tail, base._tail);
    EXPECT_EQ(access._snapshot._accounted, base._accounted);
    EXPECT_EQ(access._snapshot._fileBytes, base._fileBytes);
    EXPECT_EQ(access.read_words(fixture._parentState + 48, 1)[0] & 1, 0u);
    EXPECT_EQ(access.read_words(fixture._consumerState + 48, 1)[0] & 1, 0u);
    EXPECT_EQ(immutable_history(fixture), before);
  }
} // namespace
