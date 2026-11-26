// Native process-death qualification. A separately executed producer is killed
// at actual append/commit stores. A replacement Task then uses the PUBLIC
// publication_status() API to settle its predecessor's outcome. The worker's
// injection seam and the final native record inspection are test-only details.

#include "angaria/runtime.h"
#include "mapped_payload.h"
#include "native_fixture.h"
#include "process_support.h"
#include "result_assertions.h"
#include "topology_state.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ANGARIA_NATIVE_PUBLICATION_WORKER
#error ANGARIA_NATIVE_PUBLICATION_WORKER must name the publication crash worker
#endif

extern char** environ;

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;
  namespace support = angaria::test_support;
  namespace fs = std::filesystem;

  class PublicationWorker
  {
  public:
    PublicationWorker(const fs::path& directory, QueueId queue,
                        mapped::JournalCheckpoint point, std::uint64_t index = 0)
    {
      std::array<int, 2> output{};
      if (::pipe(output.data()) == -1)
      {
        throw std::runtime_error("Cannot create publication worker pipe");
      }
      for (const auto descriptor : output)
      {
        if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == -1)
        {
          ::close(output[0]);
          ::close(output[1]);
          throw std::runtime_error("Cannot mark publication worker pipe close-on-exec");
        }
      }
      posix_spawn_file_actions_t actions;
      auto error = ::posix_spawn_file_actions_init(&actions);
      if (error == 0)
      {
        error = ::posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
        error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, output[0]) : error;
        error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, output[1]) : error;
        const std::array<std::string, 5> values{ANGARIA_NATIVE_PUBLICATION_WORKER,
            directory.string(), std::to_string(static_cast<unsigned>(queue)),
            std::to_string(static_cast<unsigned>(point)), std::to_string(index)};
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
        throw std::system_error(error, std::generic_category(), "Spawn publication worker");
      }
      _output = output[0];
    }

    ~PublicationWorker()
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

    PublicationWorker(const PublicationWorker&) = delete;
    PublicationWorker& operator=(const PublicationWorker&) = delete;

    PublicationId stop(SessionId session)
    {
      char stage{};
      receive(&stage, sizeof(stage));
      if (stage != 'I')
      {
        throw std::runtime_error("Publication worker did not report its issued identity");
      }
      std::array<std::uint64_t, 2> words{};
      receive(words.data(), sizeof(words));
      receive(&stage, sizeof(stage));
      if (stage != 'R')
      {
        throw std::runtime_error("Publication worker reported an unexpected checkpoint");
      }
      support::terminate(_pid,
          {"Cannot terminate publication worker", "Cannot reap publication worker",
           "Publication worker termination deadline expired", "Publication worker was not killed"});
      return PublicationId{TaskClaim{session, TaskId{1}, words[0]}, words[1]};
    }

  private:
    void receive(void* output, std::size_t count)
    {
      support::receive_exact(_output, output, count,
          {"Publication checkpoint deadline expired", "Publication worker checkpoint unavailable",
           "Publication worker closed before its checkpoint"});
    }

    pid_t _pid{-1};
    int _output{-1};
  };

  class PublicationRecoveryTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-publication-crash-XXXXXX").string();
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
      QueueId _queue{};
    };

    Setup create(const fs::path& directory) const
    {
      SessionOptions options;
      options._name = "Native publication crash";
      options._directory = directory;
      options._maxLogBytes = 1048576;
      options._limits = CapacityLimits::empty().tasks(2).queues(2).source_sets(2)
          .memberships_per_set(2).message_schemas(1);
      options._defaults._ringCapacity = 2;
      TaskDescriptor descriptor;
      descriptor._name = "Producer";
      auto created = support::require_value(Session::create(options, descriptor));
      auto registry = created._creator.registry();
      SourceQueueSetDescriptor source;
      source._name = "Inputs";
      source._owner = TaskId{1};
      const auto source_id = support::require_value(registry.register_source_set(source));
      QueueDescriptor queue;
      queue._name = "Updates";
      queue._producer = TaskId{1};
      queue._sourceSet = source_id;
      const auto queue_id = support::require_value(registry.register_queue(queue));
      support::require_value(registry.activate_source_set(source_id));
      support::require_value(registry.activate_queue(queue_id));
      MessageDescriptor message;
      message._name = "Notice";
      message._schema = SchemaId{MessageType{1}, 1};
      support::require_value(registry.register_message(message));
      // Closing the real creator releases its OS claim. The separately executed
      // worker must acquire its own successor claim before issuing an identity.
      return Setup{created._session.identity(), queue_id};
    }

    fs::path _root;
  };

  TEST_F(PublicationRecoveryTest, PreparedAppendRollsBackAndCommittedAppendSurvivesProcessDeath)
  {
    for (const auto point : {mapped::JournalCheckpoint::Prepared, mapped::JournalCheckpoint::Committed,
                             mapped::JournalCheckpoint::RedoWord, mapped::JournalCheckpoint::RecordCommitted,
                             mapped::JournalCheckpoint::Applied, mapped::JournalCheckpoint::Idle})
    {
      SCOPED_TRACE(static_cast<unsigned>(point));
      const auto directory = _root / std::to_string(static_cast<unsigned>(point));
      const auto setup = create(directory);
      const auto before = support::journal_snapshot(directory);
      PublicationId publication;
      {
        PublicationWorker worker(directory, setup._queue, point);
        publication = worker.stop(setup._session);
      }

      auto session = support::require_value(Session::attach(directory));
      auto successor = support::require_value(session.attach_task(TaskId{1}));
      ASSERT_GT(successor.claim()._incarnation, publication._producer._incarnation);
      const auto outcome = support::require_value(successor.publication_status(publication));
      const auto prepared_only = point == mapped::JournalCheckpoint::Prepared;
      EXPECT_EQ(outcome._status, prepared_only ? AcceptanceStatus::Rejected : AcceptanceStatus::Accepted);
      EXPECT_EQ(outcome._publication, publication);
      EXPECT_EQ(outcome._destinations.size(), prepared_only ? 0u : 1u);
      if (!prepared_only)
      {
        EXPECT_EQ(outcome._destinations.front()._queue, setup._queue);
        EXPECT_EQ(outcome._destinations.front()._status, DestinationStatus::Accepted);
      }
      const auto after = support::journal_snapshot(directory);
      EXPECT_EQ(after._revision, before._revision);
      EXPECT_EQ(after._tail == before._tail, prepared_only);

      // Repeated lookup is observational after recovery. No second acceptance
      // is synthesized, including when the producer died before seeing success.
      const auto settled = support::recovered_image(directory);
      const auto repeated = support::require_value(successor.publication_status(publication));
      EXPECT_EQ(repeated._status, outcome._status);
      EXPECT_EQ(repeated._topologyRevision, outcome._topologyRevision);
      EXPECT_EQ(support::recovered_image(directory), settled);
      runtime::RuntimeAccess access(support::open_runtime_state(directory));
      (void)runtime::runtime_topology(access);
      const auto references = access.record_references(9);
      ASSERT_EQ(references.size(), prepared_only ? 0u : 1u);
      if (!prepared_only)
      {
        const auto stored = runtime::runtime_publication_metadata(access, references.front());
        EXPECT_EQ(stored._producerIncarnation, publication._producer._incarnation);
        EXPECT_EQ(stored._publicationSequence, publication._sequence);
        ASSERT_EQ(stored._destinations.size(), 1u);
        EXPECT_EQ(stored._destinations.front()._queueSequence, 1u);
      }
    }
  }
} // namespace
