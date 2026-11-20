#include "mapped_superblock.h"
#include "native_atomic.h"
#include "native_file.h"
#include "session_controller.h"

#include <gtest/gtest.h>

#include <array>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;

  static_assert(!std::is_copy_constructible_v<mapped::MutationGuard>);
  static_assert(std::is_nothrow_move_constructible_v<mapped::MutationGuard>);
  static_assert(std::is_nothrow_move_assignable_v<mapped::MutationGuard>);

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

  struct Pipe
  {
    Pipe()
    {
      if (::pipe(_fds.data()) == -1)
      {
        throw std::runtime_error("test pipe creation failed");
      }
      for (const auto fd : _fds)
      {
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
        {
          ::close(_fds[0]);
          ::close(_fds[1]);
          throw std::runtime_error("test pipe close-on-exec setup failed");
        }
      }
    }

    ~Pipe()
    {
      for (const auto fd : _fds)
      {
        if (fd != -1)
        {
          ::close(fd);
        }
      }
    }

    int release(std::size_t index)
    {
      return std::exchange(_fds[index], -1);
    }

    std::array<int, 2> _fds{-1, -1};
  };

  class Worker
  {
  public:
    Worker(const fs::path& directory, const std::string& mode,
           std::string argument = "1", std::string extra = "0")
        : _hasForkChild(mode == "fork_holder")
    {
      Pipe output;
      Pipe input;
      posix_spawn_file_actions_t actions;
      posix_spawnattr_t attributes;
      if (::posix_spawn_file_actions_init(&actions) != 0)
      {
        throw std::runtime_error("spawn actions initialization failed");
      }
      if (::posix_spawnattr_init(&attributes) != 0)
      {
        ::posix_spawn_file_actions_destroy(&actions);
        throw std::runtime_error("spawn attributes initialization failed");
      }
      const std::array<std::string, 7> values{
          ANGARIA_NATIVE_CONTROLLER_WORKER, mode, directory.string(),
          std::to_string(STDOUT_FILENO), std::to_string(STDIN_FILENO),
          std::move(argument), std::move(extra)};
      std::array<char*, 8> arguments{};
      for (std::size_t index = 0; index != values.size(); ++index)
      {
        arguments[index] = const_cast<char*>(values[index].c_str());
      }
      // Use distinct destinations: a self-duplication can leave CLOEXEC set on
      // the inherited descriptor on supported macOS versions.
      int error = ::posix_spawn_file_actions_adddup2(&actions, output._fds[1], STDOUT_FILENO);
      error = error == 0 ? ::posix_spawn_file_actions_adddup2(&actions, input._fds[0], STDIN_FILENO) : error;
      error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, output._fds[0]) : error;
      error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, input._fds[1]) : error;
      error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, output._fds[1]) : error;
      error = error == 0 ? ::posix_spawn_file_actions_addclose(&actions, input._fds[0]) : error;
      error = error == 0 ? ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) : error;
      error = error == 0 ? ::posix_spawnattr_setpgroup(&attributes, 0) : error;
      error = error == 0 ? ::posix_spawn(&_pid, values[0].c_str(), &actions, &attributes,
                                        arguments.data(), environ) : error;
      ::posix_spawn_file_actions_destroy(&actions);
      ::posix_spawnattr_destroy(&attributes);
      if (error != 0)
      {
        throw std::system_error(error, std::generic_category(), "spawn controller worker");
      }
      _output = output.release(0);
      _input = input.release(1);
    }

    ~Worker()
    {
      release();
      if (_pid != -1 && (!_reaped || _hasForkChild))
      {
        ::kill(-_pid, SIGKILL);
      }
      if (_pid != -1 && !_reaped)
      {
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

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    bool has_output(int timeout = 0) const
    {
      pollfd event{_output, POLLIN, 0};
      int result;
      do
      {
        result = ::poll(&event, 1, timeout);
      } while (result == -1 && errno == EINTR);
      if (result == -1)
      {
        throw std::runtime_error("worker output poll failed");
      }
      return result != 0;
    }

    char receive() const
    {
      if (!has_output(3000))
      {
        throw std::runtime_error("worker response deadline expired");
      }
      char value{};
      ssize_t count;
      do
      {
        count = ::read(_output, &value, 1);
      } while (count == -1 && errno == EINTR);
      if (count != 1)
      {
        throw std::runtime_error("worker exited without its expected response");
      }
      return value;
    }

    std::uint64_t receive_pid() const
    {
      std::uint64_t value{};
      auto* bytes = reinterpret_cast<char*>(&value);
      for (std::size_t index = 0; index != sizeof(value); ++index)
      {
        bytes[index] = receive();
      }
      return value;
    }

    void release()
    {
      if (_input != -1)
      {
        ::close(std::exchange(_input, -1));
      }
    }

    int finish()
    {
      release();
      const auto status = wait_for_exit();
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }

    void kill_owner()
    {
      if (::kill(_pid, SIGKILL) == -1)
      {
        throw std::runtime_error("could not terminate worker owner");
      }
      const auto status = wait_for_exit();
      if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
      {
        throw std::runtime_error("worker was not terminated at the requested boundary");
      }
    }

  private:
    int wait_for_exit()
    {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      for (;;)
      {
        int status{};
        const auto result = ::waitpid(_pid, &status, WNOHANG);
        if (result == _pid)
        {
          _reaped = true;
          return status;
        }
        if (result == -1 && errno != EINTR)
        {
          throw std::runtime_error("worker wait failed");
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
          throw std::runtime_error("worker exit deadline expired");
        }
        std::this_thread::sleep_for(1ms);
      }
    }

    pid_t _pid{-1};
    int _output{-1};
    int _input{-1};
    bool _reaped{};
    bool _hasForkChild{};
  };

  class SessionControllerTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-controller-XXXXXX").string();
      const auto directory = ::mkdtemp(pattern.data());
      ASSERT_NE(directory, nullptr);
      _root = directory;
      _directory = _root / "session";
      _configuration._session = {0x123456789abcdef0, 0xfedcba9876543210};
      _configuration._liveAbiProfile = require_value(mapped::qualify_native_profile())._liveAbiProfile;
      _configuration._maxLogBytes = 1048576;
      _configuration._limits = CapacityLimits::empty().tasks(4);
      make_session(_directory, _configuration);
    }

    void TearDown() override
    {
      std::error_code ignored;
      if (!_root.empty())
      {
        fs::remove_all(_root, ignored);
      }
    }

    static void make_session(const fs::path& directory, const mapped::SuperblockConfiguration& configuration)
    {
      fs::create_directories(directory / "claims");
      const auto layout = mapped::make_fixed_layout(configuration._limits);
      if (!layout)
      {
        throw std::runtime_error("invalid test layout");
      }
      auto file = require_value(mapped::NativeFile::create_exclusive(directory / "session.ang",
                                                                    configuration._maxLogBytes));
      require_value(file.resize(layout->_arenaBegin));
      require_value(file.write_exact(0, require_value(mapped::encode_initial_superblock(configuration))));
      require_value(mapped::NativeFile::create_exclusive(directory / "mutation.lock"));
    }

    std::shared_ptr<mapped::SessionController> open_controller() const
    {
      return require_value(mapped::SessionController::open_existing(_directory));
    }

    static void create_claims(const std::shared_ptr<mapped::SessionController>& controller,
                              std::initializer_list<unsigned> tasks = {1, 2})
    {
      auto guard = require_value(controller->acquire_mutation());
      for (const auto task : tasks)
      {
        require_value(controller->create_task_claim_file(guard, static_cast<TaskId>(task)));
      }
    }

    static int descriptor_for(const fs::path& path)
    {
      struct stat expected{};
      if (::stat(path.c_str(), &expected) == -1)
      {
        throw std::runtime_error("could not stat descriptor target");
      }
      for (const auto& entry : fs::directory_iterator("/dev/fd"))
      {
        const int fd = std::stoi(entry.path().filename().string());
        struct stat actual{};
        if (::fstat(fd, &actual) == 0 && actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino)
        {
          return fd;
        }
      }
      throw std::runtime_error("owned descriptor was not retained");
    }

    fs::path _root;
    fs::path _directory;
    mapped::SuperblockConfiguration _configuration;
  };

  TEST_F(SessionControllerTest, AliasesShareOneControllerAndLiveSessionCopiesAreRejected)
  {
    auto controller = open_controller();
    const auto alias = _root / "alias";
    fs::create_directory_symlink(_directory, alias);
    auto aliased = require_value(mapped::SessionController::open_existing(alias / "."));
    EXPECT_EQ(controller.get(), aliased.get());
    EXPECT_EQ(controller->session_id(), _configuration._session);
    EXPECT_EQ(controller->file_identity(), aliased->file_identity());
    EXPECT_EQ(controller->configuration()._limits._tasks, 4u);

    const auto copy = _root / "copy";
    make_session(copy, _configuration);
    expect_error(mapped::SessionController::open_existing(copy), ErrorCode::Conflict);
    fs::remove(copy / "session.ang");
    fs::create_hard_link(_directory / "session.ang", copy / "session.ang");
    expect_error(mapped::SessionController::open_existing(copy), ErrorCode::Conflict);
  }

  TEST_F(SessionControllerTest, LocalGateExcludesAnotherThreadOnTheSharedDescriptor)
  {
    auto controller = open_controller();
    auto guard = require_value(controller->acquire_mutation());
    std::promise<OperationResult> attempted;
    auto result = attempted.get_future();
    std::promise<OperationResult> acquired;
    auto completion = acquired.get_future();
    std::thread contender([&]
    {
      {
        const auto first = controller->try_acquire_mutation();
        if (const auto* error = std::get_if<Error>(&first))
        {
          attempted.set_value(*error);
        }
        else
        {
          attempted.set_value(std::monostate{});
        }
      }
      auto next = controller->acquire_mutation();
      acquired.set_value(std::holds_alternative<mapped::MutationGuard>(next) ?
                             OperationResult{std::monostate{}} : OperationResult{std::get<Error>(next)});
    });
    const auto first = result.get();
    EXPECT_EQ(completion.wait_for(0ms), std::future_status::timeout);
    guard = mapped::MutationGuard{};
    contender.join();
    expect_error(first, ErrorCode::OwnershipBusy);
    require_value(completion.get());
  }

  TEST_F(SessionControllerTest, NestedGateEntryIsRejectedWithoutReleasingTheOuterGuard)
  {
    auto controller = open_controller();
    auto guard = require_value(controller->acquire_mutation());
    expect_error(controller->try_acquire_mutation(), ErrorCode::OwnershipBusy);
    expect_error(controller->acquire_mutation(), ErrorCode::OwnershipBusy);
    require_value(guard.check_owner());
    Worker outsider(_directory, "try_gate");
    EXPECT_EQ(outsider.receive(), 'B');
    EXPECT_EQ(outsider.finish(), 0);
    guard = mapped::MutationGuard{};
    require_value(controller->try_acquire_mutation());
  }

  TEST_F(SessionControllerTest, GateMovesRetainOwnershipAndValidateTheirController)
  {
    auto controller = open_controller();
    mapped::MutationGuard empty;
    expect_error(empty.check_owner(), ErrorCode::InvalidState);
    expect_error(controller->file(empty), ErrorCode::InvalidState);
    auto guard = require_value(controller->acquire_mutation());
    mapped::MutationGuard moved{std::move(guard)};
    expect_error(guard.check_owner(), ErrorCode::InvalidState);
    require_value(moved.check_owner());

    auto other_configuration = _configuration;
    ++other_configuration._session._low;
    const auto other_directory = _root / "other";
    make_session(other_directory, other_configuration);
    auto other = require_value(mapped::SessionController::open_existing(other_directory));
    auto destination = require_value(other->acquire_mutation());
    expect_error(other->file(moved), ErrorCode::WrongOwner);
    destination = std::move(moved);
    expect_error(moved.check_owner(), ErrorCode::InvalidState);
    Worker released(other_directory, "try_gate");
    EXPECT_EQ(released.receive(), 'A');
    EXPECT_EQ(released.finish(), 0);
    Worker held(_directory, "try_gate");
    EXPECT_EQ(held.receive(), 'B');
    EXPECT_EQ(held.finish(), 0);
    destination = mapped::MutationGuard{};
    auto reopened = require_value(controller->try_acquire_mutation());
    require_value(reopened.check_owner());
  }

  TEST_F(SessionControllerTest, IndependentProcessGateAttemptsAndBlockingAcquisitionRespectOwnership)
  {
    auto controller = open_controller();
    auto guard = require_value(controller->acquire_mutation());
    Worker immediate(_directory, "try_gate");
    EXPECT_EQ(immediate.receive(), 'B');
    EXPECT_EQ(immediate.finish(), 0);
    Worker blocking(_directory, "block_gate");
    EXPECT_EQ(blocking.receive(), 'S');
    EXPECT_FALSE(blocking.has_output(25));
    guard = mapped::MutationGuard{};
    EXPECT_EQ(blocking.receive(), 'A');
    EXPECT_EQ(blocking.finish(), 0);
    require_value(controller->try_acquire_mutation());
  }

  TEST_F(SessionControllerTest, ClaimsAreIndependentAndChildLeasesRetainTheirParentAndController)
  {
    auto controller = open_controller();
    create_claims(controller);
    auto parent = require_value(controller->try_claim_task(static_cast<TaskId>(1)));
    auto child = require_value(controller->try_claim_task(static_cast<TaskId>(2), parent));
    EXPECT_EQ(parent->task_id(), static_cast<TaskId>(1));
    EXPECT_EQ(child->task_id(), static_cast<TaskId>(2));
    expect_error(controller->try_claim_task(static_cast<TaskId>(1)), ErrorCode::OwnershipBusy);
    expect_error(controller->try_claim_task(static_cast<TaskId>(1), parent), ErrorCode::InvalidArgument);
    parent.reset();
    std::weak_ptr<mapped::SessionController> retained = controller;
    controller.reset();
    ASSERT_FALSE(retained.expired());
    controller = open_controller();
    EXPECT_EQ(controller.get(), retained.lock().get());
    {
      auto guard = require_value(controller->acquire_mutation());
      EXPECT_TRUE(require_value(controller->probe_task_claim(static_cast<TaskId>(1))));
      EXPECT_TRUE(require_value(controller->probe_task_claim(static_cast<TaskId>(2))));
    }
    child.reset();
    EXPECT_FALSE(require_value(controller->probe_task_claim(static_cast<TaskId>(1))));
    EXPECT_FALSE(require_value(controller->probe_task_claim(static_cast<TaskId>(2))));
    require_value(controller->try_claim_task(static_cast<TaskId>(1)));
  }

  TEST_F(SessionControllerTest, ClaimCreationIsExplicitStableAndBounded)
  {
    auto controller = open_controller();
    const auto claim_path = _directory / "claims/task-1.lock";
    expect_error(controller->try_claim_task(static_cast<TaskId>(1)), ErrorCode::IoError);
    EXPECT_FALSE(fs::exists(claim_path));
    mapped::MutationGuard empty;
    expect_error(controller->create_task_claim_file(empty, static_cast<TaskId>(1)), ErrorCode::InvalidState);
    {
      auto guard = require_value(controller->acquire_mutation());
      require_value(controller->create_task_claim_file(guard, static_cast<TaskId>(1)));
      auto file = require_value(mapped::NativeFile::open_existing(claim_path, mapped::FileAccess::ReadOnly));
      const auto identity = require_value(file.identity());
      require_value(controller->create_task_claim_file(guard, static_cast<TaskId>(1)));
      auto same = require_value(mapped::NativeFile::open_existing(claim_path, mapped::FileAccess::ReadOnly));
      EXPECT_EQ(require_value(same.identity()), identity);
      for (const auto task : {TaskId::Invalid, static_cast<TaskId>(5)})
      {
        expect_error(controller->create_task_claim_file(guard, task), ErrorCode::InvalidArgument);
        expect_error(controller->try_claim_task(task), ErrorCode::InvalidArgument);
        expect_error(controller->probe_task_claim(task), ErrorCode::InvalidArgument);
      }
    }
    fs::remove(claim_path);
    auto guard = require_value(controller->acquire_mutation());
    expect_error(controller->create_task_claim_file(guard, static_cast<TaskId>(1)), ErrorCode::IoError);
    EXPECT_FALSE(fs::exists(claim_path));
    EXPECT_FALSE(fs::exists(_directory / "claims/task-0.lock"));
    EXPECT_FALSE(fs::exists(_directory / "claims/task-5.lock"));
  }

  TEST_F(SessionControllerTest, SimultaneousLocalClaimsHaveOneRetainedWinner)
  {
    auto controller = open_controller();
    create_claims(controller, {1});
    std::barrier start{3};
    std::array<std::optional<Result<std::shared_ptr<mapped::TaskLease>>>, 2> results;
    const auto attempt = [&](std::size_t index)
    {
      start.arrive_and_wait();
      results[index].emplace(controller->try_claim_task(static_cast<TaskId>(1)));
    };
    std::thread first(attempt, 0);
    std::thread second(attempt, 1);
    start.arrive_and_wait();
    first.join();
    second.join();
    unsigned winners{};
    for (const auto& result : results)
    {
      ASSERT_TRUE(result);
      if (std::holds_alternative<std::shared_ptr<mapped::TaskLease>>(*result))
      {
        ++winners;
      }
      else
      {
        expect_error(*result, ErrorCode::OwnershipBusy);
      }
    }
    EXPECT_EQ(winners, 1u);
  }

  TEST_F(SessionControllerTest, ClaimExclusionAndProbesWorkAcrossIndependentProcesses)
  {
    auto controller = open_controller();
    create_claims(controller);
    auto claim = require_value(controller->try_claim_task(static_cast<TaskId>(1)));
    Worker duplicate(_directory, "try_claim", "1");
    EXPECT_EQ(duplicate.receive(), 'B');
    EXPECT_EQ(duplicate.finish(), 0);
    Worker independent(_directory, "try_claim", "2");
    EXPECT_EQ(independent.receive(), 'A');
    EXPECT_EQ(independent.finish(), 0);
    claim.reset();
    Worker owner(_directory, "hold_claim", "1");
    EXPECT_EQ(owner.receive(), 'A');
    {
      auto guard = require_value(controller->acquire_mutation());
      EXPECT_TRUE(require_value(controller->probe_task_claim(static_cast<TaskId>(1))));
      expect_error(controller->try_claim_task(static_cast<TaskId>(1)), ErrorCode::OwnershipBusy);
    }
    EXPECT_EQ(owner.finish(), 0);
    EXPECT_FALSE(require_value(controller->probe_task_claim(static_cast<TaskId>(1))));
    require_value(controller->try_claim_task(static_cast<TaskId>(1)));
  }

  TEST_F(SessionControllerTest, MissingMutationLockAndClaimsDirectoryAreNeverRecreatedOnAttach)
  {
    fs::remove(_directory / "mutation.lock");
    expect_error(mapped::SessionController::open_existing(_directory), ErrorCode::IoError);
    EXPECT_FALSE(fs::exists(_directory / "mutation.lock"));
    require_value(mapped::NativeFile::create_exclusive(_directory / "mutation.lock"));
    fs::remove(_directory / "claims");
    expect_error(mapped::SessionController::open_existing(_directory), ErrorCode::IoError);
    EXPECT_FALSE(fs::exists(_directory / "claims"));
  }

  TEST_F(SessionControllerTest, CoordinationFilesRejectSymlinksPipesDirectoriesAndHardLinks)
  {
    const auto lock = _directory / "mutation.lock";
    const auto saved = _directory / "saved.lock";
    fs::rename(lock, saved);
    fs::create_symlink(saved, lock);
    expect_error(mapped::SessionController::open_existing(_directory), ErrorCode::IoError);
    fs::remove(lock);
    ASSERT_EQ(::mkfifo(lock.c_str(), 0600), 0);
    expect_error(mapped::SessionController::open_existing(_directory), ErrorCode::IoError);
    fs::remove(lock);
    fs::create_directory(lock);
    expect_error(mapped::SessionController::open_existing(_directory), ErrorCode::IoError);
    fs::remove(lock);
    fs::create_hard_link(saved, lock);
    expect_error(mapped::SessionController::open_existing(_directory), ErrorCode::IoError);
    fs::remove(lock);
    fs::rename(saved, lock);
    fs::rename(_directory / "claims", _directory / "saved-claims");
    fs::create_directory_symlink(_directory / "saved-claims", _directory / "claims");
    expect_error(mapped::SessionController::open_existing(_directory), ErrorCode::IoError);
  }

  TEST_F(SessionControllerTest, TaskClaimFilesRejectNonregularAndAliasedLockResources)
  {
    auto controller = open_controller();
    const auto claim = _directory / "claims/task-1.lock";
    const auto target = _directory / "claims/target.lock";
    require_value(mapped::NativeFile::create_exclusive(target));
    auto guard = require_value(controller->acquire_mutation());
    fs::create_symlink(target, claim);
    expect_error(controller->try_claim_task(static_cast<TaskId>(1)), ErrorCode::IoError);
    expect_error(controller->create_task_claim_file(guard, static_cast<TaskId>(1)), ErrorCode::IoError);
    fs::remove(claim);
    ASSERT_EQ(::mkfifo(claim.c_str(), 0600), 0);
    expect_error(controller->try_claim_task(static_cast<TaskId>(1)), ErrorCode::IoError);
    fs::remove(claim);
    fs::create_directory(claim);
    expect_error(controller->try_claim_task(static_cast<TaskId>(1)), ErrorCode::IoError);
    fs::remove(claim);
    fs::create_hard_link(target, claim);
    expect_error(controller->try_claim_task(static_cast<TaskId>(1)), ErrorCode::IoError);
  }

  TEST_F(SessionControllerTest, TruncatedFixedSectionsFailBeforeCoordinationChanges)
  {
    auto file = require_value(mapped::NativeFile::open_existing(_directory / "session.ang",
                                                              mapped::FileAccess::ReadWrite));
    require_value(file.resize(mapped::superblock_bytes));
    expect_error(mapped::SessionController::open_existing(_directory), ErrorCode::IoError);
    EXPECT_EQ(require_value(file.size()), mapped::superblock_bytes);
    EXPECT_TRUE(fs::is_empty(_directory / "claims"));
    EXPECT_EQ(fs::file_size(_directory / "mutation.lock"), 0u);
  }

  TEST_F(SessionControllerTest, IncompatibleProfilesAndRequiredFeaturesFailWithoutWrites)
  {
    auto file = require_value(mapped::NativeFile::open_existing(_directory / "session.ang",
                                                              mapped::FileAccess::ReadWrite));
    for (const bool foreign_profile : {true, false})
    {
      auto incompatible = _configuration;
      if (foreign_profile)
      {
        incompatible._liveAbiProfile ^= 1;
      }
      else
      {
        incompatible._requiredFeatures |= std::uint64_t{1} << 63;
      }
      const auto before = require_value(mapped::encode_initial_superblock(incompatible));
      require_value(file.write_exact(0, before));
      expect_error(mapped::SessionController::open_existing(_directory), ErrorCode::IoError);
      mapped::SuperblockBytes after{};
      require_value(file.read_exact(0, after));
      EXPECT_EQ(after, before);
      EXPECT_TRUE(fs::is_empty(_directory / "claims"));
      EXPECT_EQ(fs::file_size(_directory / "mutation.lock"), 0u);
    }
  }

  TEST_F(SessionControllerTest, RetainedFileLimitsAndImmutableConfigurationRemainBoundToTheController)
  {
    auto controller = open_controller();
    {
      auto guard = require_value(controller->acquire_mutation());
      auto* file = require_value(controller->file(guard));
      const auto size = require_value(file->size());
      expect_error(file->resize(_configuration._maxLogBytes + 1), ErrorCode::CapacityExceeded);
      EXPECT_EQ(require_value(file->size()), size);
    }
    for (const bool changed_limit : {true, false})
    {
      auto changed = _configuration;
      if (changed_limit)
      {
        changed._maxLogBytes += 64;
      }
      else
      {
        changed._limits.tasks(3);
      }
      const auto prefix = require_value(mapped::encode_initial_superblock(changed));
      {
        auto guard = require_value(controller->acquire_mutation());
        require_value(require_value(controller->file(guard))->write_exact(0, prefix));
      }
      expect_error(mapped::SessionController::open_existing(_directory), ErrorCode::IoError);
      EXPECT_EQ(controller->configuration()._maxLogBytes, _configuration._maxLogBytes);
      EXPECT_EQ(controller->configuration()._limits._tasks, _configuration._limits._tasks);
    }
  }

  TEST_F(SessionControllerTest, ReplacedCoordinationPathsFailClosedWhileOwnersRemainAlive)
  {
    auto controller = open_controller();
    create_claims(controller, {1});
    const auto claim_path = _directory / "claims/task-1.lock";
    fs::rename(claim_path, _directory / "old-claim.lock");
    require_value(mapped::NativeFile::create_exclusive(claim_path));
    expect_error(controller->try_claim_task(static_cast<TaskId>(1)), ErrorCode::IoError);
    expect_error(controller->probe_task_claim(static_cast<TaskId>(1)), ErrorCode::IoError);
    fs::rename(_directory / "mutation.lock", _directory / "old-mutation.lock");
    require_value(mapped::NativeFile::create_exclusive(_directory / "mutation.lock"));
    expect_error(controller->try_acquire_mutation(), ErrorCode::IoError);
    expect_error(mapped::SessionController::open_existing(_directory), ErrorCode::IoError);
  }

  TEST_F(SessionControllerTest, GuardAndClaimLifetimesRetainCloseOnExecDescriptors)
  {
    auto controller = open_controller();
    create_claims(controller, {1});
    auto claim = require_value(controller->try_claim_task(static_cast<TaskId>(1)));
    auto guard = require_value(controller->acquire_mutation());
    const auto gate_fd = descriptor_for(_directory / "mutation.lock");
    const auto claim_fd = descriptor_for(_directory / "claims/task-1.lock");
    EXPECT_NE(::fcntl(gate_fd, F_GETFD) & FD_CLOEXEC, 0);
    EXPECT_NE(::fcntl(claim_fd, F_GETFD) & FD_CLOEXEC, 0);
    Worker exec(_directory, "check_closed", std::to_string(gate_fd), std::to_string(claim_fd));
    EXPECT_EQ(exec.receive(), 'A');
    EXPECT_EQ(exec.finish(), 0);
    std::weak_ptr<mapped::SessionController> retained = controller;
    controller.reset();
    EXPECT_FALSE(retained.expired());
    guard = mapped::MutationGuard{};
    EXPECT_FALSE(retained.expired());
    require_value(claim->check_owner());
    claim.reset();
    EXPECT_TRUE(retained.expired());
    errno = 0;
    EXPECT_EQ(::fcntl(gate_fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    errno = 0;
    EXPECT_EQ(::fcntl(claim_fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
  }

  TEST_F(SessionControllerTest, TerminatedProcessReleasesItsMutationGateAndTaskClaim)
  {
    auto controller = open_controller();
    create_claims(controller, {1});
    Worker owner(_directory, "hold_both");
    EXPECT_EQ(owner.receive(), 'A');
    EXPECT_TRUE(require_value(controller->probe_task_claim(static_cast<TaskId>(1))));
    expect_error(controller->try_acquire_mutation(), ErrorCode::OwnershipBusy);
    owner.kill_owner();
    auto claim = require_value(controller->try_claim_task(static_cast<TaskId>(1)));
    auto guard = require_value(controller->try_acquire_mutation());
    require_value(claim->check_owner());
    require_value(guard.check_owner());
  }

  TEST_F(SessionControllerTest, ForkChildRejectsInheritedOwnersWithoutUnlockingItsParent)
  {
    auto controller = open_controller();
    create_claims(controller, {1});
    Worker owner(_directory, "fork_reject");
    EXPECT_EQ(owner.receive(), 'A');
    expect_error(controller->try_acquire_mutation(), ErrorCode::OwnershipBusy);
    expect_error(controller->try_claim_task(static_cast<TaskId>(1)), ErrorCode::OwnershipBusy);
    EXPECT_EQ(owner.finish(), 0);
    require_value(controller->try_acquire_mutation());
    require_value(controller->try_claim_task(static_cast<TaskId>(1)));
  }

  TEST_F(SessionControllerTest, SurvivingForkChildCannotRetainItsTerminatedParentsLocks)
  {
    auto controller = open_controller();
    create_claims(controller, {1});
    Worker owner(_directory, "fork_holder");
    const auto child = owner.receive_pid();
    ASSERT_GT(child, 0u);
    EXPECT_EQ(owner.receive(), 'C');
    ASSERT_EQ(::kill(static_cast<pid_t>(child), 0), 0);
    owner.kill_owner();
    ASSERT_EQ(::kill(static_cast<pid_t>(child), 0), 0);
    // The fork child is still alive here. Release therefore demonstrates that
    // it did not inherit a lock-retaining open description from the owner.
    auto claim = require_value(controller->try_claim_task(static_cast<TaskId>(1)));
    auto guard = require_value(controller->try_acquire_mutation());
    require_value(claim->check_owner());
    require_value(guard.check_owner());
  }
} // namespace
