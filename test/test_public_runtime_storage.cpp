// Public setup/sealing examples, followed by explicitly private byte and fault
// probes. The temporary-directory helper owns only test files; applications use
// Session and Registry rather than the storage helpers exercised at the end.
#include "angaria/runtime.h"
#include "native_fixture.h"
#include "producer_log_fixture.h"
#include "public_runtime_storage.h"
#include "result_assertions.h"
#include "wire_fixture.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace runtime = angaria::detail;
  namespace mapped = runtime::mapped;
  namespace fs = std::filesystem;
  using angaria::test_support::expect_error;
  using angaria::test_support::require_value;

  class PublicRuntimeStorageTest : public testing::Test
  {
  protected:
    SessionOptions options(const std::string& name = "session") const
    {
      SessionOptions result;
      result._name = "Public admission storage";
      result._directory = _directory.path() / name;
      result._maxLogBytes = 1048576;
      result._producerLogBytes = 1048576;
      result._producerLogPublications = 64;
      result._defaults._ringCapacity = 8;
      result._limits = CapacityLimits{}.tasks(4).queues(4).source_sets(4).publishing_sets(4)
                          .fields(4).message_schemas(4).symbols(0).string_pool_bytes(0);
      return result;
    }

    std::uint64_t seal(const fs::path& directory) const
    {
      auto file = require_value(mapped::NativeFile::open_existing(directory / "session.ang", mapped::FileAccess::ReadOnly));
      std::array<std::byte, 8> bytes{};
      require_value(file.read_exact(184, bytes));
      return angaria::test_support::get_integer(bytes, 0, 8);
    }

    angaria::test_support::ProducerLogDirectory _directory;
  };

  TEST_F(PublicRuntimeStorageTest, FirstPublicationIdSealsCompletedSetupAndReplacementKeepsTheSeal)
  {
    const auto configured = options();
    std::uint64_t revision{};
    std::uint64_t firstIncarnation{};
    {
      auto created = require_value(Session::create(configured, TaskDescriptor{"Setup"}));
      auto registry = created._creator.registry();
      require_value(registry.register_task(TaskDescriptor{"Consumer"}));
      revision = require_value(registry.snapshot())._revision;
      EXPECT_EQ(seal(configured._directory), 0u);

      // IDs can be issued by a Task without outgoing Queues. This still marks
      // the transition from mutable setup to an immutable messaging catalog.
      const auto first = require_value(created._creator.next_publication_id());
      firstIncarnation = first._producer._incarnation;
      EXPECT_EQ(first._sequence, 1u);
      EXPECT_EQ(seal(configured._directory), revision);
      EXPECT_EQ(require_value(created._creator.next_publication_id())._sequence, 2u);
      EXPECT_EQ(require_value(registry.snapshot())._revision, revision);
      expect_error(registry.register_task(TaskDescriptor{"TooLate"}), ErrorCode::InvalidState);
      EXPECT_TRUE(fs::is_regular_file(configured._directory / "producer-1.messages.ang"));
      EXPECT_TRUE(fs::is_regular_file(configured._directory / "producer-2.messages.ang"));
      EXPECT_EQ(fs::file_size(configured._directory / "producer-1.messages.ang"), 4288u);
      EXPECT_EQ(fs::file_size(configured._directory / "producer-2.messages.ang"), 4288u);
    }
    auto attached = require_value(Session::attach(configured._directory));
    auto replacement = require_value(attached.attach_task("Setup"));
    const auto next = require_value(replacement.next_publication_id());
    EXPECT_GT(next._producer._incarnation, firstIncarnation);
    EXPECT_EQ(next._sequence, 1u);
    EXPECT_EQ(seal(configured._directory), revision);
  }

  TEST_F(PublicRuntimeStorageTest, UnpublishedRegularResourcesAreRebuiltButSymlinksAreNeverFollowed)
  {
    const auto configured = options();
    auto created = require_value(Session::create(configured, TaskDescriptor{"Setup"}));
    const auto outside = _directory.path() / "outside.txt";
    {
      std::ofstream output(outside);
      output << "preserve me";
    }
    const auto log = configured._directory / "producer-1.messages.ang";
    fs::create_symlink(outside, log);
    expect_error(created._creator.next_publication_id(), ErrorCode::IoError);
    EXPECT_TRUE(fs::is_symlink(log));
    EXPECT_EQ(seal(configured._directory), 0u);
    {
      std::ifstream input(outside);
      std::string text;
      std::getline(input, text);
      EXPECT_EQ(text, "preserve me");
    }
    fs::remove(log);
    {
      // A prior interrupted, unsealed bootstrap may leave a short regular file.
      // No messaging endpoint escaped, so retry can replace that file safely.
      std::ofstream incomplete(log);
      incomplete << "partial bootstrap";
    }
    EXPECT_EQ(require_value(created._creator.next_publication_id())._sequence, 1u);
    EXPECT_EQ(fs::file_size(log), 4288u);
    EXPECT_NE(seal(configured._directory), 0u);
  }

  TEST_F(PublicRuntimeStorageTest, PreviousFormatIsRejectedBeforeAnySessionByteIsWritten)
  {
    const auto configured = options();
    {
      auto created = require_value(Session::create(configured, TaskDescriptor{"Setup"}));
      (void)created;
    }
    const auto path = configured._directory / "session.ang";
    {
      // Private corruption fixture: downgrade only the declared major version.
      // Attaching must not acquire a mutable runtime and reinterpret the bytes.
      std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
      output.seekp(8);
      const std::array<char, 2> oldMajor{2, 0};
      output.write(oldMajor.data(), oldMajor.size());
    }
    const auto bytes = [&]()
    {
      std::ifstream input(path, std::ios::binary);
      return std::vector<char>{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    };
    const auto before = bytes();
    expect_error(Session::attach(configured._directory), ErrorCode::IoError);
    EXPECT_EQ(bytes(), before);
  }

  class SealFailure : public mapped::JournalFaultInjector
  {
  public:
    explicit SealFailure(mapped::JournalCheckpoint point) : _point(point)
    {
    }

    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t) override
    {
      if (!_fired && point == _point)
      {
        _fired = true;
        return Error{ErrorCode::IoError, "Injected seal interruption", "test.seal"};
      }
      return std::monostate{};
    }

  private:
    mapped::JournalCheckpoint _point;
    bool _fired{};
  };

  TEST_F(PublicRuntimeStorageTest, AdministrativeRecoverySeparatesUnpublishedFilesFromCommittedSeal)
  {
    // These are private in-process journal fault probes, not process-kill tests.
    // A prepared seal can roll back and rebuild; a committed seal must retain
    // resource identity and complete its metadata installation on recovery.
    for (const auto committed : {false, true})
    {
      const auto configured = options(committed ? "committed" : "prepared");
      auto created = require_value(Session::create(configured, TaskDescriptor{"Setup"}));
      const auto state = angaria::test_support::open_runtime_state(configured._directory);
      SealFailure failure(committed ? mapped::JournalCheckpoint::Committed : mapped::JournalCheckpoint::Prepared);
      {
        runtime::RuntimeAccess access(state, true, &failure);
        if (committed)
        {
          EXPECT_NO_THROW((void)runtime::runtime_recorded_resources(access, created._creator.claim()));
        }
        else
        {
          EXPECT_THROW((void)runtime::runtime_recorded_resources(access, created._creator.claim()), runtime::RuntimeFailure);
        }
      }
      EXPECT_EQ(seal(configured._directory) != 0, committed);
      const auto logPath = configured._directory / "producer-1.messages.ang";
      auto file = require_value(mapped::NativeFile::open_existing(logPath, mapped::FileAccess::ReadOnly));
      const auto identity = require_value(file.identity());
      EXPECT_EQ(require_value(created._creator.next_publication_id())._sequence, 1u);
      auto after = require_value(mapped::NativeFile::open_existing(logPath, mapped::FileAccess::ReadOnly));
      if (committed)
      {
        EXPECT_EQ(require_value(after.identity()), identity);
      }
      EXPECT_NE(seal(configured._directory), 0u);
    }
  }
} // namespace
