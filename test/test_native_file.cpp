#include "mapped_superblock.h"
#include "native_file.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ANGARIA_TEST_PYTHON
#define ANGARIA_TEST_PYTHON "python3"
#endif

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;
  namespace fs = std::filesystem;

  static_assert(!std::is_copy_constructible_v<mapped::NativeFile>);
  static_assert(!std::is_copy_assignable_v<mapped::NativeFile>);
  static_assert(std::is_nothrow_move_constructible_v<mapped::NativeFile>);
  static_assert(std::is_nothrow_move_assignable_v<mapped::NativeFile>);
  static_assert(!std::is_copy_constructible_v<mapped::MappedRegion>);
  static_assert(!std::is_copy_assignable_v<mapped::MappedRegion>);
  static_assert(std::is_nothrow_move_constructible_v<mapped::MappedRegion>);
  static_assert(std::is_nothrow_move_assignable_v<mapped::MappedRegion>);

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
    EXPECT_EQ(error._code, code);
    EXPECT_FALSE(error._path.empty());
    EXPECT_FALSE(error._message.empty());
  }

  class NativeFileTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-native-XXXXXX").string();
      const auto directory = ::mkdtemp(pattern.data());
      ASSERT_NE(directory, nullptr);
      _root = directory;
      _path = _root / "storage.ang";
    }

    void TearDown() override
    {
      std::error_code ignored;
      if (!_root.empty())
      {
        fs::remove_all(_root, ignored);
      }
    }

    mapped::NativeFile create_file(std::uint64_t size = 64, std::uint64_t limit = 1048576)
    {
      auto file = require_value(mapped::NativeFile::create_exclusive(_path, limit));
      require_value(file.resize(size));
      return file;
    }

    static int wait_for(pid_t child)
    {
      int status{};
      pid_t result;
      do
      {
        result = ::waitpid(child, &status, 0);
      } while (result == -1 && errno == EINTR);
      if (result != child)
      {
        throw std::runtime_error("waitpid failed");
      }
      return status;
    }

    fs::path _root;
    fs::path _path;
  };

  TEST_F(NativeFileTest, ExclusiveCreationAndExistingOpenPreserveContents)
  {
    auto file = require_value(mapped::NativeFile::create_exclusive(_path, 64));
    EXPECT_EQ(require_value(file.size()), 0u);
    require_value(file.resize(16));
    const std::array data{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}};
    require_value(file.write_exact(4, data));

    expect_error(mapped::NativeFile::create_exclusive(_path, 64), ErrorCode::Conflict);
    auto reopened = require_value(mapped::NativeFile::open_existing(_path, mapped::FileAccess::ReadWrite, 64));
    EXPECT_EQ(require_value(reopened.size()), 16u);
    EXPECT_EQ(require_value(reopened.identity()), require_value(file.identity()));
    std::array<std::byte, 3> observed{};
    require_value(reopened.read_exact(4, observed));
    EXPECT_EQ(observed, data);

    const int fd = require_value(file.native_handle());
    const int flags = ::fcntl(fd, F_GETFD);
    ASSERT_NE(flags, -1);
    EXPECT_NE(flags & FD_CLOEXEC, 0);
  }

  TEST_F(NativeFileTest, ExactIoDoesNotGrowTheFileOrChangeItsCursor)
  {
    auto file = create_file(16, 64);
    const int fd = require_value(file.native_handle());
    ASSERT_EQ(::lseek(fd, 7, SEEK_SET), 7);
    const std::array data{std::byte{1}, std::byte{2}, std::byte{3}};
    require_value(file.write_exact(4, data));
    std::array<std::byte, 16> observed{};
    require_value(file.read_exact(0, observed));
    EXPECT_EQ(observed[3], std::byte{0});
    EXPECT_TRUE(std::equal(data.begin(), data.end(), observed.begin() + 4));
    EXPECT_EQ(observed[7], std::byte{0});
    EXPECT_EQ(::lseek(fd, 0, SEEK_CUR), 7);

    expect_error(file.write_exact(15, data), ErrorCode::IoError);
    expect_error(file.read_exact(14, std::span{observed}.first(3)), ErrorCode::IoError);
    EXPECT_EQ(require_value(file.size()), 16u);
    std::array<std::byte, 16> unchanged{};
    require_value(file.read_exact(0, unchanged));
    EXPECT_EQ(unchanged, observed);
    require_value(file.read_exact(16, std::span<std::byte>{}));
    require_value(file.write_exact(16, std::span<const std::byte>{}));
    expect_error(file.read_exact(17, std::span<std::byte>{}), ErrorCode::IoError);
  }

  TEST_F(NativeFileTest, InvalidInputsAndByteLimitsFailBeforeChangingStorage)
  {
    expect_error(mapped::NativeFile::create_exclusive({}, 64), ErrorCode::InvalidArgument);
    expect_error(mapped::NativeFile::create_exclusive(_path, 0), ErrorCode::InvalidArgument);
    expect_error(mapped::NativeFile::create_exclusive(_path, std::numeric_limits<std::uint64_t>::max()),
                 ErrorCode::InvalidArgument);
    const auto nul_path = _root / std::string{"prefix\0suffix", 13};
    expect_error(mapped::NativeFile::create_exclusive(nul_path, 64), ErrorCode::InvalidArgument);
    EXPECT_FALSE(fs::exists(_root / "prefix"));
    EXPECT_FALSE(fs::exists(_path));
    expect_error(mapped::NativeFile::open_existing(_path, static_cast<mapped::FileAccess>(99), 64),
                 ErrorCode::InvalidArgument);
    expect_error(mapped::NativeFile::open_existing(_path, mapped::FileAccess::ReadOnly, 64), ErrorCode::IoError);
    EXPECT_FALSE(fs::exists(_path));

    auto file = create_file(16, 64);
    expect_error(file.resize(65), ErrorCode::CapacityExceeded);
    expect_error(file.resize(std::numeric_limits<std::uint64_t>::max()), ErrorCode::CapacityExceeded);
    expect_error(mapped::NativeFile::open_existing(_path, mapped::FileAccess::ReadOnly, 15), ErrorCode::IoError);
    EXPECT_EQ(require_value(file.size()), 16u);
  }

  TEST_F(NativeFileTest, SymlinksDirectoriesAndFifosAreRejected)
  {
    auto file = create_file();
    const auto link = _root / "symlink";
    fs::create_symlink(_path, link);
    expect_error(mapped::NativeFile::open_existing(link, mapped::FileAccess::ReadOnly), ErrorCode::IoError);
    expect_error(mapped::NativeFile::open_existing(link, mapped::FileAccess::ReadWrite), ErrorCode::IoError);
    expect_error(mapped::NativeFile::open_existing(_root, mapped::FileAccess::ReadOnly), ErrorCode::IoError);
    const auto fifo = _root / "fifo";
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    expect_error(mapped::NativeFile::open_existing(fifo, mapped::FileAccess::ReadOnly), ErrorCode::IoError);
    expect_error(mapped::NativeFile::open_existing(fifo, mapped::FileAccess::ReadWrite), ErrorCode::IoError);
    EXPECT_EQ(require_value(file.size()), 64u);
  }

  TEST_F(NativeFileTest, ReadOnlyCapabilitiesRemainSeparateAcrossAliases)
  {
    auto writable = create_file(16);
    auto read_only = require_value(mapped::NativeFile::open_existing(_path, mapped::FileAccess::ReadOnly));
    const std::array data{std::byte{0x5a}};
    expect_error(read_only.write_exact(0, data), ErrorCode::InvalidState);
    expect_error(read_only.resize(16), ErrorCode::InvalidState);
    expect_error(read_only.map_read_write(0, 1), ErrorCode::InvalidState);
    auto read_view = require_value(read_only.map_read_only(0, 16));
    expect_error(read_view.writable_bytes(), ErrorCode::InvalidState);
    auto restricted_view = require_value(writable.map_read_only(0, 1));
    expect_error(restricted_view.writable_bytes(), ErrorCode::InvalidState);

    auto write_view = require_value(writable.map_read_write(0, 16));
    require_value(write_view.writable_bytes())[0] = data[0];
    EXPECT_EQ(require_value(read_view.bytes())[0], data[0]);
    std::array<std::byte, 1> observed{};
    require_value(read_only.read_exact(0, observed));
    EXPECT_EQ(observed, data);
  }

  TEST_F(NativeFileTest, IndependentHandleLimitsRemainSeparateAcrossSharedInodeState)
  {
    auto owner = create_file(16, 64);
    const std::array<std::byte, 8> data{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8},
    };
    require_value(owner.write_exact(0, data));
    auto limited = require_value(mapped::NativeFile::open_existing(_path, mapped::FileAccess::ReadWrite, 16));
    EXPECT_EQ(require_value(limited.identity()), require_value(owner.identity()));
    auto region = require_value(limited.map_read_only(0, data.size()));

    require_value(owner.resize(32));
    const auto retained = require_value(region.bytes());
    ASSERT_EQ(retained.size(), data.size());
    EXPECT_TRUE(std::equal(retained.begin(), retained.end(), data.begin()));
    expect_error(limited.size(), ErrorCode::IoError);
    std::array<std::byte, 8> observed{};
    expect_error(limited.read_exact(0, observed), ErrorCode::IoError);
    expect_error(limited.map_read_only(0, 1), ErrorCode::IoError);
    expect_error(limited.resize(17), ErrorCode::CapacityExceeded);
    EXPECT_EQ(require_value(owner.size()), 32u);

    require_value(owner.resize(16));
    EXPECT_EQ(require_value(limited.size()), 16u);
    require_value(limited.read_exact(0, observed));
    EXPECT_EQ(observed, data);
    auto restored = require_value(limited.map_read_only(0, data.size()));
    EXPECT_EQ(require_value(restored.bytes()).size(), data.size());
  }

  TEST_F(NativeFileTest, SuperblockCodecRoundTripsThroughNativeFileAndReadOnlyMapping)
  {
    mapped::SuperblockConfiguration configuration;
    configuration._session = {0x0123456789abcdef, 0xfedcba9876543210};
    configuration._liveAbiProfile = 0x00010002;
    configuration._maxLogBytes = 65536;
    configuration._limits = CapacityLimits::empty()
                                .tasks(2)
                                .queues(3)
                                .source_sets(1)
                                .publishing_sets(1)
                                .memberships_per_set(3)
                                .fields(4)
                                .message_schemas(2)
                                .symbols(5)
                                .string_pool_bytes(17);
    const auto layout = mapped::make_fixed_layout(configuration._limits);
    ASSERT_TRUE(layout);
    const auto prefix = require_value(mapped::encode_initial_superblock(configuration));
    auto file = require_value(mapped::NativeFile::create_exclusive(_path, configuration._maxLogBytes));
    require_value(file.resize(layout->_arenaBegin));
    require_value(file.write_exact(0, prefix));

    // These are provisional storage bytes, not a bootstrapped usable Session.
    auto region = require_value(file.map_read_only(0, mapped::superblock_bytes));
    const auto bytes = require_value(region.bytes());
    ASSERT_EQ(bytes.size(), mapped::superblock_bytes);
    const auto decoded = require_value(mapped::decode_superblock(bytes));
    EXPECT_EQ(require_value(file.size()), layout->_arenaBegin);
    EXPECT_EQ(decoded._session, configuration._session);
    EXPECT_EQ(decoded._maxLogBytes, configuration._maxLogBytes);
    EXPECT_EQ(decoded._limits._tasks, configuration._limits._tasks);
    EXPECT_EQ(decoded._limits._queues, configuration._limits._queues);
    EXPECT_EQ(decoded._limits._sourceSets, configuration._limits._sourceSets);
    EXPECT_EQ(decoded._limits._publishingSets, configuration._limits._publishingSets);
    EXPECT_EQ(decoded._limits._membershipsPerSet, configuration._limits._membershipsPerSet);
    EXPECT_EQ(decoded._limits._fields, configuration._limits._fields);
    EXPECT_EQ(decoded._limits._messageSchemas, configuration._limits._messageSchemas);
    EXPECT_EQ(decoded._limits._symbols, configuration._limits._symbols);
    EXPECT_EQ(decoded._limits._stringPoolBytes, configuration._limits._stringPoolBytes);
  }

  TEST_F(NativeFileTest, UnalignedMappingsExposeOnlyTheRequestedHostPageRange)
  {
    const long page = ::sysconf(_SC_PAGESIZE);
    ASSERT_GT(page, 0);
    const auto page_bytes = static_cast<std::uint64_t>(page);
    auto file = create_file(2 * page_bytes + 31, 4 * page_bytes);
    const auto offset = page_bytes - 3;
    const auto length = page_bytes + 10;
    auto region = require_value(file.map_read_write(offset, length));
    auto bytes = require_value(region.writable_bytes());
    ASSERT_EQ(bytes.size(), length);
    bytes.front() = std::byte{0xa5};
    bytes.back() = std::byte{0x3c};
    std::array<std::byte, 1> observed{};
    require_value(file.read_exact(offset, observed));
    EXPECT_EQ(observed[0], std::byte{0xa5});
    require_value(file.read_exact(offset + length - 1, observed));
    EXPECT_EQ(observed[0], std::byte{0x3c});
    auto later_page = require_value(file.map_read_only(page_bytes + 5, 9));
    EXPECT_EQ(require_value(later_page.bytes()).size(), 9u);
  }

  TEST_F(NativeFileTest, InvalidMappingAndIoRangesAreRejectedBeforeAccess)
  {
    auto file = create_file(64);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    expect_error(file.map_read_only(0, 0), ErrorCode::InvalidArgument);
    expect_error(file.map_read_write(maximum, 1), ErrorCode::InvalidArgument);
    expect_error(file.map_read_only(1, maximum), ErrorCode::InvalidArgument);
    expect_error(file.map_read_only(mapped::max_log_bytes, 2), ErrorCode::InvalidArgument);
    expect_error(file.map_read_only(64, 1), ErrorCode::IoError);
    expect_error(file.map_read_write(63, 2), ErrorCode::IoError);
    std::array<std::byte, 1> byte{};
    expect_error(file.read_exact(maximum, byte), ErrorCode::InvalidArgument);
    expect_error(file.write_exact(maximum, byte), ErrorCode::InvalidArgument);
    EXPECT_EQ(require_value(file.size()), 64u);
  }

  TEST_F(NativeFileTest, GrowthPreservesExistingViewsAndZeroExtendsTheFile)
  {
    auto file = create_file(16, 128);
    std::array<std::byte, 16> original;
    original.fill(std::byte{0x71});
    require_value(file.write_exact(0, original));
    auto region = require_value(file.map_read_only(3, 5));
    const auto before = require_value(region.bytes());
    require_value(file.resize(128));
    const auto after = require_value(region.bytes());
    EXPECT_EQ(after.data(), before.data());
    EXPECT_EQ(after.size(), 5u);
    EXPECT_TRUE(std::all_of(after.begin(), after.end(),
                            [](std::byte value)
                            {
                              return value == std::byte{0x71};
                            }));
    std::array<std::byte, 112> added;
    added.fill(std::byte{0xff});
    require_value(file.read_exact(16, added));
    EXPECT_TRUE(std::all_of(added.begin(), added.end(),
                            [](std::byte value)
                            {
                              return value == std::byte{0};
                            }));
    auto last_byte = require_value(file.map_read_only(127, 1));
    EXPECT_EQ(require_value(last_byte.bytes())[0], std::byte{0});
  }

  TEST_F(NativeFileTest, ShrinkProtectionCoversIndependentHandlesAndHardLinks)
  {
    auto file = create_file(64);
    const auto hard_link = _root / "hardlink";
    fs::create_hard_link(_path, hard_link);
    auto alias = require_value(mapped::NativeFile::open_existing(hard_link, mapped::FileAccess::ReadWrite));
    auto read_only = require_value(mapped::NativeFile::open_existing(_path, mapped::FileAccess::ReadOnly));
    EXPECT_EQ(require_value(alias.identity()), require_value(file.identity()));
    auto region = require_value(read_only.map_read_only(8, 24));
    expect_error(file.resize(31), ErrorCode::OwnershipBusy);
    expect_error(alias.resize(31), ErrorCode::OwnershipBusy);
    EXPECT_EQ(require_value(file.size()), 64u);
    require_value(alias.resize(32));
    require_value(file.resize(32));
    EXPECT_EQ(require_value(region.bytes()).size(), 24u);
    region = mapped::MappedRegion{};
    require_value(alias.resize(0));
    EXPECT_EQ(require_value(file.size()), 0u);
  }

  TEST_F(NativeFileTest, MappingRetainsItsFileAndLeaseAfterTheFileWrapperCloses)
  {
    mapped::MappedRegion region;
    int retained_fd = -1;
    {
      auto file = create_file(32);
      retained_fd = require_value(file.native_handle());
      region = require_value(file.map_read_write(4, 8));
    }
    EXPECT_NE(::fcntl(retained_fd, F_GETFD), -1);
    require_value(region.writable_bytes())[0] = std::byte{0x63};
    auto alias = require_value(mapped::NativeFile::open_existing(_path, mapped::FileAccess::ReadWrite));
    expect_error(alias.resize(11), ErrorCode::OwnershipBusy);
    std::array<std::byte, 1> observed{};
    require_value(alias.read_exact(4, observed));
    EXPECT_EQ(observed[0], std::byte{0x63});
    region = mapped::MappedRegion{};
    errno = 0;
    EXPECT_EQ(::fcntl(retained_fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    require_value(alias.resize(11));
  }

  TEST_F(NativeFileTest, MovesTransferOwnershipAndReleasePreviousResources)
  {
    mapped::NativeFile empty_file;
    mapped::MappedRegion empty_region;
    expect_error(empty_file.size(), ErrorCode::InvalidState);
    expect_error(empty_region.bytes(), ErrorCode::InvalidState);
    auto original = create_file(64);
    const auto identity = require_value(original.identity());
    mapped::NativeFile moved{std::move(original)};
    expect_error(original.size(), ErrorCode::InvalidState);
    EXPECT_EQ(require_value(moved.identity()), identity);

    auto destination = require_value(mapped::NativeFile::create_exclusive(_root / "other.ang"));
    const int replaced_fd = require_value(destination.native_handle());
    destination = std::move(moved);
    expect_error(moved.native_handle(), ErrorCode::InvalidState);
    EXPECT_EQ(require_value(destination.identity()), identity);
    errno = 0;
    EXPECT_EQ(::fcntl(replaced_fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);

    auto high = require_value(destination.map_read_only(48, 12));
    auto low = require_value(destination.map_read_only(4, 8));
    const auto borrowed = require_value(low.bytes());
    mapped::MappedRegion transferred{std::move(low)};
    expect_error(low.bytes(), ErrorCode::InvalidState);
    EXPECT_EQ(require_value(transferred.bytes()).data(), borrowed.data());
    high = std::move(transferred);
    expect_error(transferred.bytes(), ErrorCode::InvalidState);
    require_value(destination.resize(12));
    EXPECT_EQ(require_value(high.bytes()).size(), 8u);
  }

  TEST_F(NativeFileTest, EqualRangeEndsKeepIndependentMappingLeases)
  {
    auto file = create_file(32);
    auto first = require_value(file.map_read_only(0, 16));
    auto second = require_value(file.map_read_only(8, 8));
    first = mapped::MappedRegion{};
    expect_error(file.resize(15), ErrorCode::OwnershipBusy);
    second = mapped::MappedRegion{};
    require_value(file.resize(15));
  }

  TEST_F(NativeFileTest, FreshSizeChecksRejectExternallyTruncatedRanges)
  {
    auto file = create_file(64, 128);
    const int external = ::open(_path.c_str(), O_WRONLY | O_CLOEXEC);
    ASSERT_NE(external, -1);
    ASSERT_EQ(::ftruncate(external, 8), 0);
    ASSERT_EQ(::close(external), 0);
    EXPECT_EQ(require_value(file.size()), 8u);
    std::array<std::byte, 1> byte{};
    expect_error(file.read_exact(8, byte), ErrorCode::IoError);
    expect_error(file.write_exact(8, byte), ErrorCode::IoError);
    expect_error(file.map_read_only(0, 64), ErrorCode::IoError);
    auto short_view = require_value(file.map_read_only(0, 8));
    EXPECT_EQ(require_value(short_view.bytes()).size(), 8u);
  }

  TEST_F(NativeFileTest, ConcurrentMapAndAliasShrinkCannotBothInvalidateTheView)
  {
    auto file = create_file(16);
    auto alias = require_value(mapped::NativeFile::open_existing(_path, mapped::FileAccess::ReadWrite));
    std::barrier start{3};
    std::optional<Result<mapped::MappedRegion>> mapped_result;
    std::optional<OperationResult> resize_result;
    std::thread mapper([&]
                       {
                         start.arrive_and_wait();
                         mapped_result.emplace(file.map_read_only(8, 8));
                       });
    std::thread resizer([&]
                        {
                          start.arrive_and_wait();
                          resize_result.emplace(alias.resize(8));
                        });
    start.arrive_and_wait();
    mapper.join();
    resizer.join();
    ASSERT_TRUE(mapped_result);
    ASSERT_TRUE(resize_result);
    if (std::holds_alternative<mapped::MappedRegion>(*mapped_result))
    {
      expect_error(*resize_result, ErrorCode::OwnershipBusy);
      EXPECT_EQ(require_value(std::get<mapped::MappedRegion>(*mapped_result).bytes()).size(), 8u);
    }
    else
    {
      expect_error(*mapped_result, ErrorCode::IoError);
      require_value(std::move(*resize_result));
      EXPECT_EQ(require_value(file.size()), 8u);
    }
    mapped_result.reset();
    require_value(alias.resize(8));
  }

  TEST_F(NativeFileTest, InheritedObjectsAndFactoriesRejectQuiescentForkUse)
  {
    auto file = create_file(16);
    auto region = require_value(file.map_read_only(0, 16));
    const pid_t child = ::fork();
    ASSERT_NE(child, -1);
    if (child == 0)
    {
      const auto inherited_file = file.size();
      const auto inherited_region = region.bytes();
      const auto fresh_open = mapped::NativeFile::open_existing(_path, mapped::FileAccess::ReadOnly);
      const auto wrong_owner = [](const auto& result)
      {
        const auto* error = std::get_if<Error>(&result);
        return error != nullptr && error->_code == ErrorCode::WrongOwner;
      };
      ::_exit(wrong_owner(inherited_file) && wrong_owner(inherited_region) && wrong_owner(fresh_open) ? 0 : 1);
    }
    const int status = wait_for(child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(require_value(region.bytes()).size(), 16u);
    EXPECT_EQ(require_value(file.size()), 16u);
  }

  TEST_F(NativeFileTest, IndependentExecProcessObservesAndUpdatesTheSharedMapping)
  {
    auto file = create_file(16);
    auto region = require_value(file.map_read_write(0, 16));
    auto bytes = require_value(region.writable_bytes());
    bytes[0] = std::byte{0x17};
    const char* script =
        "import mmap, sys\n"
        "with open(sys.argv[1], 'r+b', buffering=0) as f:\n"
        "    with mmap.mmap(f.fileno(), 16, access=mmap.ACCESS_WRITE) as view:\n"
        "        assert view[0] == 0x17\n"
        "        view[3] = 0x6c\n";
    const pid_t child = ::fork();
    ASSERT_NE(child, -1);
    if (child == 0)
    {
      ::execlp(ANGARIA_TEST_PYTHON, ANGARIA_TEST_PYTHON, "-c", script, _path.c_str(),
               static_cast<char*>(nullptr));
      ::_exit(127);
    }
    const int status = wait_for(child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(bytes[3], std::byte{0x6c});
    std::array<std::byte, 1> observed{};
    require_value(file.read_exact(3, observed));
    EXPECT_EQ(observed[0], std::byte{0x6c});
  }
} // namespace
