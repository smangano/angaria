// Private native admission fixtures, not application Session creation. A real,
// recoverable FixedState journal makes premature recovery observable: Prepared
// would clear intent, and Committed would apply its word before clearing intent.
// Unsupported immutable profiles must reject before either action can happen.

#include "angaria/runtime.h"
#include "mutation_journal.h"
#include "native_atomic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

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

  void put_integer(std::span<std::byte> bytes, std::size_t offset,
                   std::uint64_t value, std::size_t width = 8)
  {
    for (std::size_t index = 0; index < width; ++index)
    {
      bytes[offset + index] = static_cast<std::byte>((value >> (8 * index)) & 0xff);
    }
  }

  std::uint64_t get_word(std::span<const std::byte> bytes, std::size_t offset)
  {
    std::uint64_t value{};
    for (std::size_t index = 0; index < 8; ++index)
    {
      value |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + index])} << (8 * index);
    }
    return value;
  }

  std::vector<std::byte> read_file(const fs::path& directory)
  {
    auto file = require_value(mapped::NativeFile::open_existing(directory / "session.ang",
                                                              mapped::FileAccess::ReadOnly));
    std::vector<std::byte> bytes(static_cast<std::size_t>(require_value(file.size())));
    require_value(file.read_exact(0, bytes));
    return bytes;
  }

  class DeliveryCompatibilityTest : public testing::Test
  {
  protected:
    void SetUp() override
    {
      auto pattern = (fs::temp_directory_path() / "angaria-delivery-admission-XXXXXX").string();
      const auto directory = ::mkdtemp(pattern.data());
      ASSERT_NE(directory, nullptr);
      _root = directory;
    }

    void TearDown() override
    {
      std::error_code ignored;
      if (!_root.empty())
      {
        fs::remove_all(_root, ignored);
      }
    }

    fs::path make_fixture(const std::string& name, mapped::JournalControl control)
    {
      const auto directory = _root / name;
      fs::create_directories(directory / "claims");
      mapped::SuperblockConfiguration configuration;
      configuration._session = {0x1234, 0x5678};
      configuration._formatMinor = 2;
      configuration._requiredFeatures = 7;
      configuration._liveAbiProfile = require_value(mapped::qualify_native_profile())._liveAbiProfile;
      configuration._maxLogBytes = 1048576;
      configuration._limits = CapacityLimits::empty().tasks(1);
      const auto layout = *mapped::make_fixed_layout(configuration._limits);
      _taskState = layout._sections[6]._offset;
      auto prefix = require_value(mapped::encode_initial_superblock(configuration));

      mapped::RedoCommand command;
      command._target = _taskState;
      command._payload.resize(8);
      put_integer(command._payload, 0, 123);
      const std::array commands{command};
      const auto encoded = require_value(mapped::encode_commands(commands));
      mapped::JournalHeader header;
      header._control = control;
      header._generation = 1;
      header._kind = mapped::JournalKind::FixedState;
      header._baseTail = layout._arenaBegin;
      header._newTail = layout._arenaBegin;
      header._baseAccounted = layout._arenaBegin;
      header._newAccounted = layout._arenaBegin;
      header._inlineBytes = static_cast<std::uint32_t>(encoded.size());
      header._originalFileBytes = layout._arenaBegin;
      header._newFileBytes = layout._arenaBegin;
      const auto encodedHeader = require_value(mapped::encode_journal_header(header));
      std::copy(encodedHeader.begin(), encodedHeader.end(), prefix.begin() + mapped::journal_offset);
      std::copy(encoded.begin(), encoded.end(),
                prefix.begin() + mapped::journal_offset + mapped::journal_header_bytes);
      auto file = require_value(mapped::NativeFile::create_exclusive(directory / "session.ang",
                                                                    configuration._maxLogBytes));
      require_value(file.resize(layout._arenaBegin));
      require_value(file.write_exact(0, prefix));
      require_value(mapped::NativeFile::create_exclusive(directory / "mutation.lock"));
      return directory;
    }

    fs::path _root;
    std::uint64_t _taskState{};
  };

  TEST_F(DeliveryCompatibilityTest, SupportedCoreProfileCanRecoverThePrivateJournalFixture)
  {
    // Positive control: these intents really are recoverable. The domain-level
    // Session bootstrap is deliberately absent; only private journal mechanics
    // are exercised here, not public Task attachment or delivery transitions.
    for (const auto control : {mapped::JournalControl::Prepared, mapped::JournalControl::Committed})
    {
      const auto directory = make_fixture(std::to_string(static_cast<unsigned>(control)), control);
      const auto before = read_file(directory);
      {
        auto controller = require_value(mapped::SessionController::open_existing(directory));
        auto guard = require_value(controller->acquire_mutation());
        auto journal = require_value(mapped::MutationJournal::open(controller, guard));
        const auto snapshot = require_value(journal.snapshot(guard));
        EXPECT_EQ(snapshot._generation, control == mapped::JournalControl::Committed ? 1u : 0u);
      }
      const auto after = read_file(directory);
      EXPECT_NE(after, before);
      EXPECT_EQ(get_word(after, mapped::journal_offset), 0u);
      EXPECT_EQ(get_word(after, static_cast<std::size_t>(_taskState)),
                control == mapped::JournalControl::Committed ? 123u : 0u);
    }
  }

  TEST_F(DeliveryCompatibilityTest, UnsupportedProfilesRejectBeforePreparedOrCommittedRecovery)
  {
    // The public attachment boundary must reject unsupported immutable metadata
    // before it clears Prepared or applies Committed, even with valid redo work
    // waiting. Raw profile changes below construct private incompatible fixtures;
    // applications never promote a stored capability word in place.
    for (const auto control : {mapped::JournalControl::Prepared, mapped::JournalControl::Committed})
    {
      for (unsigned variant = 0; variant < 6; ++variant)
      {
        SCOPED_TRACE(static_cast<unsigned>(control));
        SCOPED_TRACE(variant);
        const auto directory = make_fixture(std::to_string(static_cast<unsigned>(control)) + "-" +
                                            std::to_string(variant), control);
        auto before = read_file(directory);
        const char* path = "superblock.required_features";
        switch (variant)
        {
        case 0:
          put_integer(before, 56, 15); // Known, separately gated allowed loss.
          break;
        case 1:
          put_integer(before, 56, (std::uint64_t{1} << 63) | 7);
          break;
        case 2:
          put_integer(before, 10, 1, 2); // Core bit cannot reinterpret 2.1.
          break;
        case 3:
          put_integer(before, 10, 3, 2);
          path = "superblock.format";
          break;
        case 4:
          put_integer(before, 52, require_value(mapped::qualify_native_profile())._liveAbiProfile ^ 1u, 4);
          path = "superblock.live_abi_profile";
          break;
        case 5:
          put_integer(before, 50, 2, 2);
          path = "superblock.synchronization_version";
          break;
        }
        {
          auto file = require_value(mapped::NativeFile::open_existing(directory / "session.ang",
                                                                    mapped::FileAccess::ReadWrite));
          require_value(file.write_exact(0, before));
        }
        const auto attached = Session::attach(directory);
        ASSERT_TRUE(std::holds_alternative<Error>(attached));
        const auto& error = std::get<Error>(attached);
        EXPECT_EQ(error._code, ErrorCode::IoError);
        EXPECT_EQ(error._path, path);
        EXPECT_EQ(read_file(directory), before);
        EXPECT_TRUE(fs::is_empty(directory / "claims"));
        EXPECT_EQ(fs::file_size(directory / "mutation.lock"), 0u);
      }
    }
  }
} // namespace
