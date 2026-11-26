// Complete Registry qualification uses independent exec clients. The domain
// recovery suites already enumerate individual write cuts; these integration
// cases check that mixed definitions and historical snapshots compose correctly.
#include "result_helpers.h"
#include "process_support.h"
#include "angaria/runtime.h"
#include "runtime_state.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ANGARIA_NATIVE_REGISTRY_WORKER
#error ANGARIA_NATIVE_REGISTRY_WORKER must name the independent Registry worker
#endif

extern char** environ;

namespace
{
  using namespace angaria;
  namespace fs = std::filesystem;
  namespace mapped = angaria::detail::mapped;
  using namespace std::chrono_literals;

  using angaria::test_support::require_value;

  struct Outcome
  {
    char _marker{};
    std::uint64_t _value{};
  };

  std::string read_bytes(const fs::path& path)
  {
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
      throw std::runtime_error("Cannot read Registry qualification fixture");
    }
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
  }

  // Pipe reports establish exact process boundaries; no sleep decides when a
  // registration has committed. Every assertion path closes/reaps the worker,
  // and a deadline makes a missing checkpoint fail rather than hang the suite.
  class RegistryWorker
  {
  public:
    RegistryWorker(const fs::path& path, const std::string& mode,
                   const std::string& actor = "RegistrarA", const std::string& prefix = "A",
                   mapped::JournalCheckpoint point = mapped::JournalCheckpoint::Prepared,
                   std::uint64_t index = 0)
    {
      std::array<int, 2> output{-1, -1};
      std::array<int, 2> input{-1, -1};
      if (::pipe(output.data()) == -1 || ::pipe(input.data()) == -1)
      {
        angaria::test_support::close_pipe_fds(output, input);
        throw std::runtime_error("Registry worker pipe failed");
      }
      int error{};
      for (auto* pair : {&output, &input})
      {
        for (auto& fd : *pair)
        {
          // Keep spawn's sources distinct from stdin/stdout even when the
          // invoking test environment left a standard descriptor closed.
          if (fd <= STDERR_FILENO)
          {
            const auto replacement = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            if (replacement == -1)
            {
              error = errno;
              break;
            }
            ::close(fd);
            fd = replacement;
          }
          else if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
          {
            error = errno;
            break;
          }
        }
        if (error != 0)
        {
          break;
        }
      }
      posix_spawn_file_actions_t actions;
      if (error == 0)
      {
        error = ::posix_spawn_file_actions_init(&actions);
        if (error == 0)
        {
          error = ::posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
          error = error == 0 ? ::posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO) : error;
          for (const auto fd : {output[0], output[1], input[0], input[1]})
          {
            error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, fd) : error;
          }
          const std::array<std::string, 7> values{
              ANGARIA_NATIVE_REGISTRY_WORKER, mode, path.string(), actor, prefix,
              std::to_string(static_cast<unsigned>(point)), std::to_string(index)};
          std::array<char*, 8> arguments{};
          for (std::size_t position = 0; position != values.size(); ++position)
          {
            arguments[position] = const_cast<char*>(values[position].c_str());
          }
          error = error == 0 ? ::posix_spawn(&_pid, values[0].c_str(), &actions, nullptr,
                                            arguments.data(), environ) : error;
          ::posix_spawn_file_actions_destroy(&actions);
        }
      }
      if (error != 0)
      {
        angaria::test_support::close_pipe_fds(output, input);
        throw std::system_error(error, std::generic_category(), "spawn Registry worker");
      }
      ::close(output[1]);
      ::close(input[0]);
      _output = output[0];
      _input = input[1];
    }

    ~RegistryWorker()
    {
      release();
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

    RegistryWorker(const RegistryWorker&) = delete;
    RegistryWorker& operator=(const RegistryWorker&) = delete;

    char receive()
    {
      char value{};
      receive_exact(&value, sizeof(value));
      return value;
    }

    std::uint64_t receive_number()
    {
      std::uint64_t value{};
      receive_exact(&value, sizeof(value));
      return value;
    }

    Outcome receive_outcome()
    {
      return Outcome{receive(), receive_number()};
    }

    void proceed()
    {
      angaria::test_support::proceed_without_sigpipe(_input, "Registry worker continuation failed",
          "block Registry worker SIGPIPE", "consume Registry worker SIGPIPE");
    }

    void terminate_at_checkpoint()
    {
      if (receive() != 'R')
      {
        throw std::runtime_error("Registry worker reached an unexpected checkpoint");
      }
      terminate();
    }

    void terminate()
    {
      angaria::test_support::terminate(_pid,
          {"Registry worker termination failed", "Registry worker wait failed",
           "Registry worker exit deadline expired", "Registry worker did not stop at SIGKILL"});
    }

    int finish()
    {
      release();
      return angaria::test_support::exit_code(wait_for_exit());
    }

  private:
    void release()
    {
      angaria::test_support::close_descriptor(_input);
    }

    void receive_exact(void* data, std::size_t length)
    {
      angaria::test_support::receive_exact(_output, data, length,
          {"Registry worker report deadline expired", "Registry worker did not report",
           "Registry worker exited without its report"});
    }

    int wait_for_exit()
    {
      const auto status = angaria::test_support::wait_for_exit(_pid,
          "Registry worker wait failed", "Registry worker exit deadline expired");
      _pid = -1;
      return status;
    }

    pid_t _pid{-1};
    int _output{-1};
    int _input{-1};
  };

  class RegistryQualificationTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-registry-processes-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    CreatedSession create(const fs::path& directory)
    {
      SessionOptions options;
      options._name = "Mixed Registry clients";
      options._directory = directory;
      options._maxLogBytes = 4194304;
      options._limits = CapacityLimits::empty().tasks(16).queues(8).source_sets(8)
          .publishing_sets(8).memberships_per_set(4).fields(16).message_schemas(16);
      options._defaults._ringCapacity = 3;
      TaskDescriptor setup;
      setup._name = "Setup";
      auto created = require_value(Session::create(options, setup));
      auto actor = created._creator.registry();
      for (const auto* name : {"RegistrarA", "RegistrarB"})
      {
        TaskDescriptor role;
        role._name = name;
        require_value(actor.register_task(role));
      }
      return created;
    }

    static SourceQueueSetId recovery_graph(Registry actor)
    {
      const auto registrar = require_value(actor.find_task("RegistrarA"));
      FieldDescriptor field;
      field._id = static_cast<FieldId>(0xee000001u);
      field._name = "Price";
      field._base = EFieldBase::BASE_INT64;
      field._size = 8;
      const auto price = require_value(actor.register_field(field));
      MessageDescriptor message;
      message._name = "Quote";
      message._schema = {static_cast<MessageType>(7), 1};
      message._fields = {price};
      require_value(actor.register_message(message));
      SourceQueueSetDescriptor source;
      source._name = "Inputs";
      source._owner = registrar;
      const auto inputs = require_value(actor.register_source_set(source));
      PublishingQueueSetDescriptor publisher;
      publisher._name = "EmptyUpdates";
      publisher._owner = registrar;
      require_value(actor.register_publishing_set(publisher));
      return inputs;
    }

    static std::array<std::uint64_t, 9> inspect(const fs::path& directory)
    {
      RegistryWorker worker(directory, "inspect");
      if (worker.receive() != 'I')
      {
        throw std::runtime_error("Inspector did not report a consistent Registry snapshot");
      }
      std::array<std::uint64_t, 9> summary{};
      for (auto& number : summary)
      {
        number = worker.receive_number();
      }
      if (worker.finish() != 0)
      {
        throw std::runtime_error("Registry inspector failed after its report");
      }
      return summary;
    }

    fs::path _root;
  };

  // File-based bootstrap exits completely. A different executable then reads
  // every entity kind and the bootstrap revision without attaching any Task.
  TEST_F(RegistryQualificationTest, TomlBootstrapAndOfflineInspectionWorkAcrossIndependentExecutables)
  {
    auto text = read_bytes(ANGARIA_CONFIGURATION_EXAMPLE);
    const std::string original = "directory = \"../build/trading-session\"";
    const auto position = text.find(original);
    ASSERT_NE(position, std::string::npos);
    text.replace(position, original.size(), "directory = \"session\"");
    const auto file = _root / "trading.toml";
    {
      std::ofstream output(file);
      output << text;
      ASSERT_TRUE(output);
    }
    RegistryWorker creator(file, "bootstrap", "MarketData");
    ASSERT_EQ(creator.receive(), 'D');
    const auto revision = creator.receive_number();
    ASSERT_EQ(creator.finish(), 0);
    const auto before = read_bytes(_root / "session" / "session.ang");
    const auto summary = inspect(_root / "session");
    EXPECT_EQ(summary[0], revision);
    EXPECT_EQ(summary[1], 4u);
    EXPECT_EQ(summary[2], 2u);
    EXPECT_EQ(summary[3], 1u);
    EXPECT_EQ(summary[4], 3u);
    EXPECT_EQ(summary[5], 2u);
    EXPECT_EQ(summary[6], 1u);
    EXPECT_EQ(summary[7], 0u); // Inspection claimed no application role.
    EXPECT_EQ(summary[8], 1u); // The creator was the only bootstrap Task.
    EXPECT_EQ(read_bytes(_root / "session" / "session.ang"), before);
  }

  // Two actors register all entity kinds while a third process repeatedly
  // checks whole snapshots. A snapshot must never mix revisions of inverse
  // relationships, even though setup itself spans several valid revisions.
  TEST_F(RegistryQualificationTest, ConcurrentMixedRegistrarsAndObserverComposeWithRestartAndRetainedSchemas)
  {
    const auto directory = _root / "session";
    auto created = create(directory);
    RegistryWorker first(directory, "register", "RegistrarA", "A");
    RegistryWorker second(directory, "register", "RegistrarB", "B");
    RegistryWorker observer(directory, "observe");
    ASSERT_EQ(first.receive(), 'R');
    ASSERT_EQ(second.receive(), 'R');
    ASSERT_EQ(observer.receive(), 'R');
    observer.proceed();
    // Capture the first observation before releasing either registrar. The
    // final observation below waits for both commits, making progress explicit
    // even when scheduling lets the middle samples race through quickly.
    ASSERT_EQ(observer.receive(), 'S');
    const auto initial_revision = observer.receive_number();
    first.proceed();
    second.proceed();
    observer.proceed();
    ASSERT_EQ(first.receive(), 'D');
    first.receive_number();
    ASSERT_EQ(second.receive(), 'D');
    second.receive_number();
    ASSERT_EQ(observer.receive(), 'M');
    observer.proceed();
    ASSERT_EQ(observer.receive(), 'O');
    EXPECT_EQ(observer.receive_number(), 16u);
    const auto final_revision = observer.receive_number();
    EXPECT_GT(final_revision, initial_revision);
    EXPECT_EQ(observer.finish(), 0);
    const auto live = inspect(directory);
    EXPECT_EQ(final_revision, live[0]);
    EXPECT_EQ(live[1], 5u);
    EXPECT_EQ(live[2], 2u);
    EXPECT_EQ(live[3], 4u);
    EXPECT_EQ(live[4], 2u);
    EXPECT_EQ(live[5], 2u);
    EXPECT_EQ(live[6], 2u);
    EXPECT_EQ(live[7], 3u); // Setup and the two independent registrars.
    {
      RegistryWorker competitor(directory, "claim", "RegistrarA");
      ASSERT_EQ(competitor.receive(), 'E');
      EXPECT_EQ(competitor.receive_number(), static_cast<std::uint64_t>(ErrorCode::OwnershipBusy));
      EXPECT_EQ(competitor.finish(), 0);
    }
    first.terminate();
    RegistryWorker replacement(directory, "claim", "RegistrarA");
    ASSERT_EQ(replacement.receive(), 'A');
    EXPECT_EQ(replacement.receive_number(), 2u);
    EXPECT_EQ(replacement.finish(), 0);
    EXPECT_EQ(second.finish(), 0);
    {
      auto closing = std::move(created._creator);
    }
    const auto offline = inspect(directory);
    EXPECT_EQ(offline[0], live[0]);
    EXPECT_EQ(offline[7], 0u);
    auto reader = created._session.registry();
    const auto version_one = require_value(reader.find_message("AQuote", 1));
    const auto version_two = require_value(reader.find_message("AQuote", 2));
    const auto first_revision = require_value(reader.metadata(version_one))._revision;
    const auto retained = require_value(reader.snapshot(first_revision));
    const auto old = std::find_if(retained._messages.begin(), retained._messages.end(), [&](const auto& entry)
    {
      return entry._descriptor._schema == version_one;
    });
    ASSERT_NE(old, retained._messages.end());
    EXPECT_EQ(old->_descriptor._fields.size(), 1u);
    EXPECT_EQ(require_value(reader.describe_message(version_two))._fields.size(), 2u);
  }

  // Representative integration cuts complement the exhaustive per-domain
  // matrices: Fields/schema history and the other Queue Set kind already exist
  // while a Queue/inverse membership or a new exact schema version is stopped.
  TEST_F(RegistryQualificationTest, InterruptedMixedUpdatesRecoverWholeSnapshotsAndExactRetries)
  {
    const std::array cuts{mapped::JournalCheckpoint::Prepared, mapped::JournalCheckpoint::Committed,
                          mapped::JournalCheckpoint::RedoWord};
    unsigned ordinal{};
    for (const auto* mode : {"crash_queue", "crash_schema"})
    {
      for (const auto cut : cuts)
      {
        SCOPED_TRACE(mode);
        SCOPED_TRACE(static_cast<unsigned>(cut));
        const auto directory = _root / ("cut-" + std::to_string(ordinal++));
        auto created = create(directory);
        const auto inputs = recovery_graph(created._creator.registry());
        const auto before = require_value(created._session.registry().snapshot());
        RegistryWorker writer(directory, mode, "RegistrarA", "", cut,
                              cut == mapped::JournalCheckpoint::RedoWord ?
                              (std::string{mode} == "crash_queue" ? 4 : 1) : 0);
        writer.terminate_at_checkpoint();
        const bool accepted = cut != mapped::JournalCheckpoint::Prepared;
        const auto summary = inspect(directory); // A new process performs recovery.
        EXPECT_EQ(summary[0], before._revision + (accepted ? 1 : 0));
        auto reader = created._session.registry();
        const auto recovered = require_value(reader.snapshot());
        const auto retained = require_value(reader.snapshot(before._revision));
        EXPECT_EQ(retained._fields.size(), 1u);
        EXPECT_EQ(retained._messages.size(), 1u);
        EXPECT_TRUE(retained._queues.empty());
        EXPECT_TRUE(retained._sources[0]._members.empty());
        auto actor = require_value(created._session.attach_task(require_value(reader.find_task("RegistrarA"))));
        const auto revision_before_retry = recovered._revision;
        if (std::string{mode} == "crash_queue")
        {
          EXPECT_EQ(recovered._queues.size(), accepted ? 1u : 0u);
          EXPECT_EQ(recovered._sources[0]._members.size(), accepted ? 1u : 0u);
          QueueDescriptor queue;
          queue._name = "RecoveredQueue";
          queue._producer = actor.claim()._task;
          queue._sourceSet = inputs;
          require_value(actor.registry().register_queue(queue));
        }
        else
        {
          EXPECT_EQ(recovered._messages.size(), accepted ? 2u : 1u);
          MessageDescriptor message;
          message._name = "Quote";
          message._schema = {static_cast<MessageType>(7), 2};
          const auto price = require_value(reader.find_field("Price"));
          message._fields = {price, price};
          require_value(actor.registry().register_message(message));
        }
        EXPECT_EQ(require_value(reader.snapshot())._revision, revision_before_retry + (accepted ? 0 : 1));
      }
    }
  }

  TEST_F(RegistryQualificationTest, InterruptedRecoveryCanRepeatBeforeAnOfflineSnapshot)
  {
    const auto directory = _root / "session";
    auto created = create(directory);
    recovery_graph(created._creator.registry());
    const auto before = require_value(created._session.registry().snapshot());
    RegistryWorker writer(directory, "crash_queue", "RegistrarA", "", mapped::JournalCheckpoint::Committed);
    writer.terminate_at_checkpoint();
    RegistryWorker recovery(directory, "recover", "RegistrarA", "", mapped::JournalCheckpoint::RedoWord, 4);
    recovery.terminate_at_checkpoint();
    const auto summary = inspect(directory);
    EXPECT_EQ(summary[0], before._revision + 1);
    EXPECT_EQ(summary[4], 1u);
    auto reader = created._session.registry();
    const auto recovered = require_value(reader.snapshot());
    ASSERT_EQ(recovered._sources.size(), 1u);
    ASSERT_EQ(recovered._queues.size(), 1u);
    EXPECT_EQ(recovered._sources[0]._members, (std::vector<QueueId>{recovered._queues[0]._id}));
    EXPECT_TRUE(require_value(reader.snapshot(before._revision))._sources[0]._members.empty());
    const auto bytes = read_bytes(directory / "session.ang");
    EXPECT_EQ(inspect(directory)[0], summary[0]);
    EXPECT_EQ(read_bytes(directory / "session.ang"), bytes);
  }
} // namespace
