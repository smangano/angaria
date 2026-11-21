// Private journal regression for large redo ranges. The synthetic ring record
// provides legal mutable storage without requiring Queue publication APIs; this is
// transaction-layer setup, not an example of sending an application message.

#include "mutation_journal.h"
#include "native_atomic.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <variant>

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;
  namespace fs = std::filesystem;

  template <typename T>
  T require_value(Result<T> result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    return std::get<T>(std::move(result));
  }

  void put_word(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value)
  {
    for (std::size_t index = 0; index != 8; ++index)
    {
      bytes[offset + index] = static_cast<std::byte>((value >> (8 * index)) & 0xff);
    }
  }

  // Fail once after an actual word store, then observe execute's repair pass.
  // Seeing index zero again distinguishes replay from simply continuing on.
  class MiddleRedoFailure : public mapped::JournalFaultInjector
  {
  public:
    MiddleRedoFailure(std::uint64_t middle, std::uint64_t last) : _middle(middle), _last(last)
    {
    }

    OperationResult checkpoint(mapped::JournalCheckpoint point, std::uint64_t index) override
    {
      if (point == mapped::JournalCheckpoint::RedoWord)
      {
        if (_failed && index == 0)
        {
          _restarted = true;
        }
        if (index == _last)
        {
          _sawFinalWord = true;
        }
        if (!_failed && index == _middle)
        {
          _failed = true;
          return Error{ErrorCode::IoError, "Injected midpoint failure in bulk redo", "test.bulk_redo"};
        }
      }
      return std::monostate{};
    }

    bool _failed{};
    bool _restarted{};
    bool _sawFinalWord{};

  private:
    std::uint64_t _middle;
    std::uint64_t _last;
  };

  class JournalBulkRedoTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-bulk-redo-XXXXXX").string();
      const auto directory = ::mkdtemp(pattern.data());
      ASSERT_NE(directory, nullptr);
      _root = directory;
      fs::create_directory(_root / "claims");
      _configuration._session = {0x123456789abcdef0, 0xfedcba9876543210};
      _configuration._liveAbiProfile = require_value(mapped::qualify_native_profile())._liveAbiProfile;
      _configuration._maxLogBytes = 4 * 1024 * 1024;
      _configuration._limits = CapacityLimits::empty().tasks(1);
      const auto layout = mapped::make_fixed_layout(_configuration._limits);
      ASSERT_TRUE(layout);
      auto file = require_value(mapped::NativeFile::create_exclusive(_root / "session.ang",
                                                                    _configuration._maxLogBytes));
      require_value(file.resize(layout->_arenaBegin));
      require_value(file.write_exact(0, require_value(mapped::encode_initial_superblock(_configuration))));
      require_value(mapped::NativeFile::create_exclusive(_root / "mutation.lock"));
    }

    void TearDown() override
    {
      std::error_code ignored;
      if (!_root.empty())
      {
        fs::remove_all(_root, ignored);
      }
    }

    fs::path _root;
    mapped::SuperblockConfiguration _configuration;
  };

  // Interrupt a committed 1 MiB write halfway through, then let execute repair it.
  // Replay must restart safely, preserve recorded command order for the overlap, and
  // number checkpoints across the entire command stream rather than restarting per command.
  TEST_F(JournalBulkRedoTest, MidrangeFailureReplaysLargeAndOverlappingCommandsWithContinuousCheckpoints)
  {
    constexpr std::size_t range_bytes = 1024 * 1024;
    constexpr std::size_t word_count = range_bytes / 8;
    constexpr std::uint64_t seed = 0x1020304000000000;
    const std::array<std::uint64_t, 2> replacement{0xabcdef0123456789, 0xfedcba9876543210};
    auto controller = require_value(mapped::SessionController::open_existing(_root));
    auto guard = require_value(controller->acquire_mutation());
    MiddleRedoFailure failure(word_count / 2, word_count + 1);
    auto journal = require_value(mapped::MutationJournal::open(controller, guard, &failure));
    mapped::TransactionPlan plan;
    plan._base = require_value(journal.snapshot(guard));
    plan._kind = mapped::JournalKind::Append;
    plan._newRevision = plan._base._revision + 1;
    mapped::JournalRecord ring;
    ring._kind = 8;
    ring._mutable.resize(range_bytes);
    plan._records.push_back(std::move(ring));
    mapped::RedoCommand range;
    range._target = plan._base._tail + mapped::record_envelope_bytes;
    range._payload.resize(range_bytes);
    for (std::size_t index = 0; index != word_count; ++index)
    {
      put_word(range._payload, index * 8, seed + index);
    }
    mapped::RedoCommand overlap;
    overlap._target = range._target + range_bytes - 16;
    overlap._payload.resize(16);
    put_word(overlap._payload, 0, replacement[0]);
    put_word(overlap._payload, 8, replacement[1]);
    plan._commands.push_back(std::move(range));
    plan._commands.push_back(std::move(overlap));

    const auto result = require_value(journal.execute(guard, plan));
    EXPECT_TRUE(failure._failed);
    EXPECT_TRUE(failure._restarted);
    EXPECT_TRUE(failure._sawFinalWord);
    EXPECT_EQ(result._generation, plan._base._generation + 1);
    auto& file = *require_value(controller->file(guard));
    auto region = require_value(file.map_read_write(plan._commands[0]._target, range_bytes));
    const auto bytes = require_value(region.writable_bytes());
    for (std::size_t index = 0; index != word_count; ++index)
    {
      const auto expected = index < word_count - 2 ? seed + index : replacement[index - (word_count - 2)];
      ASSERT_EQ(require_value(mapped::AtomicWord::bind(bytes, index * 8)).load_acquire(), expected) << index;
    }
    const auto settled = require_value(journal.snapshot(guard));
    EXPECT_EQ(settled._generation, result._generation);
    EXPECT_EQ(settled._fileBytes, result._tail);
    EXPECT_EQ(settled._accounted, result._tail);
  }
} // namespace
