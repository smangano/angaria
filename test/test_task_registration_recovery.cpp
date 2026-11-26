// Public registration across independent processes, followed by qualification of
// the private hooks used by that same implementation. Application code registers
// through a live Task Registry; raw snapshots and fault callbacks below are test
// infrastructure. SIGKILL covers a running local host, not power-loss durability.

#include "native_fixture.h"
#include "result_helpers.h"
#include "process_support.h"
#include "angaria/runtime.h"
#include "runtime_state.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ANGARIA_NATIVE_REGISTRATION_WORKER
#error ANGARIA_NATIVE_REGISTRATION_WORKER must name the independent registration worker
#endif

extern char** environ;

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = angaria::detail::mapped;
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;

  using angaria::test_support::require_value;

  using angaria::test_support::has_error;

  struct Outcome
  {
    char _marker{};
    std::uint64_t _value{};
  };

  // Pipe reports establish exact process boundaries; no sleep decides when a
  // registration has committed. Every assertion path closes/reaps the worker,
  // and a deadline makes a missing checkpoint fail rather than hang the suite.
  class RegistrationWorker
  {
  public:
    RegistrationWorker(const fs::path& directory, const std::string& mode, unsigned actor = 1,
                       const std::string& name = "Registered", const std::string& doc = "durable metadata",
                       mapped::JournalCheckpoint point = mapped::JournalCheckpoint::Prepared,
                       std::uint64_t index = 0)
    {
      std::array<int, 2> output{-1, -1};
      std::array<int, 2> input{-1, -1};
      if (::pipe(output.data()) == -1 || ::pipe(input.data()) == -1)
      {
        angaria::test_support::close_pipe_fds(output, input);
        throw std::runtime_error("registration worker pipe failed");
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
          const std::array<std::string, 8> values{
              ANGARIA_NATIVE_REGISTRATION_WORKER, mode, directory.string(), std::to_string(actor), name, doc,
              std::to_string(static_cast<unsigned>(point)), std::to_string(index)};
          std::array<char*, 9> arguments{};
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
        throw std::system_error(error, std::generic_category(), "spawn registration worker");
      }
      ::close(output[1]);
      ::close(input[0]);
      _output = output[0];
      _input = input[1];
    }

    ~RegistrationWorker()
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

    RegistrationWorker(const RegistrationWorker&) = delete;
    RegistrationWorker& operator=(const RegistrationWorker&) = delete;

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
      angaria::test_support::proceed(_input, "registration worker continuation failed");
    }

    void terminate_at_checkpoint()
    {
      if (receive() != 'R')
      {
        throw std::runtime_error("registration worker reached an unexpected checkpoint");
      }
      terminate();
    }

    void terminate()
    {
      angaria::test_support::terminate(_pid,
          {"registration worker termination failed", "registration worker wait failed",
           "registration worker exit deadline expired", "registration worker did not stop at SIGKILL"});
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
          {"registration worker report deadline expired", "registration worker did not report",
           "registration worker exited without its report"});
    }

    int wait_for_exit()
    {
      const auto status = angaria::test_support::wait_for_exit(_pid,
          "registration worker wait failed", "registration worker exit deadline expired");
      _pid = -1;
      return status;
    }

    pid_t _pid{-1};
    int _output{-1};
    int _input{-1};
  };

  class TaskRegistrationRecoveryTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-register-XXXXXX").string();
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
      options._name = "Registration processes";
      options._directory = directory;
      options._maxLogBytes = 1048576;
      options._limits = CapacityLimits::empty().tasks(8);
      TaskDescriptor creator;
      creator._name = "Creator";
      return require_value(Session::create(options, creator));
    }

    Session create_detached(const fs::path& directory)
    {
      auto created = create(directory);
      auto session = std::move(created._session);
      {
        auto closing = std::move(created._creator);
      }
      return session;
    }

    static TaskDescriptor descriptor(const std::string& name = "Registered",
                                     const std::string& doc = "durable metadata")
    {
      TaskDescriptor result;
      result._name = name;
      result._doc = doc;
      return result;
    }

    static std::shared_ptr<runtime::SessionState> state(const fs::path& directory)
    {
      auto value = std::make_shared<runtime::SessionState>();
      value->_directory = fs::canonical(directory);
      value->_controller = require_value(mapped::SessionController::open_existing(directory));
      return value;
    }

    static mapped::JournalSnapshot snapshot(const fs::path& directory)
    {
      return angaria::test_support::journal_snapshot(directory);
    }

    static std::vector<std::uint64_t> image(const fs::path& directory)
    {
      return angaria::test_support::recovered_image(directory);
    }

    static mapped::FileIdentity claim_identity(const fs::path& directory, TaskId task)
    {
      const auto path = directory / "claims" /
          ("task-" + std::to_string(static_cast<unsigned>(task)) + ".lock");
      auto file = require_value(mapped::NativeFile::open_existing(path, mapped::FileAccess::ReadOnly));
      return require_value(file.identity());
    }

    static void inspect_report(RegistrationWorker& worker, TaskId id, TaskId creator,
                               std::uint64_t revision, TaskKind kind, TaskId parent,
                               std::uint64_t incarnation)
    {
      ASSERT_EQ(worker.receive(), 'I');
      EXPECT_EQ(worker.receive_number(), static_cast<std::uint64_t>(id));
      EXPECT_EQ(worker.receive_number(), static_cast<std::uint64_t>(creator));
      EXPECT_EQ(worker.receive_number(), revision);
      EXPECT_EQ(worker.receive_number(), static_cast<std::uint64_t>(kind));
      EXPECT_EQ(worker.receive_number(), static_cast<std::uint64_t>(parent));
      EXPECT_EQ(worker.receive_number(), incarnation);
      EXPECT_EQ(worker.finish(), 0);
    }

    fs::path _root;
  };

  // Register logical participants once through the creator's public Registry.
  // Fresh executables discover the same descriptors/attribution, and a Process
  // Task that was only registered can attach and issue its first ID independently.
  TEST_F(TaskRegistrationRecoveryTest, IndependentProcessesDiscoverAndAttachRegisteredTasks)
  {
    const auto directory = _root / "discovery";
    auto created = create(directory);
    auto registry = created._creator.registry();
    const auto process = require_value(registry.register_task(descriptor("Gateway", "risk gateway")));
    auto thread_definition = descriptor("RiskLoop", "risk thread");
    thread_definition._kind = TaskKind::Thread;
    thread_definition._process = process;
    const auto thread = require_value(registry.register_task(thread_definition));
    EXPECT_FALSE(require_value(registry.task_status(process))._attached);
    RegistrationWorker process_reader(directory, "inspect_attach", 1, "Gateway", "risk gateway");
    inspect_report(process_reader, process, TaskId{1}, 2, TaskKind::Process, TaskId::Invalid, 1);
    RegistrationWorker thread_reader(directory, "inspect", 1, "RiskLoop", "risk thread");
    inspect_report(thread_reader, thread, TaskId{1}, 3, TaskKind::Thread, process, 0);
    EXPECT_FALSE(require_value(registry.task_status(process))._attached);
    EXPECT_EQ(require_value(registry.metadata(process))._revision, 2u);
    EXPECT_EQ(require_value(registry.metadata(thread))._revision, 3u);
  }

  // Retain two different live actor claims and release their requests against
  // the same closed gate. Equal registrations converge, conflicting descriptors
  // choose one winner, and distinct names each consume one identity/revision.
  TEST_F(TaskRegistrationRecoveryTest, ConcurrentActorsSerializeIdentityAllocationAndIdempotentRegistration)
  {
    for (unsigned scenario = 0; scenario != 3; ++scenario)
    {
      SCOPED_TRACE(scenario);
      const auto directory = _root / ("concurrent-" + std::to_string(scenario));
      auto created = create(directory);
      EXPECT_EQ(require_value(created._creator.registry().register_task(descriptor("OtherActor"))), TaskId{2});
      auto session = std::move(created._session);
      {
        auto closing = std::move(created._creator);
      }
      const auto first_name = scenario == 2 ? "First" : "Shared";
      const auto second_name = scenario == 2 ? "Second" : "Shared";
      const auto second_doc = scenario == 1 ? "different" : "same";
      RegistrationWorker first(directory, "register", 1, first_name, "same");
      RegistrationWorker second(directory, "register", 2, second_name, second_doc);
      ASSERT_EQ(first.receive(), 'R');
      ASSERT_EQ(second.receive(), 'R');
      {
        runtime::RuntimeAccess closed_gate(state(directory));
        EXPECT_EQ(closed_gate._snapshot._revision, 2u);
        first.proceed();
        second.proceed();
        EXPECT_EQ(first.receive(), 'B');
        EXPECT_EQ(second.receive(), 'B');
      }
      const auto first_result = first.receive_outcome();
      const auto second_result = second.receive_outcome();
      if (scenario == 0)
      {
        EXPECT_EQ(first_result._marker, 'A');
        EXPECT_EQ(second_result._marker, 'A');
        EXPECT_EQ(first_result._value, 3u);
        EXPECT_EQ(second_result._value, 3u);
        const auto creator = require_value(session.registry().metadata(TaskId{3}))._createdBy;
        EXPECT_TRUE(creator == TaskId{1} || creator == TaskId{2});
      }
      else if (scenario == 1)
      {
        const auto first_won = first_result._marker == 'A';
        const auto& accepted = first_won ? first_result : second_result;
        const auto& rejected = first_won ? second_result : first_result;
        EXPECT_EQ(accepted._marker, 'A');
        EXPECT_EQ(accepted._value, 3u);
        EXPECT_EQ(rejected._marker, 'E');
        EXPECT_EQ(rejected._value, static_cast<std::uint64_t>(ErrorCode::Conflict));
        EXPECT_EQ(require_value(session.registry().describe_task(TaskId{3}))._doc, first_won ? "same" : "different");
        EXPECT_EQ(require_value(session.registry().metadata(TaskId{3}))._createdBy, first_won ? TaskId{1} : TaskId{2});
      }
      else
      {
        EXPECT_EQ(first_result._marker, 'A');
        EXPECT_EQ(second_result._marker, 'A');
        EXPECT_NE(first_result._value, second_result._value);
        EXPECT_EQ(first_result._value + second_result._value, 7u);
        const auto first_id = require_value(session.registry().find_task("First"));
        const auto second_id = require_value(session.registry().find_task("Second"));
        EXPECT_EQ(require_value(session.registry().metadata(first_id))._createdBy, TaskId{1});
        EXPECT_EQ(require_value(session.registry().metadata(second_id))._createdBy, TaskId{2});
      }
      EXPECT_EQ(snapshot(directory)._revision, scenario == 2 ? 4u : 3u);
      EXPECT_EQ(first.finish(), 0);
      EXPECT_EQ(second.finish(), 0);
      EXPECT_EQ(snapshot(directory)._revision, scenario == 2 ? 4u : 3u);
    }
  }

  // Claim files precede canonical registration so a committed Task is always
  // attachable. A killed pre-intent creator can leave one unused file; retry may
  // use that unpublished ID for another name while retaining the exact inode.
  TEST_F(TaskRegistrationRecoveryTest, PreIntentClaimFileSurvivesCrashAndIsReusedWithoutUnlinking)
  {
    const auto directory = _root / "claim-file";
    auto session = create_detached(directory);
    const auto base = snapshot(directory);
    RegistrationWorker writer(directory, "claim", 1, "Proposed");
    ASSERT_EQ(writer.receive(), 'R');
    const auto identity = claim_identity(directory, TaskId{2});
    writer.terminate();
    EXPECT_TRUE(has_error(session.registry().find_task("Proposed"), ErrorCode::NotFound));
    EXPECT_EQ(snapshot(directory)._revision, base._revision);
    EXPECT_EQ(snapshot(directory)._tail, base._tail);
    auto actor = require_value(session.attach_task(TaskId{1}));
    const auto id = require_value(actor.registry().register_task(descriptor("Replacement")));
    EXPECT_EQ(id, TaskId{2});
    EXPECT_EQ(claim_identity(directory, id), identity);
    EXPECT_TRUE(has_error(session.registry().find_task("Proposed"), ErrorCode::NotFound));
    EXPECT_EQ(require_value(session.registry().find_task("Replacement")), id);
    auto task = require_value(session.attach_task(id));
    EXPECT_EQ(task.claim()._incarnation, 1u);
  }

  // Stop real registration at resource/materialization/acceptance/replay cuts.
  // Recovery exposes no new Task before Committed, or exactly one fully indexed
  // Task after it; retry by the same descriptor cannot duplicate that identity.
  TEST_F(TaskRegistrationRecoveryTest, CrashCutsExposeOnlyTheOldOrCommittedRegistration)
  {
    struct Cut
    {
      mapped::JournalCheckpoint _point;
      std::uint64_t _index;
      bool _accepted;
    };
    const std::array<Cut, 11> cuts{{
        {mapped::JournalCheckpoint::JournalBodyWord, 0, false},
        {mapped::JournalCheckpoint::Prepared, 0, false},
        {mapped::JournalCheckpoint::FileGrowth, 0, false},
        {mapped::JournalCheckpoint::RecordBodyWord, 0, false},
        {mapped::JournalCheckpoint::RecordReady, 0, false},
        {mapped::JournalCheckpoint::Committed, 0, true},
        {mapped::JournalCheckpoint::RedoWord, 0, true},
        {mapped::JournalCheckpoint::RedoWord, 1, true},
        {mapped::JournalCheckpoint::EpilogueWord, 2, true},
        {mapped::JournalCheckpoint::Applied, 0, true},
        {mapped::JournalCheckpoint::Idle, 0, true},
    }};
    for (std::size_t cut = 0; cut != cuts.size(); ++cut)
    {
      SCOPED_TRACE(cut);
      const auto directory = _root / ("crash-" + std::to_string(cut));
      auto session = create_detached(directory);
      const auto base = snapshot(directory);
      const auto creator = require_value(session.registry().metadata(TaskId{1}));
      const auto& point = cuts[cut];
      RegistrationWorker writer(directory, "crash", 1, "Registered", "durable metadata", point._point, point._index);
      writer.terminate_at_checkpoint();
      const auto found = session.registry().find_task("Registered");
      if (point._accepted)
      {
        EXPECT_EQ(require_value(found), TaskId{2});
      }
      else
      {
        EXPECT_TRUE(has_error(found, ErrorCode::NotFound));
      }
      const auto recovered = snapshot(directory);
      EXPECT_EQ(recovered._revision, base._revision + (point._accepted ? 1 : 0));
      EXPECT_EQ(recovered._tail == base._tail, !point._accepted);
      const auto before_retry = claim_identity(directory, TaskId{2});
      auto actor = require_value(session.attach_task(TaskId{1}));
      const auto before_registration = snapshot(directory);
      EXPECT_EQ(require_value(actor.registry().register_task(descriptor())), TaskId{2});
      const auto after = snapshot(directory);
      EXPECT_EQ(after._revision, 2u);
      if (point._accepted)
      {
        EXPECT_EQ(after, before_registration);
      }
      EXPECT_EQ(claim_identity(directory, TaskId{2}), before_retry);
      const auto metadata = require_value(session.registry().metadata(TaskId{1}));
      EXPECT_EQ(metadata._createdBy, creator._createdBy);
      EXPECT_EQ(metadata._createdAt, creator._createdAt);
      EXPECT_EQ(metadata._revision, creator._revision);
      auto registered = require_value(session.attach_task(TaskId{2}));
      EXPECT_EQ(registered.claim()._incarnation, 1u);
    }
  }

  // Recovery can die after partial index redo or final-root installation too.
  // The next recovery must finish the same accepted definition/manifest rather
  // than allocating another Task, losing its claim file, or advancing revision twice.
  TEST_F(TaskRegistrationRecoveryTest, RepeatedInterruptedRecoveryPreservesTheAcceptedTaskIdentity)
  {
    const std::array<std::pair<mapped::JournalCheckpoint, std::uint64_t>, 4> cuts{{
        {mapped::JournalCheckpoint::RedoWord, 0},
        {mapped::JournalCheckpoint::RecordCommitted, 0},
        {mapped::JournalCheckpoint::EpilogueWord, 2},
        {mapped::JournalCheckpoint::Applied, 0},
    }};
    for (std::size_t cut = 0; cut != cuts.size(); ++cut)
    {
      SCOPED_TRACE(cut);
      const auto directory = _root / ("recover-" + std::to_string(cut));
      auto session = create_detached(directory);
      RegistrationWorker writer(directory, "crash", 1, "Registered", "durable metadata",
                                  mapped::JournalCheckpoint::Committed);
      writer.terminate_at_checkpoint();
      const auto identity = claim_identity(directory, TaskId{2});
      RegistrationWorker first_repair(directory, "recover", 1, "Registered", "durable metadata",
                                        cuts[cut].first, cuts[cut].second);
      first_repair.terminate_at_checkpoint();
      RegistrationWorker second_repair(directory, "recover", 1, "Registered", "durable metadata",
                                         mapped::JournalCheckpoint::EpilogueWord, 0);
      second_repair.terminate_at_checkpoint();
      EXPECT_EQ(require_value(session.registry().find_task("Registered")), TaskId{2});
      EXPECT_EQ(snapshot(directory)._revision, 2u);
      const auto metadata = require_value(session.registry().metadata(TaskId{2}));
      auto actor = require_value(session.attach_task(TaskId{1}));
      const auto before_retry = image(directory);
      EXPECT_EQ(require_value(actor.registry().register_task(descriptor())), TaskId{2});
      EXPECT_EQ(image(directory), before_retry);
      EXPECT_EQ(require_value(session.registry().metadata(TaskId{2}))._createdAt, metadata._createdAt);
      EXPECT_EQ(claim_identity(directory, TaskId{2}), identity);
      auto registered = require_value(session.attach_task(TaskId{2}));
      EXPECT_EQ(registered.claim()._incarnation, 1u);
    }
  }

  // Failure before commit is a known I/O rejection; repair after commit returns
  // acceptance, and blocked accepted repair returns OutcomeUnknown. Recovery plus
  // lookup/exact re-registration resolves all three without duplicate identities.
  TEST_F(TaskRegistrationRecoveryTest, IoOutcomesResolveThroughLookupAndExactRegistrationRetry)
  {
    const std::array<std::string, 3> modes{"io_before", "io_after", "io_unknown"};
    for (const auto& mode : modes)
    {
      SCOPED_TRACE(mode);
      const auto directory = _root / mode;
      auto session = create_detached(directory);
      RegistrationWorker writer(directory, mode);
      const auto first = writer.receive_outcome();
      const bool accepted = mode != "io_before";
      if (mode == "io_after")
      {
        EXPECT_EQ(first._marker, 'A');
        EXPECT_EQ(first._value, 2u);
      }
      else
      {
        EXPECT_EQ(first._marker, 'E');
        EXPECT_EQ(first._value, static_cast<std::uint64_t>(
            accepted ? ErrorCode::OutcomeUnknown : ErrorCode::IoError));
      }
      const auto found = session.registry().find_task("Registered");
      if (accepted)
      {
        EXPECT_EQ(require_value(found), TaskId{2});
      }
      else
      {
        EXPECT_TRUE(has_error(found, ErrorCode::NotFound));
      }
      EXPECT_EQ(snapshot(directory)._revision, accepted ? 2u : 1u);
      const auto identity = claim_identity(directory, TaskId{2});
      const auto before_retry = image(directory);
      writer.proceed();
      const auto retry = writer.receive_outcome();
      EXPECT_EQ(retry._marker, 'A');
      EXPECT_EQ(retry._value, 2u);
      EXPECT_EQ(snapshot(directory)._revision, 2u);
      if (accepted)
      {
        EXPECT_EQ(image(directory), before_retry);
      }
      EXPECT_EQ(claim_identity(directory, TaskId{2}), identity);
      EXPECT_EQ(writer.finish(), 0);
      auto registered = require_value(session.attach_task(TaskId{2}));
      EXPECT_EQ(registered.claim()._incarnation, 1u);
    }
  }
} // namespace
