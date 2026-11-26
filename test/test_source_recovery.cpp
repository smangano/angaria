// A separately executed consumer uses public APIs and is then killed. Its
// replacement must recover unfinished work, while a completed acknowledgment
// stays complete. The private snapshot only checks that progress did not append.
#include "source_fixture.h"

#include "native_fixture.h"
#include "process_support.h"
#include "result_assertions.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
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

#ifndef ANGARIA_NATIVE_SOURCE_WORKER
#error ANGARIA_NATIVE_SOURCE_WORKER must name the independent consumer worker
#endif

extern char** environ;

namespace
{
  using namespace angaria;
  namespace support = angaria::test_support;
  namespace fs = std::filesystem;

  class SourceWorker
  {
  public:
    SourceWorker(const fs::path& directory, SourceQueueSetId source, std::string_view operation)
    {
      std::array<int, 2> output{};
      if (::pipe(output.data()) == -1)
      {
        throw std::runtime_error("Cannot create Source worker pipe");
      }
      for (const auto descriptor : output)
      {
        if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == -1)
        {
          ::close(output[0]);
          ::close(output[1]);
          throw std::runtime_error("Cannot mark Source worker pipe close-on-exec");
        }
      }
      posix_spawn_file_actions_t actions;
      auto error = ::posix_spawn_file_actions_init(&actions);
      if (error == 0)
      {
        error = ::posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
        error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, output[0]) : error;
        error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, output[1]) : error;
        const std::array<std::string, 4> values{ANGARIA_NATIVE_SOURCE_WORKER,
            directory.string(), std::to_string(static_cast<unsigned>(source)), std::string{operation}};
        std::array<char*, 5> arguments{};
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
        throw std::system_error(error, std::generic_category(), "Spawn Source worker");
      }
      _output = output[0];
    }

    ~SourceWorker()
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

    SourceWorker(const SourceWorker&) = delete;
    SourceWorker& operator=(const SourceWorker&) = delete;

    DeliveryToken stop(SessionId session, TaskId consumer, SourceQueueSetId source, QueueId queue)
    {
      char stage{};
      receive(&stage, sizeof(stage));
      if (stage != 'A')
      {
        throw std::runtime_error("Source worker did not report its public operation");
      }
      std::array<std::uint64_t, 3> words{};
      receive(words.data(), sizeof(words));
      receive(&stage, sizeof(stage));
      if (stage != 'R')
      {
        throw std::runtime_error("Source worker did not reach its termination checkpoint");
      }
      support::terminate(_pid,
          {"Cannot terminate Source worker", "Cannot reap Source worker",
           "Source worker termination deadline expired", "Source worker was not killed"});
      return {{session, consumer, words[0]}, source, queue, words[1], words[2]};
    }

  private:
    void receive(void* output, std::size_t count)
    {
      support::receive_exact(_output, output, count,
          {"Source worker readiness deadline expired", "Source worker report unavailable",
           "Source worker closed before reporting"});
    }

    pid_t _pid{-1};
    int _output{-1};
  };

  class SourceRecoveryTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-source-process-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    struct Setup
    {
      SessionId _session{};
      TaskId _consumer{};
      SourceQueueSetId _source{};
      QueueId _queue{};
      SchemaId _schema{};
      std::array<PublicationId, 3> _publications;
    };

    Setup create(const fs::path& directory)
    {
      auto scenario = support::create_source_scenario(directory);
      auto writer = support::require_value(scenario._created._creator.open_writer(scenario._queue));
      std::array<PublicationId, 3> publications;
      for (std::uint32_t value = 1; value != 4; ++value)
      {
        publications[value - 1] = support::send_source_value(scenario, writer, value);
      }
      // Three messages overrun the ring of two. The consumer must use retained
      // Queue history even before it or its replacement processes anything.
      return {scenario._created._session.identity(), scenario._consumer, scenario._source,
              scenario._queue, scenario._schema, publications};
    }

    fs::path _root;
  };

  TEST_F(SourceRecoveryTest, ReplacementRedeliversUnfinishedMessagesAndPreservesCompletedAcknowledgments)
  {
    for (const std::string_view operation : {"receive", "acknowledge", "release", "close"})
    {
      SCOPED_TRACE(operation);
      const auto directory = _root / std::string{operation};
      const auto setup = create(directory);
      const auto before = support::journal_snapshot(directory);
      DeliveryToken old;
      {
        SourceWorker worker(directory, setup._source, operation);
        old = worker.stop(setup._session, setup._consumer, setup._source, setup._queue);
      }
      auto session = support::require_value(Session::attach(directory));
      auto replacement = support::require_value(session.attach_task("Consumer"));
      EXPECT_GT(replacement.claim()._incarnation, old._consumer._incarnation);
      auto source = support::require_value(replacement.open_source_set(setup._source));
      const auto codec = support::require_value(BinaryPayloadCodec::from_registry(session.registry(), setup._schema, 4));
      const auto first_sequence = operation == "acknowledge" ? 2u : 1u;
      for (auto sequence = first_sequence; sequence != 4; ++sequence)
      {
        auto received = source.receive();
        ASSERT_TRUE(std::holds_alternative<Delivery>(received));
        const auto delivery = std::get<Delivery>(std::move(received));
        EXPECT_EQ(delivery._token._sequence, sequence);
        EXPECT_EQ(delivery._message._header._publication, setup._publications[sequence - 1]);
        EXPECT_EQ(delivery._message._header._consumer, replacement.claim());
        const auto saved = support::require_value(delivery._message.copy_message(4));
        EXPECT_EQ(std::get<std::uint32_t>(support::require_value(saved.decode(codec)).front()), sequence);
        // The old process's token cannot complete a replacement's new issuance,
        // even when its sequence or reset issuance counter happens to match.
        support::expect_error(source.acknowledge(old), ErrorCode::StaleDelivery);
        support::require_value(source.acknowledge(delivery._token));
      }
      EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(source.receive()));
      support::require_value(source.close());
      const auto after = support::journal_snapshot(directory);
      EXPECT_EQ(after._tail, before._tail);
      EXPECT_EQ(after._revision, before._revision);
      // Consumer completion leaves original acceptance lookup intact.
      auto producer = support::require_value(session.attach_task("Producer"));
      for (const auto publication : setup._publications)
      {
        EXPECT_EQ(support::require_value(producer.publication_status(publication))._status,
                  AcceptanceStatus::Accepted);
      }
    }
  }
} // namespace
