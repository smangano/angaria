// Kill an independently executed consumer at real loss-journal stores. The
// public replacement sees either the original pending prefix or its committed
// loss, never an incomplete mixture. Native reads below are evidence only.
#include "source_fixture.h"

#include "delivery_words.h"
#include "mapped_payload.h"
#include "native_fixture.h"
#include "process_support.h"
#include "topology_state.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ANGARIA_NATIVE_LOSS_WORKER
#error ANGARIA_NATIVE_LOSS_WORKER must name the independent loss worker
#endif

extern char** environ;

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = runtime::mapped;
  namespace support = angaria::test_support;
  namespace fs = std::filesystem;
  using support::require_value;

  class LossWorker
  {
  public:
    LossWorker(const fs::path& directory, std::string_view operation,
                  mapped::JournalCheckpoint point, std::uint64_t index = 0)
    {
      std::array<int, 2> output{};
      if (::pipe(output.data()) == -1)
      {
        throw std::runtime_error("Cannot create loss worker pipe");
      }
      for (const auto descriptor : output)
      {
        if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == -1)
        {
          ::close(output[0]);
          ::close(output[1]);
          throw std::runtime_error("Cannot mark loss worker pipe close-on-exec");
        }
      }
      posix_spawn_file_actions_t actions;
      auto error = ::posix_spawn_file_actions_init(&actions);
      if (error == 0)
      {
        error = ::posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
        error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, output[0]) : error;
        error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, output[1]) : error;
        const std::array<std::string, 5> values{ANGARIA_NATIVE_LOSS_WORKER, directory.string(),
            std::string{operation}, std::to_string(static_cast<unsigned>(point)), std::to_string(index)};
        std::array<char*, 6> arguments{};
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
        throw std::system_error(error, std::generic_category(), "Spawn loss worker");
      }
      _output = output[0];
    }

    ~LossWorker()
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

    LossWorker(const LossWorker&) = delete;
    LossWorker& operator=(const LossWorker&) = delete;

    void stop()
    {
      char stage{};
      support::receive_exact(_output, &stage, sizeof(stage),
          {"Loss checkpoint deadline expired", "Loss worker checkpoint unavailable",
           "Loss worker closed before its checkpoint"});
      if (stage != 'R')
      {
        throw std::runtime_error("Loss worker reported an unexpected checkpoint");
      }
      support::terminate(_pid,
          {"Cannot terminate loss worker", "Cannot reap loss worker",
           "Loss worker termination deadline expired", "Loss worker was not killed"});
    }

  private:
    pid_t _pid{-1};
    int _output{-1};
  };

  class LossRecoveryTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-loss-crash-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    void create(const fs::path& directory)
    {
      auto scenario = support::create_source_scenario(directory, true,
          TaskKind::Process, DeliveryPolicy::AllowLoss);
      auto writer = require_value(scenario._created._creator.open_writer(scenario._queue));
      for (std::uint32_t value = 1; value <= 5; ++value)
      {
        support::send_source_value(scenario, writer, value);
      }
    }

    void verify(const fs::path& directory, bool committed)
    {
      auto session = require_value(Session::attach(directory));
      auto consumer = require_value(session.attach_task("Consumer"));
      auto source = require_value(consumer.open_source_set("Inputs"));
      auto event = source.receive();
      if (!committed)
      {
        // Prepared loss created no terminal state; replacement can still report
        // the original gap. A committed-but-unreturned gap is not emitted again.
        ASSERT_TRUE(std::holds_alternative<DeliveryGap>(event));
        EXPECT_EQ(std::get<DeliveryGap>(event)._firstSequence, 1u);
        EXPECT_EQ(std::get<DeliveryGap>(event)._lastSequence, 3u);
        event = source.receive();
      }
      ASSERT_TRUE(std::holds_alternative<Delivery>(event));
      const auto delivery = std::get<Delivery>(event);
      EXPECT_EQ(delivery._token._sequence, 4u);
      require_value(source.acknowledge(delivery._token));
      require_value(source.close());

      runtime::RuntimeAccess access(support::open_runtime_state(directory));
      (void)runtime::runtime_topology(access);
      unsigned lost{};
      std::optional<UtcTimestamp> captured;
      for (const auto reference : access.record_references(9))
      {
        const auto publication = runtime::runtime_publication_metadata(access, reference);
        const auto& row = publication._destinations.front();
        const auto receipt = require_value(mapped::decode_receipt_words(
            access.read_words(row._receiptReference, 16), mapped::ReceiptProfile::AllowLoss));
        if (row._queueSequence <= 3)
        {
          EXPECT_EQ(receipt._state, mapped::ReceiptState::Lost);
          EXPECT_EQ(receipt._presence, 4u);
          if (!captured)
          {
            captured = receipt._terminalAt;
          }
          EXPECT_EQ(receipt._terminalAt, *captured);
          ++lost;
        }
      }
      EXPECT_EQ(lost, 3u);
    }

    fs::path _root;
  };

  TEST_F(LossRecoveryTest, PreparedLossRollsBackAndCommittedLossSurvivesConsumerDeath)
  {
    for (const auto point : {mapped::JournalCheckpoint::Prepared, mapped::JournalCheckpoint::Committed,
                             mapped::JournalCheckpoint::Applied, mapped::JournalCheckpoint::Idle})
    {
      SCOPED_TRACE(static_cast<unsigned>(point));
      const auto directory = _root / std::to_string(static_cast<unsigned>(point));
      create(directory);
      const auto before = support::journal_snapshot(directory);
      {
        LossWorker worker(directory, "receive", point);
        worker.stop();
      }
      verify(directory, point != mapped::JournalCheckpoint::Prepared);
      const auto after = support::journal_snapshot(directory);
      EXPECT_EQ(after._tail, before._tail);
      EXPECT_EQ(after._revision, before._revision);
    }
  }

  TEST_F(LossRecoveryTest, InterruptedLossRecoveryCanBeStoppedAndRepeatedAgain)
  {
    const auto directory = _root / "repeated";
    create(directory);
    {
      LossWorker worker(directory, "receive", mapped::JournalCheckpoint::Committed);
      worker.stop();
    }
    for (const auto index : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{2}})
    {
      LossWorker recovery(directory, "recover", mapped::JournalCheckpoint::RedoWord, index);
      recovery.stop();
    }
    verify(directory, true);
  }

  TEST_F(LossRecoveryTest, EveryStreamedReceiptAndQueueWordCanBeInterrupted)
  {
    // LoseRange installs seven absolute words in each of three stale receipts,
    // followed by the terminal prefix and pending link. Stopping at every store
    // also covers Lost becoming visible before its captured timestamp/presence.
    for (std::uint64_t index = 0; index < 23; ++index)
    {
      SCOPED_TRACE(index);
      const auto directory = _root / ("word-" + std::to_string(index));
      create(directory);
      {
        LossWorker worker(directory, "receive", mapped::JournalCheckpoint::RedoWord, index);
        worker.stop();
      }
      verify(directory, true);
    }
  }
}
