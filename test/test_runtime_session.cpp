#include "angaria/runtime.h"
#include "mutation_journal.h"
#include "native_atomic.h"
#include "session_metadata.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
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

#ifndef ANGARIA_NATIVE_SESSION_WORKER
#error ANGARIA_NATIVE_SESSION_WORKER must name the public Session qualification worker
#endif
#ifndef ANGARIA_TEST_VERSION
#error ANGARIA_TEST_VERSION must contain the configured framework version
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

  void put_word(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value)
  {
    for (std::size_t index = 0; index != 8; ++index)
    {
      bytes[offset + index] = static_cast<std::byte>((value >> (8 * index)) & 0xff);
    }
  }

  class CurrentDirectory
  {
  public:
    explicit CurrentDirectory(const fs::path& directory) : _previous(fs::current_path())
    {
      fs::current_path(directory);
    }

    ~CurrentDirectory()
    {
      std::error_code ignored;
      fs::current_path(_previous, ignored);
    }

  private:
    fs::path _previous;
  };

  class SessionWorker
  {
  public:
    SessionWorker(const fs::path& directory, const char* mode, unsigned task = 1)
    {
      std::array<int, 2> output{-1, -1};
      std::array<int, 2> input{-1, -1};
      if (::pipe(output.data()) == -1 || ::pipe(input.data()) == -1)
      {
        for (const auto fd : {output[0], output[1], input[0], input[1]})
        {
          if (fd != -1)
          {
            ::close(fd);
          }
        }
        throw std::runtime_error("session qualification pipe failed");
      }
      int error{};
      for (const auto fd : {output[0], output[1], input[0], input[1]})
      {
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
        {
          error = errno;
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
          const std::array<std::string, 4> values{
              ANGARIA_NATIVE_SESSION_WORKER, mode, directory.string(), std::to_string(task)};
          std::array<char*, 5> arguments{};
          for (std::size_t index = 0; index != values.size(); ++index)
          {
            arguments[index] = const_cast<char*>(values[index].c_str());
          }
          error = error == 0 ? ::posix_spawn(&_pid, values[0].c_str(), &actions, nullptr,
                                            arguments.data(), environ) : error;
          ::posix_spawn_file_actions_destroy(&actions);
        }
      }
      ::close(output[1]);
      ::close(input[0]);
      if (error != 0)
      {
        ::close(output[0]);
        ::close(input[1]);
        throw std::system_error(error, std::generic_category(), "start public Session worker");
      }
      _output = output[0];
      _input = input[1];
    }

    ~SessionWorker()
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

    SessionWorker(const SessionWorker&) = delete;
    SessionWorker& operator=(const SessionWorker&) = delete;

    char receive()
    {
      char value{};
      receive_exact(&value, 1);
      return value;
    }

    std::uint64_t receive_number()
    {
      std::uint64_t value{};
      receive_exact(&value, sizeof(value));
      return value;
    }

    int finish()
    {
      release();
      const auto status = wait_for_exit();
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }

    void terminate()
    {
      if (::kill(_pid, SIGKILL) == -1)
      {
        throw std::runtime_error("could not terminate Session worker");
      }
      const auto status = wait_for_exit();
      if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
      {
        throw std::runtime_error("Session worker did not terminate at the requested boundary");
      }
    }

  private:
    void release()
    {
      if (_input != -1)
      {
        ::close(std::exchange(_input, -1));
      }
    }

    void receive_exact(void* data, std::size_t length)
    {
      auto* bytes = static_cast<char*>(data);
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (length != 0)
      {
        if (std::chrono::steady_clock::now() >= deadline)
        {
          throw std::runtime_error("Session worker report deadline expired");
        }
        pollfd event{_output, POLLIN, 0};
        int ready;
        do
        {
          ready = ::poll(&event, 1, 3000);
        } while (ready == -1 && errno == EINTR);
        if (ready <= 0)
        {
          throw std::runtime_error("Session worker did not report");
        }
        const auto count = ::read(_output, bytes, length);
        if (count > 0)
        {
          bytes += count;
          length -= static_cast<std::size_t>(count);
        }
        else if (count != -1 || errno != EINTR)
        {
          throw std::runtime_error("Session worker exited without its report");
        }
      }
    }

    int wait_for_exit()
    {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      for (;;)
      {
        int status{};
        const auto result = ::waitpid(_pid, &status, WNOHANG);
        if (result == _pid)
        {
          _pid = -1;
          return status;
        }
        if (result == -1 && errno != EINTR)
        {
          throw std::runtime_error("Session worker wait failed");
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
          throw std::runtime_error("Session worker exit deadline expired");
        }
        std::this_thread::sleep_for(1ms);
      }
    }

    pid_t _pid{-1};
    int _output{-1};
    int _input{-1};
  };

  class RuntimeFile
  {
  public:
    explicit RuntimeFile(const fs::path& directory)
        : _controller(require_value(mapped::SessionController::open_existing(directory))),
          _guard(require_value(_controller->acquire_mutation())),
          _journal(require_value(mapped::MutationJournal::open(_controller, _guard))),
          _file(require_value(_controller->file(_guard)))
    {
      require_value(_journal.recover(_guard));
    }

    std::uint64_t word(std::uint64_t offset)
    {
      auto region = require_value(_file->map_read_write(offset, 8));
      return require_value(mapped::AtomicWord::bind(require_value(region.writable_bytes()), 0)).load_acquire();
    }

    void set_word(std::uint64_t offset, std::uint64_t value)
    {
      auto region = require_value(_file->map_read_write(offset, 8));
      require_value(mapped::AtomicWord::bind(require_value(region.writable_bytes()), 0)).store_release(value);
    }

    std::vector<std::byte> image()
    {
      const auto size = require_value(_file->size());
      auto region = require_value(_file->map_read_write(0, size));
      const auto bytes = require_value(region.writable_bytes());
      std::vector<std::byte> result(bytes.size());
      for (std::size_t offset = 0; offset != bytes.size(); offset += 8)
      {
        put_word(result, offset, require_value(mapped::AtomicWord::bind(bytes, offset)).load_acquire());
      }
      return result;
    }

    std::uint64_t section(mapped::SectionKind kind) const
    {
      const auto layout = mapped::make_fixed_layout(_controller->configuration()._limits);
      if (!layout)
      {
        throw std::runtime_error("runtime fixture has invalid section layout");
      }
      for (const auto& section : layout->_sections)
      {
        if (section._kind == kind)
        {
          return section._offset;
        }
      }
      throw std::runtime_error("runtime fixture section is absent");
    }

    std::shared_ptr<mapped::SessionController> _controller;
    mapped::MutationGuard _guard;
    mapped::MutationJournal _journal;
    mapped::NativeFile* _file;
  };

  class RuntimeSessionTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-runtime-XXXXXX").string();
      const auto directory = ::mkdtemp(pattern.data());
      ASSERT_NE(directory, nullptr);
      _root = directory;
      _directory = _root / "session";
    }

    void TearDown() override
    {
      std::error_code ignored;
      if (!_root.empty())
      {
        fs::remove_all(_root, ignored);
      }
    }

    SessionOptions options(const fs::path& directory = {}) const
    {
      SessionOptions result;
      result._name = "Runtime";
      result._applicationVersion = "App-2.5";
      result._directory = directory.empty() ? _directory : directory;
      result._maxLogBytes = 1048576;
      result._limits = CapacityLimits::empty().tasks(4);
      return result;
    }

    static TaskDescriptor creator()
    {
      TaskDescriptor result;
      result._name = "Creator";
      result._doc = "Creator process";
      return result;
    }

    CreatedSession create()
    {
      return require_value(Session::create(options(), creator()));
    }

    static void add_thread_tasks(const fs::path& directory, unsigned count = 1)
    {
      RuntimeFile access(directory);
      for (unsigned child = 0; child != count; ++child)
      {
        auto base = require_value(access._journal.snapshot(access._guard));
        mapped::TaskDefinition definition;
        definition._id = static_cast<TaskId>(child + 2);
        definition._descriptor._name = "Worker" + std::to_string(child);
        definition._descriptor._kind = TaskKind::Thread;
        definition._descriptor._process = TaskId{1};
        definition._descriptor._doc = "Thread fixture for the attachment contract";
        definition._registration._createdBy = TaskId{1};
        definition._registration._createdAt = UtcTimestamp{std::chrono::nanoseconds{123}};
        definition._registration._revision = base._revision + 1;
        mapped::JournalRecord record;
        record._kind = 2;
        record._immutable = require_value(mapped::encode_task_definition(definition, base._tail));
        mapped::TransactionPlan plan;
        plan._base = base;
        plan._kind = mapped::JournalKind::Append;
        plan._actor = TaskId{1};
        plan._actorIncarnation = access.word(access.section(mapped::SectionKind::TaskState));
        plan._newRevision = base._revision + 1;
        plan._records.push_back(std::move(record));
        mapped::RedoCommand index;
        index._target = access.section(mapped::SectionKind::TaskIndex) + (child + 1) * mapped::index_entry_bytes;
        index._payload.resize(32);
        put_word(index._payload, 0, child + 2);
        put_word(index._payload, 8, base._tail);
        put_word(index._payload, 16, plan._newRevision);
        plan._commands.push_back(std::move(index));
        require_value(access._journal.execute(access._guard, plan));
        require_value(access._controller->create_task_claim_file(access._guard, definition._id));
      }
    }

    fs::path _root;
    fs::path _directory;
  };

  TEST_F(RuntimeSessionTest, CreationRecordsCreatorEffectiveOptionsAndFrameworkMetadata)
  {
    auto created = create();
    const auto id = created._session.identity();
    EXPECT_NE(id, SessionId{});
    EXPECT_EQ(created._creator.claim(), (TaskClaim{id, TaskId{1}, 1}));
    const auto metadata = require_value(created._session.metadata());
    const auto recorded = require_value(created._session.options());
    EXPECT_EQ(metadata._session, id);
    EXPECT_EQ(metadata._createdBy, TaskId{1});
    EXPECT_EQ(metadata._angariaVersion, ANGARIA_TEST_VERSION);
    EXPECT_EQ(metadata._applicationVersion, "App-2.5");
    EXPECT_FALSE(metadata._creatorUser.empty());
    EXPECT_EQ(recorded._name, "Runtime");
    EXPECT_EQ(recorded._applicationVersion, "App-2.5");
    EXPECT_EQ(recorded._directory, fs::canonical(_directory));
    EXPECT_EQ(recorded._maxLogBytes, 1048576u);
    EXPECT_EQ(recorded._storage, StorageMapping::TwoPlane);
    EXPECT_FALSE(recorded._segmentBytes);
    EXPECT_EQ(recorded._limits._tasks, 4u);
    EXPECT_EQ(recorded._limits._queues, 0u);
    EXPECT_EQ(recorded._limits._stringPoolBytes, 0u);
    EXPECT_EQ(recorded._defaults._ringCapacity, 1024u);
    EXPECT_EQ(recorded._defaults._delivery, DeliveryPolicy::RecoverFromLog);
    EXPECT_EQ(recorded._defaults._selection, SelectionPolicy::Strict);
    EXPECT_EQ(recorded._defaults._wait, WaitPolicy::Spin);
    EXPECT_EQ(recorded._defaults._acceptance, AcceptancePolicy::AllDestinations);
    auto registry = created._session.registry();
    EXPECT_EQ(require_value(registry.find_task("Creator")), TaskId{1});
    const auto definition = require_value(registry.describe_task(TaskId{1}));
    EXPECT_EQ(definition._name, "Creator");
    EXPECT_EQ(definition._doc, "Creator process");
    EXPECT_EQ(definition._kind, TaskKind::Process);
    EXPECT_FALSE(definition._process);
    const auto registration = require_value(registry.metadata(TaskId{1}));
    EXPECT_EQ(registration._createdBy, TaskId{1});
    EXPECT_EQ(registration._createdAt, metadata._createdAt);
    EXPECT_EQ(registration._revision, 1u);
    const auto status = require_value(registry.task_status(TaskId{1}));
    EXPECT_TRUE(status._attached);
    EXPECT_EQ(status._processId, static_cast<std::uint64_t>(::getpid()));
    EXPECT_EQ(status._incarnation, 1u);
    EXPECT_EQ(status._restartCount, 0u);
    EXPECT_TRUE(status._lastAttachedAt);
    expect_error(registry.find_task("Unknown"), ErrorCode::NotFound);
    expect_error(created._session.attach_task(TaskId{2}), ErrorCode::NotFound);
    SessionWorker worker(_directory, "inspect");
    EXPECT_EQ(worker.receive(), 'A');
    EXPECT_EQ(worker.finish(), 0);
  }

  TEST_F(RuntimeSessionTest, InvalidCreationSettingsLeaveNoPublishedOrTemporaryDirectory)
  {
    struct InvalidCase
    {
      const char* _name;
      ErrorCode _code;
      std::function<void(SessionOptions&, TaskDescriptor&)> _change;
    };
    const std::vector<InvalidCase> cases{
        {"empty name", ErrorCode::InvalidConfiguration, [](auto& value, auto&)
        {
          value._name.clear();
        }},
        {"null name", ErrorCode::InvalidConfiguration, [](auto& value, auto&)
        {
          value._name = std::string{"bad\0name", 8};
        }},
        {"invalid UTF-8", ErrorCode::InvalidConfiguration, [](auto& value, auto&)
        {
          value._name = "\xc0\xaf";
        }},
        {"empty directory", ErrorCode::InvalidConfiguration, [](auto& value, auto&)
        {
          value._directory.clear();
        }},
        {"null directory", ErrorCode::InvalidConfiguration, [](auto& value, auto&)
        {
          value._directory = fs::path{value._directory.string() + std::string{"\0hidden", 7}};
        }},
        {"invalid directory UTF-8", ErrorCode::InvalidConfiguration, [](auto& value, auto&)
        {
          value._directory /= "\xc0\xaf";
        }},
        {"zero log limit", ErrorCode::InvalidConfiguration, [](auto& value, auto&)
        {
          value._maxLogBytes = 0;
        }},
        {"unrepresentable limit", ErrorCode::InvalidConfiguration, [](auto& value, auto&)
        {
          value._maxLogBytes = std::uint64_t{1} << 63;
        }},
        {"no creator capacity", ErrorCode::InvalidConfiguration, [](auto& value, auto&)
        {
          value._limits.tasks(0);
        }},
        {"empty creator", ErrorCode::InvalidConfiguration, [](auto&, auto& task)
        {
          task._name.clear();
        }},
        {"thread creator", ErrorCode::InvalidConfiguration, [](auto&, auto& task)
        {
          task._kind = TaskKind::Thread;
          task._process = TaskId{1};
        }},
        {"process with parent", ErrorCode::InvalidConfiguration, [](auto&, auto& task)
        {
          task._process = TaskId{1};
        }},
        {"segmented storage", ErrorCode::UnsupportedPolicy, [](auto& value, auto&)
        {
          value._storage = StorageMapping::Segmented;
          value._segmentBytes = 1048576;
        }},
        {"two-plane segment option", ErrorCode::InvalidConfiguration, [](auto& value, auto&)
        {
          value._segmentBytes = 0;
        }},
        {"zero ring", ErrorCode::InvalidConfiguration, [](auto& value, auto&)
        {
          value._defaults._ringCapacity = 0;
        }},
        {"unknown selection", ErrorCode::InvalidConfiguration, [](auto& value, auto&)
        {
          value._defaults._selection = static_cast<SelectionPolicy>(99);
        }},
        {"fair selection", ErrorCode::UnsupportedPolicy, [](auto& value, auto&)
        {
          value._defaults._selection = SelectionPolicy::Fair;
        }},
        {"eventfd", ErrorCode::UnsupportedPolicy, [](auto& value, auto&)
        {
          value._defaults._wait = WaitPolicy::EventFd;
        }},
    };
    for (const auto& example : cases)
    {
      SCOPED_TRACE(example._name);
      auto configured = options();
      auto task = creator();
      example._change(configured, task);
      expect_error(Session::create(configured, task), example._code);
      EXPECT_TRUE(fs::is_empty(_root));
    }
    auto too_small = options();
    too_small._maxLogBytes = mapped::make_fixed_layout(too_small._limits)->_arenaBegin;
    expect_error(Session::create(too_small, creator()), ErrorCode::LogFull);
    EXPECT_TRUE(fs::is_empty(_root));
  }

  TEST_F(RuntimeSessionTest, ExistingTargetsArePreservedAndConcurrentCreationHasOneWinner)
  {
    fs::create_directory(_directory);
    std::ofstream(_directory / "sentinel") << "preserve";
    expect_error(Session::create(options(), creator()), ErrorCode::Conflict);
    std::ifstream sentinel(_directory / "sentinel");
    std::string contents;
    sentinel >> contents;
    EXPECT_EQ(contents, "preserve");
    fs::remove_all(_directory);
    std::ofstream(_directory) << "ordinary file";
    expect_error(Session::create(options(), creator()), ErrorCode::Conflict);
    EXPECT_EQ(fs::file_size(_directory), 13u);
    fs::remove(_directory);
    fs::create_symlink(_root / "missing", _directory);
    expect_error(Session::create(options(), creator()), ErrorCode::Conflict);
    EXPECT_TRUE(fs::is_symlink(_directory));
    EXPECT_EQ(fs::read_symlink(_directory), _root / "missing");
    fs::remove(_directory);
    SessionWorker first(_directory, "create");
    SessionWorker second(_directory, "create");
    const auto a = first.receive();
    const auto b = second.receive();
    EXPECT_TRUE((a == 'C' && b == 'B') || (a == 'B' && b == 'C'));
    EXPECT_EQ(first.finish(), 0);
    EXPECT_EQ(second.finish(), 0);
    auto attached = require_value(Session::attach(_directory));
    EXPECT_EQ(require_value(attached.metadata())._createdBy, TaskId{1});
    EXPECT_EQ(std::distance(fs::directory_iterator(_root), fs::directory_iterator{}), 1);
  }

  TEST_F(RuntimeSessionTest, IndependentAttachmentAndRelocationUseTheActualDirectory)
  {
    SessionId identity;
    SessionMetadata metadata;
    {
      auto created = create();
      identity = created._session.identity();
      metadata = require_value(created._session.metadata());
      auto second = require_value(Session::attach(_directory / "."));
      EXPECT_EQ(second.identity(), identity);
      expect_error(second.attach_task(TaskId{1}), ErrorCode::OwnershipBusy);
      CurrentDirectory current(_root);
      auto relative = require_value(Session::attach("session"));
      EXPECT_EQ(relative.identity(), identity);
      EXPECT_EQ(require_value(relative.options())._directory, fs::canonical(_directory));
    }
    const auto moved = _root / "relocated";
    fs::rename(_directory, moved);
    auto session = require_value(Session::attach(moved));
    EXPECT_EQ(session.identity(), identity);
    EXPECT_EQ(require_value(session.metadata())._createdAt, metadata._createdAt);
    EXPECT_EQ(require_value(session.options())._directory, fs::canonical(moved));
    auto task = require_value(session.attach_task(TaskId{1}));
    EXPECT_EQ(task.claim()._incarnation, 2u);
    SessionWorker worker(moved, "inspect");
    EXPECT_EQ(worker.receive(), 'A');
    EXPECT_EQ(worker.finish(), 0);
  }

  TEST_F(RuntimeSessionTest, ActorRegistryRetainsItsTaskWhileReadOnlyViewsDoNot)
  {
    std::optional<Session> session;
    std::optional<Registry> actor;
    std::optional<Registry> reader;
    {
      auto created = create();
      session.emplace(std::move(created._session));
      actor.emplace(created._creator.registry());
      reader.emplace(session->registry());
    }
    expect_error(session->attach_task(TaskId{1}), ErrorCode::OwnershipBusy);
    SessionWorker busy(_directory, "try_claim");
    EXPECT_EQ(busy.receive(), 'B');
    EXPECT_EQ(busy.finish(), 0);
    actor.reset();
    EXPECT_FALSE(require_value(reader->task_status(TaskId{1}))._attached);
    auto replacement = require_value(session->attach_task(TaskId{1}));
    EXPECT_EQ(replacement.claim()._incarnation, 2u);
    EXPECT_EQ(require_value(reader->task_status(TaskId{1}))._restartCount, 1u);
  }

  TEST_F(RuntimeSessionTest, PublicationIdsPersistWithinAClaimAndResetOnlyWithANewIncarnation)
  {
    auto created = create();
    const auto first = require_value(created._creator.next_publication_id());
    const auto second = require_value(created._creator.next_publication_id());
    EXPECT_EQ(first._producer, created._creator.claim());
    EXPECT_EQ(first._sequence, 1u);
    EXPECT_EQ(second._sequence, 2u);
    auto session = std::move(created._session);
    {
      auto closing = std::move(created._creator);
    }
    auto replacement = require_value(session.attach_task(TaskId{1}));
    const auto next = require_value(replacement.next_publication_id());
    EXPECT_EQ(next._producer._incarnation, first._producer._incarnation + 1);
    EXPECT_EQ(next._sequence, 1u);
    EXPECT_NE(next, first);
    const auto status = require_value(session.registry().task_status(TaskId{1}));
    EXPECT_EQ(status._restartCount, 1u);
    EXPECT_TRUE(status._attached);
  }

  TEST_F(RuntimeSessionTest, CoordinatedProcessTaskIssuanceIsUniqueAcrossThreads)
  {
    auto created = create();
    std::mutex mutex;
    std::vector<std::uint64_t> sequences;
    std::vector<Error> errors;
    std::barrier start{5};
    std::array<std::thread, 4> threads;
    for (auto& thread : threads)
    {
      thread = std::thread([&]
      {
        start.arrive_and_wait();
        for (unsigned iteration = 0; iteration != 8; ++iteration)
        {
          const auto result = created._creator.next_publication_id();
          std::lock_guard lock(mutex);
          if (const auto* id = std::get_if<PublicationId>(&result))
          {
            sequences.push_back(id->_sequence);
          }
          else
          {
            errors.push_back(std::get<Error>(result));
          }
        }
      });
    }
    start.arrive_and_wait();
    for (auto& thread : threads)
    {
      thread.join();
    }
    ASSERT_TRUE(errors.empty());
    ASSERT_EQ(sequences.size(), 32u);
    std::sort(sequences.begin(), sequences.end());
    for (std::size_t index = 0; index != sequences.size(); ++index)
    {
      EXPECT_EQ(sequences[index], index + 1);
    }
  }

  TEST_F(RuntimeSessionTest, TerminatedOwnerIsReportedDetachedAndCanBeReplaced)
  {
    auto created = create();
    auto session = std::move(created._session);
    {
      auto closing = std::move(created._creator);
    }
    SessionWorker owner(_directory, "hold_issue");
    EXPECT_EQ(owner.receive(), 'A');
    const auto incarnation = owner.receive_number();
    EXPECT_EQ(owner.receive_number(), 1u);
    EXPECT_EQ(owner.receive_number(), 2u);
    EXPECT_TRUE(require_value(session.registry().task_status(TaskId{1}))._attached);
    expect_error(session.attach_task(TaskId{1}), ErrorCode::OwnershipBusy);
    owner.terminate();
    const auto dead = require_value(session.registry().task_status(TaskId{1}));
    EXPECT_FALSE(dead._attached);
    EXPECT_EQ(dead._incarnation, incarnation);
    auto replacement = require_value(session.attach_task(TaskId{1}));
    EXPECT_EQ(replacement.claim()._incarnation, incarnation + 1);
    EXPECT_EQ(require_value(replacement.next_publication_id())._sequence, 1u);
    EXPECT_EQ(require_value(session.registry().task_status(TaskId{1}))._restartCount, 2u);
  }

  TEST_F(RuntimeSessionTest, MovesAndForkRejectionPreserveNativeClaimOwnership)
  {
    auto created = create();
    auto session = std::move(created._session);
    auto task = std::move(created._creator);
    EXPECT_THROW(static_cast<void>(created._session.identity()), std::logic_error);
    EXPECT_THROW(static_cast<void>(created._creator.claim()), std::logic_error);
    expect_error(created._session.metadata(), ErrorCode::InvalidState);
    expect_error(created._creator.next_publication_id(), ErrorCode::InvalidState);
    expect_error(session.attach_task(TaskId{1}), ErrorCode::OwnershipBusy);
    {
      auto closing = std::move(task);
    }
    SessionWorker worker(_directory, "fork_reject");
    EXPECT_EQ(worker.receive(), 'A');
    expect_error(session.attach_task(TaskId{1}), ErrorCode::OwnershipBusy);
    EXPECT_EQ(worker.finish(), 0);
    require_value(session.attach_task(TaskId{1}));
  }

  TEST_F(RuntimeSessionTest, ThreadTasksRequireAndRetainTheirParentAndRejectOtherThreads)
  {
    auto created = create();
    add_thread_tasks(_directory);
    auto session = std::move(created._session);
    std::optional<Task> child;
    std::optional<Registry> retained_child;
    std::optional<Error> attachment_error;
    std::thread attaching([&]
    {
      auto result = session.attach_task(TaskId{2});
      if (const auto* error = std::get_if<Error>(&result))
      {
        attachment_error = *error;
        return;
      }
      child.emplace(std::get<Task>(std::move(result)));
      retained_child.emplace(child->registry());
      const auto publication = child->next_publication_id();
      if (const auto* error = std::get_if<Error>(&publication))
      {
        attachment_error = *error;
      }
    });
    attaching.join();
    ASSERT_FALSE(attachment_error);
    ASSERT_TRUE(child);
    EXPECT_THROW(static_cast<void>(child->claim()), std::logic_error);
    expect_error(child->next_publication_id(), ErrorCode::WrongOwner);
    {
      auto closing = std::move(created._creator);
    }
    expect_error(session.attach_task(TaskId{1}), ErrorCode::OwnershipBusy);
    child.reset();
    expect_error(session.attach_task(TaskId{1}), ErrorCode::OwnershipBusy);
    retained_child.reset();
    auto parent = require_value(session.attach_task(TaskId{1}));
    EXPECT_EQ(parent.claim()._incarnation, 2u);
    {
      auto closing = std::move(parent);
    }
    expect_error(session.attach_task(TaskId{2}), ErrorCode::WrongOwner);
  }

  TEST_F(RuntimeSessionTest, AttachAndIssuanceWorkAtTheExactRecordedLogLimit)
  {
    std::uint64_t required{};
    {
      auto created = create();
      required = fs::file_size(_directory / "session.ang");
    }
    fs::remove_all(_directory);
    auto configured = options();
    configured._maxLogBytes = required;
    auto created = require_value(Session::create(configured, creator()));
    EXPECT_EQ(fs::file_size(_directory / "session.ang"), required);
    const auto first = require_value(created._creator.next_publication_id());
    EXPECT_EQ(first._sequence, 1u);
    auto session = std::move(created._session);
    {
      auto closing = std::move(created._creator);
    }
    auto replacement = require_value(session.attach_task(TaskId{1}));
    EXPECT_EQ(replacement.claim()._incarnation, 2u);
    EXPECT_EQ(require_value(replacement.next_publication_id())._sequence, 1u);
    EXPECT_EQ(fs::file_size(_directory / "session.ang"), required);
    RuntimeFile access(_directory);
    const auto snapshot = require_value(access._journal.snapshot(access._guard));
    EXPECT_EQ(snapshot._tail, required);
    EXPECT_EQ(snapshot._accounted, required);
  }

  TEST_F(RuntimeSessionTest, ReplacingADeadParentClearsManyChildrenAtTheFullLogLimit)
  {
    constexpr unsigned child_count = 24;
    auto configured = options();
    configured._limits.tasks(child_count + 1);
    std::uint64_t required{};
    {
      auto created = require_value(Session::create(configured, creator()));
      add_thread_tasks(_directory, child_count);
      required = fs::file_size(_directory / "session.ang");
    }
    fs::remove_all(_directory);
    configured._maxLogBytes = required;
    auto created = require_value(Session::create(configured, creator()));
    add_thread_tasks(_directory, child_count);
    auto session = std::move(created._session);
    {
      auto closing = std::move(created._creator);
    }
    SessionWorker worker(_directory, "hold_children", child_count);
    EXPECT_EQ(worker.receive(), 'A');
    const auto parent_incarnation = worker.receive_number();
    EXPECT_EQ(worker.receive_number(), child_count);
    for (unsigned child = 0; child != child_count; ++child)
    {
      const auto status = require_value(session.registry().task_status(static_cast<TaskId>(child + 2)));
      EXPECT_TRUE(status._attached);
      EXPECT_EQ(status._incarnation, 1u);
    }
    worker.terminate();
    auto parent = require_value(session.attach_task(TaskId{1}));
    EXPECT_EQ(parent.claim()._incarnation, parent_incarnation + 1);
    EXPECT_EQ(fs::file_size(_directory / "session.ang"), required);
    RuntimeFile access(_directory);
    const auto state = access.section(mapped::SectionKind::TaskState);
    for (unsigned child = 0; child != child_count; ++child)
    {
      const auto row = state + (child + 1) * mapped::task_state_bytes;
      EXPECT_EQ(access.word(row + 48) & 1, 0u);
      EXPECT_EQ(access.word(row + 56), parent_incarnation);
    }
    const auto snapshot = require_value(access._journal.snapshot(access._guard));
    EXPECT_EQ(snapshot._tail, required);
    EXPECT_EQ(snapshot._accounted, required);
  }

  TEST_F(RuntimeSessionTest, ExhaustedIssuanceAndAttachmentCountersChangeNoStoredBytes)
  {
    auto created = create();
    auto session = std::move(created._session);
    std::uint64_t row{};
    {
      RuntimeFile access(_directory);
      row = access.section(mapped::SectionKind::TaskState);
      access.set_word(row + 8, std::numeric_limits<std::uint64_t>::max());
    }
    std::vector<std::byte> before;
    {
      RuntimeFile access(_directory);
      before = access.image();
    }
    expect_error(created._creator.next_publication_id(), ErrorCode::CapacityExceeded);
    {
      RuntimeFile access(_directory);
      EXPECT_EQ(access.image(), before);
      access.set_word(row + 8, 0);
    }
    {
      auto closing = std::move(created._creator);
    }
    for (const auto counter : {0u, 16u})
    {
      std::uint64_t previous{};
      {
        RuntimeFile access(_directory);
        previous = access.word(row + counter);
        access.set_word(row + counter, std::numeric_limits<std::uint64_t>::max());
        before = access.image();
      }
      expect_error(session.attach_task(TaskId{1}), ErrorCode::CapacityExceeded);
      {
        RuntimeFile access(_directory);
        EXPECT_EQ(access.image(), before);
        access.set_word(row + counter, previous);
      }
    }
  }

  TEST_F(RuntimeSessionTest, CorruptMetadataIndexesAndCompatibilityFailWithoutRepairWrites)
  {
    auto created = create();
    for (unsigned corruption = 0; corruption != 11; ++corruption)
    {
      SCOPED_TRACE(corruption);
      std::uint64_t target{};
      std::uint64_t invalid{};
      RuntimeFile access(_directory);
      switch (corruption)
      {
      case 0:
        target = 112; // Missing publication of a completed bootstrap.
        break;
      case 1:
        target = 72; // Missing Session definition.
        break;
      case 2:
        target = access.word(72) + 64 + 48; // Name slice outside its owning record.
        invalid = require_value(access._file->size()) + 64;
        break;
      case 3:
        target = access.section(mapped::SectionKind::TaskIndex); // Index/definition key conflict.
        invalid = 2;
        break;
      case 4:
        target = 48; // Native profile in the upper four bytes.
        invalid = (access.word(target) & 0xffffffff) | (std::uint64_t{0x7fffffff} << 32);
        break;
      case 5:
        target = 0; // Invalid format magic.
        break;
      case 6:
        target = 112; // Unknown bootstrap publication flag.
        invalid = 2;
        break;
      case 7:
        target = access.section(mapped::SectionKind::TaskState) + 48; // Unknown Task claim flag.
        invalid = access.word(target) | 8;
        break;
      case 8:
        target = access.section(mapped::SectionKind::TaskState) + 64; // Retirement exceeds issued sequence.
        invalid = 1;
        break;
      case 9:
        target = access.section(mapped::SectionKind::TaskState) + 80; // Reserved Task state word.
        invalid = 1;
        break;
      case 10:
        target = access.section(mapped::SectionKind::TaskState) + mapped::task_state_bytes;
        invalid = 1; // An empty Task index slot cannot contain claim state.
        break;
      }
      const auto old = access.word(target);
      access.set_word(target, invalid);
      const auto before = access.image();
      access._guard = mapped::MutationGuard{};
      expect_error(Session::attach(_directory), ErrorCode::IoError);
      // The retained controller is intentionally used to inspect and restore
      // corrupt bytes without re-opening through the rejected public factory.
      EXPECT_EQ(access.image(), before);
      access.set_word(target, old);
    }
  }
} // namespace
