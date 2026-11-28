// Native qualification for the private atomic adapter used by Session recovery.
// These tests exercise actual shared mappings and independent processes on this
// host; they do not establish power-loss durability or every possible schedule.

#include "result_assertions.h"
#include "native_atomic.h"
#include "native_file.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ANGARIA_NATIVE_ATOMIC_WORKER
#error ANGARIA_NATIVE_ATOMIC_WORKER must name the native atomic qualification worker
#endif

extern char** environ;

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;
  namespace fs = std::filesystem;
  using Clock = std::chrono::steady_clock;

  static_assert(!std::is_default_constructible_v<mapped::AtomicWord>);
  static_assert(!std::is_default_constructible_v<mapped::AtomicReadWord>);

  using angaria::test_support::require_value;

  using angaria::test_support::expect_error;

  class Worker
  {
  public:
    Worker(const char* mode, const fs::path& path)
    {
      auto worker = std::string{ANGARIA_NATIVE_ATOMIC_WORKER};
      auto argument = path.string();
      auto mode_argument = std::string{mode};
      std::array<char*, 4> arguments{worker.data(), mode_argument.data(), argument.data(), nullptr};
      const int error = ::posix_spawn(&_pid, worker.c_str(), nullptr, nullptr, arguments.data(), environ);
      if (error != 0)
      {
        _pid = -1;
        throw std::system_error(error, std::generic_category(), "start native atomic worker");
      }
    }

    ~Worker()
    {
      if (_pid > 0)
      {
        ::kill(_pid, SIGKILL);
        int status;
        while (::waitpid(_pid, &status, 0) == -1 && errno == EINTR)
        {
        }
      }
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void terminate()
    {
      if (::kill(_pid, SIGKILL) == -1)
      {
        throw std::system_error(errno, std::generic_category(), "terminate atomic worker");
      }
    }

    int wait()
    {
      const auto deadline = Clock::now() + std::chrono::seconds{5};
      for (;;)
      {
        int status;
        const auto result = ::waitpid(_pid, &status, WNOHANG);
        if (result == _pid)
        {
          _pid = -1;
          return status;
        }
        if (result == -1 && errno != EINTR)
        {
          throw std::system_error(errno, std::generic_category(), "wait for atomic worker");
        }
        if (Clock::now() >= deadline)
        {
          throw std::runtime_error("native atomic worker did not exit");
        }
        std::this_thread::yield();
      }
    }

  private:
    pid_t _pid{-1};
  };

  class NativeAtomicTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-atomic-XXXXXX").string();
      const auto directory = ::mkdtemp(pattern.data());
      ASSERT_NE(directory, nullptr);
      _root = directory;
      _path = _root / "atomic.ang";
    }

    void TearDown() override
    {
      std::error_code ignored;
      if (!_root.empty())
      {
        fs::remove_all(_root, ignored);
      }
    }

    mapped::NativeFile create_file()
    {
      auto file = require_value(mapped::NativeFile::create_exclusive(_path, 1048576));
      require_value(file.resize(64));
      return file;
    }

    fs::path _root;
    fs::path _path;
  };

  // Portable decoding can inspect foreign metadata, but live atomic access
  // requires this compiler/OS/CPU profile and synchronization version. Qualify
  // the host, then reject compatibility mismatches before live use.
  TEST_F(NativeAtomicTest, QualifiesOnlyTheActualNativeProfile)
  {
    const auto supported = require_value(mapped::qualify_native_profile());
#if defined(__linux__) && defined(__x86_64__)
    EXPECT_EQ(supported._liveAbiProfile, 0x00010001u);
#elif defined(__linux__) && defined(__aarch64__)
    EXPECT_EQ(supported._liveAbiProfile, 0x00010002u);
#elif defined(__APPLE__) && defined(__x86_64__)
    EXPECT_EQ(supported._liveAbiProfile, 0x00020001u);
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    EXPECT_EQ(supported._liveAbiProfile, 0x00020002u);
#else
    FAIL() << "native atomic qualification requires a supported test host";
#endif
    EXPECT_FALSE(supported._segmentedStorage);
    mapped::SuperblockConfiguration configuration;
    configuration._session = {1, 2};
    configuration._limits = CapacityLimits::empty();
    configuration._maxLogBytes = 1048576;
    configuration._liveAbiProfile = supported._liveAbiProfile;
    require_value(mapped::check_live_compatibility(configuration, supported));
    configuration._liveAbiProfile = 0x7fffffff;
    expect_error(mapped::check_live_compatibility(configuration, supported), ErrorCode::IoError);
    configuration._liveAbiProfile = supported._liveAbiProfile;
    ++configuration._synchronizationVersion;
    expect_error(mapped::check_live_compatibility(configuration, supported), ErrorCode::IoError);
  }

  // An AtomicWord borrows one naturally aligned eight-byte location. Binding
  // validates the actual address and complete range without changing storage;
  // sentinel bytes around the valid word reveal accidental neighboring writes.
  TEST_F(NativeAtomicTest, RejectsInvalidRangesAndAlignmentWithoutWrites)
  {
    alignas(8) std::array<std::byte, 24> bytes;
    bytes.fill(std::byte{0x5a});
    const auto before = bytes;
    expect_error(mapped::AtomicWord::bind({}, 0), ErrorCode::InvalidArgument);
    expect_error(mapped::AtomicWord::bind(std::span{bytes}.first(7), 0), ErrorCode::InvalidArgument);
    expect_error(mapped::AtomicWord::bind(bytes, 17), ErrorCode::InvalidArgument);
    expect_error(mapped::AtomicWord::bind(bytes, 24), ErrorCode::InvalidArgument);
    expect_error(mapped::AtomicWord::bind(bytes, std::numeric_limits<std::size_t>::max()),
                 ErrorCode::InvalidArgument);
    expect_error(mapped::AtomicWord::bind(bytes, 1), ErrorCode::InvalidArgument);
    EXPECT_EQ(bytes, before);

    // Alignment follows the actual address, not just the offset within a view.
    auto word = require_value(mapped::AtomicWord::bind(std::span{bytes}.subspan(1), 7));
    word.store_release(0x0123456789abcdef);
    EXPECT_EQ(word.load_acquire(), 0x0123456789abcdefu);
    word.publish_control(0xfedcba9876543210);
    EXPECT_EQ(word.load_acquire(), 0xfedcba9876543210u);
    EXPECT_TRUE(std::all_of(bytes.begin(), bytes.begin() + 8,
                            [](std::byte value)
                            {
                              return value == std::byte{0x5a};
                            }));
    EXPECT_TRUE(std::all_of(bytes.begin() + 16, bytes.end(),
                            [](std::byte value)
                            {
                              return value == std::byte{0x5a};
                            }));
  }

  TEST_F(NativeAtomicTest, ReadOnlyMappingAcquiresPublicationWithoutWritableCapability)
  {
    // A consumer maps the producer's file read-only and binds a load-only word.
    // It observes released values without requiring a writable mapping or cast.
    auto file = create_file();
    auto writable = require_value(file.map_read_write(0, 64));
    const auto bytes = require_value(writable.writable_bytes());
    std::fill(bytes.begin() + 8, bytes.end(), std::byte{0x5a});
    auto publisher = require_value(mapped::AtomicWord::bind(bytes, 0));
    publisher.store_release(0x0123456789abcdef);
    auto readOnly = require_value(mapped::NativeFile::open_existing(_path, mapped::FileAccess::ReadOnly));
    auto mapping = require_value(readOnly.map_read_only(0, 64));
    const auto observed = require_value(mapping.bytes());
    auto reader = require_value(mapped::AtomicReadWord::bind(observed, 0));
    EXPECT_EQ(reader.load_acquire(), 0x0123456789abcdefu);

    expect_error(mapped::AtomicReadWord::bind({}, 0), ErrorCode::InvalidArgument);
    expect_error(mapped::AtomicReadWord::bind(observed.first(7), 0), ErrorCode::InvalidArgument);
    expect_error(mapped::AtomicReadWord::bind(observed, 1), ErrorCode::InvalidArgument);
    expect_error(mapped::AtomicReadWord::bind(observed, 57), ErrorCode::InvalidArgument);
    expect_error(mapped::AtomicReadWord::bind(observed, std::numeric_limits<std::size_t>::max()),
                 ErrorCode::InvalidArgument);
    EXPECT_EQ(reader.load_acquire(), 0x0123456789abcdefu);
    EXPECT_TRUE(std::all_of(observed.begin() + 8, observed.end(), [](std::byte value)
    {
      return value == std::byte{0x5a};
    }));

    // Publication through the writer remains visible through the retained
    // read-only binding; no rebind or mapping refresh is needed.
    publisher.store_release(0xfedcba9876543210);
    EXPECT_EQ(reader.load_acquire(), 0xfedcba9876543210u);
  }

  // The worker fills ordinary body bytes, publishes a control generation, then
  // grows the file. Observing that later effect must expose both control and
  // preceding body; acknowledgments prevent overlapping body generations.
  TEST_F(NativeAtomicTest, IndependentProcessPublishesBodyBeforeFollowingFileGrowth)
  {
    auto file = create_file();
    auto region = require_value(file.map_read_write(0, 64));
    const auto bytes = require_value(region.writable_bytes());
    auto control = require_value(mapped::AtomicWord::bind(bytes, 0));
    auto acknowledgment = require_value(mapped::AtomicWord::bind(bytes, 8));
    control.store_release(0);
    acknowledgment.store_release(0);
    Worker worker{"publication", _path};
    const auto deadline = Clock::now() + std::chrono::seconds{5};
    for (std::uint64_t generation = 1; generation <= 256; ++generation)
    {
      while (require_value(file.size()) != 64 + generation)
      {
        ASSERT_LT(Clock::now(), deadline) << "following file growth was not observed";
        std::this_thread::yield();
      }
      ASSERT_EQ(control.load_acquire(), generation);
      for (std::size_t index = 16; index < 48; ++index)
      {
        EXPECT_EQ(bytes[index], static_cast<std::byte>((generation + 17 * (index - 16)) & 0xff));
      }
      acknowledgment.store_release(generation);
    }
    const int status = worker.wait();
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
  }

  // Sample alternating eight-byte patterns and kill their independent writer.
  // Every sample, including the retained post-kill word, must be one complete
  // pattern; this qualifies process-crash behavior on the tested host.
  TEST_F(NativeAtomicTest, TerminatedWriterLeavesAnIndivisibleNativeWord)
  {
    auto file = create_file();
    auto region = require_value(file.map_read_write(0, 64));
    const auto bytes = require_value(region.writable_bytes());
    auto value = require_value(mapped::AtomicWord::bind(bytes, 0));
    auto ready = require_value(mapped::AtomicWord::bind(bytes, 8));
    value.store_release(0xaaaaaaaa55555555);
    ready.store_release(0);
    Worker worker{"crash", _path};
    const auto deadline = Clock::now() + std::chrono::seconds{5};
    while (ready.load_acquire() != 1)
    {
      ASSERT_LT(Clock::now(), deadline) << "atomic crash worker did not start";
      std::this_thread::yield();
    }
    for (unsigned sample = 0; sample < 200000; ++sample)
    {
      const auto observed = value.load_acquire();
      ASSERT_TRUE(observed == 0xaaaaaaaa55555555 || observed == 0x55555555aaaaaaaa);
    }
    worker.terminate();
    const int status = worker.wait();
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGKILL);
    const auto retained = value.load_acquire();
    EXPECT_TRUE(retained == 0xaaaaaaaa55555555 || retained == 0x55555555aaaaaaaa);
    require_value(file.resize(128));
    value.publish_control(3);
    EXPECT_EQ(value.load_acquire(), 3u);
  }
} // namespace
