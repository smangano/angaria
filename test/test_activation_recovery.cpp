// Public topology activation and updates across independent processes, followed by qualification of
// the private hooks used by that same implementation. Application code registers
// through a live Task Registry; raw snapshots and fault callbacks below are test
// infrastructure. SIGKILL covers a running local host, not power-loss durability.

#include "native_fixture.h"
#include "result_helpers.h"
#include "process_support.h"
#include "angaria/runtime.h"
#include "topology_mutations.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ANGARIA_NATIVE_ACTIVATION_WORKER
#error ANGARIA_NATIVE_ACTIVATION_WORKER must name the independent activation worker
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
  class ActivationWorker
  {
  public:
    ActivationWorker(const fs::path& directory, const std::string& mode, unsigned kind,
                 unsigned actor = 1, std::uint64_t raw = 0, std::uint32_t version = 1,
                 const std::string& name = "Registered", const std::string& doc = "durable metadata",
                 mapped::JournalCheckpoint point = mapped::JournalCheckpoint::Prepared,
                 std::uint64_t index = 0)
    {
      std::array<int, 2> output{-1, -1};
      std::array<int, 2> input{-1, -1};
      if (::pipe(output.data()) == -1 || ::pipe(input.data()) == -1)
      {
        angaria::test_support::close_pipe_fds(output, input);
        throw std::runtime_error("activation worker pipe failed");
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
          const std::array<std::string, 11> values{
              ANGARIA_NATIVE_ACTIVATION_WORKER, mode, directory.string(), std::to_string(actor),
              std::to_string(kind), std::to_string(raw),
              std::to_string(version), name, doc,
              std::to_string(static_cast<unsigned>(point)), std::to_string(index)};
          std::array<char*, 12> arguments{};
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
        throw std::system_error(error, std::generic_category(), "spawn activation worker");
      }
      ::close(output[1]);
      ::close(input[0]);
      _output = output[0];
      _input = input[1];
    }

    ~ActivationWorker()
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

    ActivationWorker(const ActivationWorker&) = delete;
    ActivationWorker& operator=(const ActivationWorker&) = delete;

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
      angaria::test_support::proceed_without_sigpipe(_input, "activation worker continuation failed",
          "block activation worker SIGPIPE", "consume activation worker SIGPIPE");
    }

    void terminate_at_checkpoint()
    {
      if (receive() != 'R')
      {
        throw std::runtime_error("activation worker reached an unexpected checkpoint");
      }
      terminate();
    }

    void terminate()
    {
      angaria::test_support::terminate(_pid,
          {"activation worker termination failed", "activation worker wait failed",
           "activation worker exit deadline expired", "activation worker did not stop at SIGKILL"});
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
          {"activation worker report deadline expired", "activation worker did not report",
           "activation worker exited without its report"});
    }

    int wait_for_exit()
    {
      const auto status = angaria::test_support::wait_for_exit(_pid,
          "activation worker wait failed", "activation worker exit deadline expired");
      _pid = -1;
      return status;
    }

    pid_t _pid{-1};
    int _output{-1};
    int _input{-1};
  };

  class ActivationRecoveryTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-activation-processes-XXXXXX").string();
      ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
      _root = pattern;
    }

    void TearDown() override
    {
      std::error_code ignored;
      fs::remove_all(_root, ignored);
    }

    static std::shared_ptr<runtime::SessionState> state(const fs::path& directory)
    {
      return angaria::test_support::open_runtime_state(directory);
    }

    static mapped::JournalSnapshot snapshot(const fs::path& directory)
    {
      return angaria::test_support::journal_snapshot(directory);
    }

    static std::vector<std::uint64_t> image(const fs::path& directory)
    {
      return angaria::test_support::recovered_image(directory);
    }

    static std::vector<std::string> claims(const fs::path& directory)
    {
      return angaria::test_support::claim_names(directory);
    }

    static std::size_t ring_count(const fs::path& directory)
    {
      runtime::RuntimeAccess access(state(directory));
      return access.record_references(8).size();
    }

    // Every scenario starts with two registered logical Tasks, Sources 1/2,
    // Queues 1/2 and Publishing set 3. Only the operation's prerequisites differ;
    // all baseline setup uses public registration and activation calls.
    Session create_detached(const fs::path& directory, unsigned operation)
    {
      SessionOptions options;
      options._name = "Activation processes";
      options._directory = directory;
      options._maxLogBytes = 2097152;
      options._limits = CapacityLimits::empty().tasks(4).queues(4).source_sets(4)
          .publishing_sets(4).memberships_per_set(4);
      TaskDescriptor creator;
      creator._name = "Creator";
      auto created = require_value(Session::create(options, creator));
      auto registry = created._creator.registry();
      TaskDescriptor other;
      other._name = "OtherActor";
      require_value(registry.register_task(other));
      SourceQueueSetDescriptor source;
      source._name = "Inputs";
      source._owner = operation == 1 ? TaskId::Invalid : TaskId{1};
      require_value(registry.register_source_set(source));
      source._name = "OtherInputs";
      source._owner = TaskId{1};
      require_value(registry.register_source_set(source));
      QueueDescriptor queue;
      queue._name = "First";
      queue._producer = operation == 0 ? TaskId::Invalid : TaskId{1};
      queue._sourceSet = SourceQueueSetId{1};
      queue._ringCapacity = 3;
      queue._doc = "odd ring";
      require_value(registry.register_queue(queue));
      queue._name = "Second";
      queue._producer = TaskId{1};
      queue._ringCapacity = 5;
      require_value(registry.register_queue(queue));
      PublishingQueueSetDescriptor publishing;
      publishing._name = "Updates";
      publishing._owner = operation == 2 ? TaskId::Invalid : TaskId{1};
      if (operation != 2)
      {
        publishing._destinations = {operation == 4 ? QueueId{1} : QueueId{2}};
      }
      require_value(registry.register_publishing_set(publishing));
      if (operation == 4 || operation == 5 || operation == 7)
      {
        require_value(registry.activate_source_set(SourceQueueSetId{1}));
      }
      if (operation == 4 || operation == 7)
      {
        require_value(registry.activate_queue(QueueId{1}));
        require_value(registry.activate_queue(QueueId{2}));
      }
      if (operation == 4)
      {
        require_value(registry.activate_publishing_set(PublishingQueueSetId{3}));
      }
      return std::move(created._session);
    }

    static OperationResult perform(Registry registry, unsigned operation)
    {
      switch (static_cast<runtime::TopologyMutation>(operation))
      {
      case runtime::TopologyMutation::AdoptQueue:
        return registry.adopt_queue(QueueId{1}, TaskId{1});
      case runtime::TopologyMutation::AdoptSource:
        return registry.adopt_source_set(SourceQueueSetId{1}, TaskId{1});
      case runtime::TopologyMutation::AdoptPublishing:
        return registry.adopt_publishing_set(PublishingQueueSetId{3}, TaskId{1});
      case runtime::TopologyMutation::AssignSource:
        return registry.assign_source(QueueId{1}, SourceQueueSetId{2}, 7);
      case runtime::TopologyMutation::SetDestinations:
      {
        const std::array destinations{QueueId{2}, QueueId{1}};
        return registry.set_destinations(PublishingQueueSetId{3}, destinations);
      }
      case runtime::TopologyMutation::ActivateQueue:
        return registry.activate_queue(QueueId{1});
      case runtime::TopologyMutation::ActivateSource:
        return registry.activate_source_set(SourceQueueSetId{1});
      case runtime::TopologyMutation::ActivatePublishing:
        return registry.activate_publishing_set(PublishingQueueSetId{3});
      }
      throw std::runtime_error("invalid activation operation");
    }

    static bool changed(Registry registry, unsigned operation)
    {
      switch (static_cast<runtime::TopologyMutation>(operation))
      {
      case runtime::TopologyMutation::AdoptQueue:
        return require_value(registry.describe_queue(QueueId{1}))._producer == TaskId{1};
      case runtime::TopologyMutation::AdoptSource:
        return require_value(registry.describe_source_set(SourceQueueSetId{1}))._owner == TaskId{1};
      case runtime::TopologyMutation::AdoptPublishing:
        return require_value(registry.describe_publishing_set(PublishingQueueSetId{3}))._owner == TaskId{1};
      case runtime::TopologyMutation::AssignSource:
      {
        const auto queue = require_value(registry.describe_queue(QueueId{1}));
        return queue._sourceSet == SourceQueueSetId{2} && queue._priority == 7;
      }
      case runtime::TopologyMutation::SetDestinations:
        return require_value(registry.describe_publishing_set(PublishingQueueSetId{3}))._destinations ==
            std::vector<QueueId>{QueueId{2}, QueueId{1}};
      case runtime::TopologyMutation::ActivateQueue:
        return require_value(registry.state(QueueId{1})) == EntityState::Active;
      case runtime::TopologyMutation::ActivateSource:
        return require_value(registry.state(SourceQueueSetId{1})) == EntityState::Active;
      case runtime::TopologyMutation::ActivatePublishing:
        return require_value(registry.state(PublishingQueueSetId{3})) == EntityState::Active;
      }
      throw std::runtime_error("invalid activation operation");
    }

    static void check_graph(Registry registry, unsigned operation, bool accepted)
    {
      EXPECT_EQ(changed(registry, operation), accepted);
      const auto members = require_value(registry.source_members(SourceQueueSetId{1}));
      EXPECT_EQ(members, operation == 3 && accepted ? std::vector<QueueId>{QueueId{2}} :
          (std::vector<QueueId>{QueueId{1}, QueueId{2}}));
      const auto other = require_value(registry.source_members(SourceQueueSetId{2}));
      EXPECT_EQ(other, operation == 3 && accepted ? std::vector<QueueId>{QueueId{1}} : std::vector<QueueId>{});
      for (const auto id : members)
      {
        EXPECT_EQ(require_value(registry.describe_queue(id))._sourceSet, SourceQueueSetId{1});
      }
      for (const auto id : other)
      {
        EXPECT_EQ(require_value(registry.describe_queue(id))._sourceSet, SourceQueueSetId{2});
      }
    }

    // A materialized Queue definition occupies the same bytes after its state
    // changes. Count its actual word checkpoints, then stop inside ring slots,
    // rather than accidentally stopping only inside the preceding definition.
    static std::uint64_t middle_ring_checkpoint(const fs::path& directory)
    {
      runtime::RuntimeAccess access(state(directory));
      const auto topology = runtime::runtime_topology(access);
      const auto found = std::find_if(topology._queues.begin(), topology._queues.end(), [](const auto& queue)
      {
        return queue._definition._id == QueueId{1};
      });
      if (found == topology._queues.end())
      {
        throw std::runtime_error("activation fixture has no Queue");
      }
      const auto definitionWords = access.record_envelope(found->_reference, 3)._totalBytes / 8 - 1;
      return definitionWords + 7 + *found->_definition._descriptor._ringCapacity * mapped::ring_slot_bytes / 16;
    }

    fs::path _root;
  };

  // Independent executables mutate then inspect through public APIs. Original
  // creation attribution survives adoption, assignment and activation; each
  // effective update creates exactly one new Registry revision.
  TEST_F(ActivationRecoveryTest, IndependentProcessesApplyAndDiscoverEveryMutation)
  {
    for (unsigned operation = 0; operation != 8; ++operation)
    {
      SCOPED_TRACE(operation);
      const auto directory = _root / std::to_string(operation);
      auto session = create_detached(directory, operation);
      const auto before = snapshot(directory);
      const auto initialRings = ring_count(directory);
      const auto files = claims(directory);
      ActivationWorker writer(directory, "register", operation);
      ASSERT_EQ(writer.receive(), 'R');
      writer.proceed();
      const auto result = writer.receive_outcome();
      EXPECT_EQ(result._marker, 'A');
      EXPECT_EQ(result._value, 0u);
      EXPECT_EQ(writer.finish(), 0);
      check_graph(session.registry(), operation, true);
      EXPECT_EQ(snapshot(directory)._revision, before._revision + 1);
      EXPECT_EQ(ring_count(directory), initialRings + (operation == 5 ? 1 : 0));
      EXPECT_EQ(claims(directory), files);
      ActivationWorker reader(directory, "inspect", operation);
      ASSERT_EQ(reader.receive(), 'I');
      EXPECT_EQ(reader.receive_number(), 1u);
      EXPECT_EQ(reader.receive_number(), 1u);
      EXPECT_EQ(reader.receive_number(), before._revision + 1);
      EXPECT_EQ(reader.receive_number(), operation == 3 ? 1u : 2u);
      EXPECT_EQ(reader.finish(), 0);
      if (operation == 3)
      {
        EXPECT_EQ(require_value(session.registry().metadata(SourceQueueSetId{1}))._revision, before._revision + 1);
        EXPECT_EQ(require_value(session.registry().metadata(SourceQueueSetId{2}))._revision, before._revision + 1);
      }
    }
  }

  // Both actors may activate topology owned by an offline logical participant.
  // Their overlapping calls must allocate one ring and one accepted revision.
  TEST_F(ActivationRecoveryTest, ConcurrentQueueActivationReservesOneRingAndRevision)
  {
    const auto directory = _root / "activation-race";
    auto session = create_detached(directory, 5);
    const auto before = snapshot(directory);
    const auto rings = ring_count(directory);
    ActivationWorker first(directory, "register", 5, 1);
    ActivationWorker second(directory, "register", 5, 2);
    ASSERT_EQ(first.receive(), 'R');
    ASSERT_EQ(second.receive(), 'R');
    first.proceed();
    second.proceed();
    EXPECT_EQ(first.receive_outcome()._marker, 'A');
    EXPECT_EQ(second.receive_outcome()._marker, 'A');
    EXPECT_EQ(snapshot(directory)._revision, before._revision + 1);
    EXPECT_EQ(ring_count(directory), rings + 1);
    check_graph(session.registry(), 5, true);
    EXPECT_EQ(first.finish(), 0);
    EXPECT_EQ(second.finish(), 0);
  }

  // Active destination replacement belongs to its producer Task. Another live
  // Task cannot win a race by changing that set's destinations, even though
  // setup/activation of offline ownership is allowed through a registrar.
  TEST_F(ActivationRecoveryTest, ConcurrentActivePublishingUpdateKeepsOwnerAuthority)
  {
    const auto directory = _root / "publisher-race";
    auto session = create_detached(directory, 4);
    const auto before = snapshot(directory);
    const auto rings = ring_count(directory);
    ActivationWorker owner(directory, "register", 4, 1);
    ActivationWorker outsider(directory, "register", 4, 2, 1);
    ASSERT_EQ(owner.receive(), 'R');
    ASSERT_EQ(outsider.receive(), 'R');
    owner.proceed();
    outsider.proceed();
    EXPECT_EQ(owner.receive_outcome()._marker, 'A');
    const auto rejected = outsider.receive_outcome();
    EXPECT_EQ(rejected._marker, 'E');
    EXPECT_EQ(rejected._value, static_cast<std::uint64_t>(ErrorCode::WrongOwner));
    check_graph(session.registry(), 4, true);
    EXPECT_EQ(snapshot(directory)._revision, before._revision + 1);
    EXPECT_EQ(ring_count(directory), rings);
    EXPECT_EQ(owner.finish(), 0);
    EXPECT_EQ(outsider.finish(), 0);
  }

  // Keep each operation's crash matrix independently attributable and bounded
  // by the normal test deadline; the complete suite still exercises all cuts.
  class ActivationCrashRecoveryTest : public ActivationRecoveryTest,
                                      public testing::WithParamInterface<runtime::TopologyMutation>
  {
  };

  // Stop actual store/I/O boundaries for every operation. Assignment has three
  // definitions and two membership groups; activation also owns ring bytes.
  // Recovery must expose the whole accepted change or the exact old topology.
  TEST_P(ActivationCrashRecoveryTest, CrashCutsPreserveWholeActivationAndTopologyChanges)
  {
    struct Cut
    {
      mapped::JournalCheckpoint _point;
      std::uint64_t _index;
      bool _accepted;
    };
    const std::array<Cut, 10> common{{
        {mapped::JournalCheckpoint::JournalBodyWord, 0, false},
        {mapped::JournalCheckpoint::Prepared, 0, false},
        {mapped::JournalCheckpoint::FileGrowth, 0, false},
        {mapped::JournalCheckpoint::RecordBodyWord, 0, false},
        {mapped::JournalCheckpoint::RecordReady, 0, false},
        {mapped::JournalCheckpoint::Committed, 0, true},
        {mapped::JournalCheckpoint::RedoWord, 0, true},
        {mapped::JournalCheckpoint::RedoWord, 3, true},
        {mapped::JournalCheckpoint::EpilogueWord, 2, true},
        {mapped::JournalCheckpoint::Idle, 0, true},
    }};
    const auto operation = static_cast<unsigned>(GetParam());
    const auto model = _root / (std::to_string(operation) + "-model");
    auto modelSession = create_detached(model, operation);
    std::vector<Cut> cuts(common.begin(), common.end());
    if (operation == 3)
    {
      cuts.push_back({mapped::JournalCheckpoint::RecordReady, 2, false});
      cuts.push_back({mapped::JournalCheckpoint::RedoWord, 5, true});
      cuts.push_back({mapped::JournalCheckpoint::RedoWord, 11, true});
      cuts.push_back({mapped::JournalCheckpoint::RedoWord, 16, true});
      cuts.push_back({mapped::JournalCheckpoint::RecordCommitted, 2, true});
    }
    if (operation == 5)
    {
      cuts.push_back({mapped::JournalCheckpoint::RecordReady, 1, false});
      cuts.push_back({mapped::JournalCheckpoint::RecordBodyWord, middle_ring_checkpoint(model), false});
      cuts.push_back({mapped::JournalCheckpoint::RedoWord, 4, true});
      cuts.push_back({mapped::JournalCheckpoint::RedoWord, 5, true});
      cuts.push_back({mapped::JournalCheckpoint::RecordCommitted, 1, true});
    }
    for (std::size_t cut = 0; cut < cuts.size(); ++cut)
    {
      SCOPED_TRACE(operation);
      SCOPED_TRACE(cut);
      const auto directory = _root / (std::to_string(operation) + "-crash-" + std::to_string(cut));
      auto session = create_detached(directory, operation);
      const auto before = snapshot(directory);
      const auto rings = ring_count(directory);
      const auto files = claims(directory);
      const auto& point = cuts[cut];
      ActivationWorker writer(directory, "crash", operation, 1, 0, 1, "", "", point._point, point._index);
      writer.terminate_at_checkpoint();
      check_graph(session.registry(), operation, point._accepted);
      const auto recovered = snapshot(directory);
      EXPECT_EQ(recovered._revision, before._revision + (point._accepted ? 1 : 0));
      EXPECT_EQ(recovered._tail == before._tail, !point._accepted);
      EXPECT_EQ(ring_count(directory), rings + (operation == 5 && point._accepted ? 1 : 0));
      EXPECT_EQ(claims(directory), files);
      auto actor = require_value(session.attach_task(TaskId{1}));
      const auto beforeRetry = snapshot(directory);
      const auto result = perform(actor.registry(), operation);
      EXPECT_TRUE(std::holds_alternative<std::monostate>(result));
      EXPECT_EQ(snapshot(directory)._revision, before._revision + 1);
      EXPECT_EQ(ring_count(directory), rings + (operation == 5 ? 1 : 0));
      check_graph(session.registry(), operation, true);
      if (point._accepted)
      {
        EXPECT_EQ(snapshot(directory), beforeRetry);
      }
    }
  }

  INSTANTIATE_TEST_SUITE_P(TopologyOperations, ActivationCrashRecoveryTest,
      testing::Values(runtime::TopologyMutation::AdoptQueue,
                      runtime::TopologyMutation::AdoptSource,
                      runtime::TopologyMutation::AdoptPublishing,
                      runtime::TopologyMutation::AssignSource,
                      runtime::TopologyMutation::SetDestinations,
                      runtime::TopologyMutation::ActivateQueue,
                      runtime::TopologyMutation::ActivateSource,
                      runtime::TopologyMutation::ActivatePublishing),
      [](const testing::TestParamInfo<runtime::TopologyMutation>& parameter)
      {
        switch (parameter.param)
        {
        case runtime::TopologyMutation::AdoptQueue:
          return std::string{"AdoptQueue"};
        case runtime::TopologyMutation::AdoptSource:
          return std::string{"AdoptSource"};
        case runtime::TopologyMutation::AdoptPublishing:
          return std::string{"AdoptPublishing"};
        case runtime::TopologyMutation::AssignSource:
          return std::string{"AssignSource"};
        case runtime::TopologyMutation::SetDestinations:
          return std::string{"SetDestinations"};
        case runtime::TopologyMutation::ActivateQueue:
          return std::string{"ActivateQueue"};
        case runtime::TopologyMutation::ActivateSource:
          return std::string{"ActivateSource"};
        case runtime::TopologyMutation::ActivatePublishing:
          return std::string{"ActivatePublishing"};
        }
        throw std::logic_error("invalid activation crash operation");
      });

  // Accepted repair can be interrupted repeatedly, including inside the second
  // Source index, destination-membership rows, or activation ring geometry.
  // A fresh process always finishes the same accepted revision and allocation.
  TEST_F(ActivationRecoveryTest, RepeatedAcceptedRecoveryFinishesOneRevision)
  {
    for (unsigned operation = 0; operation != 8; ++operation)
    {
      SCOPED_TRACE(operation);
      const auto directory = _root / (std::to_string(operation) + "-repair");
      auto session = create_detached(directory, operation);
      const auto before = snapshot(directory);
      const auto rings = ring_count(directory);
      ActivationWorker writer(directory, "crash", operation, 1, 0, 1, "", "", mapped::JournalCheckpoint::Committed);
      writer.terminate_at_checkpoint();
      ActivationWorker first(directory, "recover", operation, 1, 0, 1, "", "", mapped::JournalCheckpoint::RedoWord,
                             operation == 3 ? 5 : operation == 5 ? 4 : 1);
      first.terminate_at_checkpoint();
      ActivationWorker second(directory, "recover", operation, 1, 0, 1, "", "",
                              operation == 3 ? mapped::JournalCheckpoint::RedoWord : mapped::JournalCheckpoint::RecordCommitted,
                              operation == 3 ? 16 : operation == 5 ? 1 : 0);
      second.terminate_at_checkpoint();
      if (operation == 3)
      {
        ActivationWorker third(directory, "recover", operation, 1, 0, 1, "", "", mapped::JournalCheckpoint::RecordCommitted, 2);
        third.terminate_at_checkpoint();
      }
      check_graph(session.registry(), operation, true);
      EXPECT_EQ(snapshot(directory)._revision, before._revision + 1);
      EXPECT_EQ(ring_count(directory), rings + (operation == 5 ? 1 : 0));
      auto actor = require_value(session.attach_task(TaskId{1}));
      const auto beforeRetry = snapshot(directory);
      EXPECT_TRUE(std::holds_alternative<std::monostate>(perform(actor.registry(), operation)));
      EXPECT_EQ(snapshot(directory), beforeRetry);
    }
  }

  // Precommit failure proves rollback. Accepted updates can complete in-call or
  // report OutcomeUnknown until an independent lookup repairs them. Exact retry
  // must settle that same change, including its original ring allocation.
  TEST_F(ActivationRecoveryTest, IoOutcomesResolveThroughLookupAndExactRetry)
  {
    for (unsigned operation = 0; operation != 8; ++operation)
    {
      for (const auto& mode : {std::string{"io_before"}, std::string{"io_after"}, std::string{"io_unknown"}})
      {
        SCOPED_TRACE(operation);
        SCOPED_TRACE(mode);
        const auto directory = _root / (std::to_string(operation) + mode);
        auto session = create_detached(directory, operation);
        const auto before = snapshot(directory);
        const auto rings = ring_count(directory);
        ActivationWorker writer(directory, mode, operation);
        const auto result = writer.receive_outcome();
        const bool accepted = mode != "io_before";
        EXPECT_EQ(result._marker, mode == "io_after" ? 'A' : 'E');
        EXPECT_EQ(result._value, mode == "io_after" ? 0u : static_cast<std::uint64_t>(
            accepted ? ErrorCode::OutcomeUnknown : ErrorCode::IoError));
        check_graph(session.registry(), operation, accepted);
        EXPECT_EQ(snapshot(directory)._revision, before._revision + (accepted ? 1 : 0));
        EXPECT_EQ(ring_count(directory), rings + (operation == 5 && accepted ? 1 : 0));
        const auto beforeRetry = image(directory);
        writer.proceed();
        const auto retried = writer.receive_outcome();
        EXPECT_EQ(retried._marker, 'A');
        EXPECT_EQ(retried._value, 0u);
        check_graph(session.registry(), operation, true);
        EXPECT_EQ(snapshot(directory)._revision, before._revision + 1);
        EXPECT_EQ(ring_count(directory), rings + (operation == 5 ? 1 : 0));
        if (accepted)
        {
          EXPECT_EQ(image(directory), beforeRetry);
        }
        EXPECT_EQ(writer.finish(), 0);
      }
    }
  }

  // Actual endpoint ownership and SIGKILL/replacement are qualified by
  // PublicRuntimeAdmission using real messages, schemas and progress files.
  // This suite retains the administrative activation/recovery boundaries.

} // namespace
