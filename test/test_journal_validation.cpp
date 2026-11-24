// These private validation examples separate a well-formed command from permission
// to mutate its target. The allowlist protects immutable history, reserved bytes, and
// engine-owned roots; application code submits operations through Session/Task instead.

#include "journal_validation.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace
{
  using namespace angaria;
  namespace mapped = angaria::detail::mapped;

  template <typename T>
  T require_value(Result<T> result)
  {
    if (const auto* error = std::get_if<Error>(&result))
    {
      throw std::runtime_error(error->_path + ": " + error->_message);
    }
    return std::get<T>(std::move(result));
  }

  void expect_error(const OperationResult& result, ErrorCode code = ErrorCode::IoError)
  {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    const auto& error = std::get<Error>(result);
    EXPECT_EQ(error._code, code);
    EXPECT_EQ(error._path, "journal.targets");
    EXPECT_FALSE(error._message.empty());
  }

  void put_word(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value)
  {
    for (unsigned index = 0; index < 8; ++index)
    {
      bytes[offset + index] = static_cast<std::byte>((value >> (8 * index)) & 0xff);
    }
  }

  mapped::RedoCommand write_range(std::uint64_t target, std::size_t bytes = 8)
  {
    return mapped::RedoCommand{._target = target, ._payload = std::vector<std::byte>(bytes)};
  }

  class JournalValidationTest : public testing::Test
  {
  protected:
    mapped::JournalRecordRange publication() const
    {
      // This local fixture describes only the receipt geometry used by the
      // target validator; registered message semantics are a separate layer.
      mapped::RecordEnvelope envelope;
      envelope._kind = 9;
      envelope._totalBytes = 448;
      envelope._immutableBytes = 128;
      envelope._generation = 1;
      envelope._readiness = 2;
      std::array<std::byte, 128> body{};
      put_word(body, 96, _layout._arenaBegin + 192);
      put_word(body, 104, 2);
      return require_value(mapped::validate_journal_record(_layout._arenaBegin, envelope, body,
                                                           _layout._arenaBegin));
    }

    std::uint64_t section_offset(mapped::SectionKind kind) const
    {
      for (const auto& section : _layout._sections)
      {
        if (section._kind == kind)
        {
          return section._offset;
        }
      }
      throw std::runtime_error("missing test section");
    }

    mapped::FixedLayout _layout = *mapped::make_fixed_layout(CapacityLimits::empty()
                                                                .tasks(1)
                                                                .queues(1)
                                                                .source_sets(1)
                                                                .publishing_sets(1)
                                                                .memberships_per_set(1));
  };

  // Each retained destination reserves mutable receipt state for delivery progress.
  // Only its seven declared words are generic redo targets; recovery space remains protected.
  TEST_F(JournalValidationTest, PublicationReceiptsExposeOnlyTheirSevenStateWords)
  {
    const std::array records{publication()};
    for (const auto receipt : {std::uint64_t{0}, std::uint64_t{128}})
    {
      const auto begin = records[0]._mutableBegin + receipt;
      for (std::uint64_t offset = 0; offset < 56; offset += 8)
      {
        const std::array commands{write_range(begin + offset)};
        require_value(mapped::validate_redo_targets(commands, _layout, records));
      }
      for (const auto offset : {std::uint64_t{56}, std::uint64_t{64}, std::uint64_t{120}})
      {
        const std::array commands{write_range(begin + offset)};
        expect_error(mapped::validate_redo_targets(commands, _layout, records));
      }
    }
  }

  // A valid first word or command cannot excuse an invalid later target.
  // Whole-stream validation is what lets replay reject malformed intent before any partial repair.
  TEST_F(JournalValidationTest, AReservedLaterWordRejectsTheCompleteRangeOrStream)
  {
    const std::array records{publication()};
    const auto receipt = records[0]._mutableBegin;
    const std::array crossing{write_range(receipt + 48, 16)};
    expect_error(mapped::validate_redo_targets(crossing, _layout, records));
    const std::array commands{write_range(receipt), write_range(receipt + 128 + 120)};
    const auto before = commands;
    expect_error(mapped::validate_redo_targets(commands, _layout, records, ErrorCode::InvalidArgument),
                 ErrorCode::InvalidArgument);
    EXPECT_EQ(commands, before);
  }

  // Generic redo must not rewrite accepted history or bypass the journal engine
  // that installs allocation, revision, manifest, and generation roots as one final sequence.
  TEST_F(JournalValidationTest, ImmutableRecordsAndOwnedControlWordsAreNeverRedoTargets)
  {
    const std::array records{publication()};
    const auto record = records[0]._offset;
    const std::array<std::uint64_t, 11> targets{
        record, record + 48, record + 56, record + 64, record + 184,
        80, 88, 96, 120, 176, mapped::journal_offset,
    };
    for (const auto target : targets)
    {
      const std::array commands{write_range(target)};
      expect_error(mapped::validate_redo_targets(commands, _layout, records));
    }
    for (const auto target : {std::uint64_t{72}, std::uint64_t{112}})
    {
      const std::array commands{write_range(target)};
      require_value(mapped::validate_redo_targets(commands, _layout, records));
    }
  }

  // Some atomic words combine narrow fields with reserved bits. A full-word write
  // is allowed only when those reserved bits stay zero, including at the end of a writable row.
  TEST_F(JournalValidationTest, PackedFixedStateWordsPreserveReservedUpperBits)
  {
    const std::array<std::uint64_t, 2> targets{
        section_offset(mapped::SectionKind::SourceState) + 16,
        section_offset(mapped::SectionKind::MembershipState),
    };
    for (const auto target : targets)
    {
      std::array commands{write_range(target)};
      commands[0]._payload[3] = std::byte{0xff};
      require_value(mapped::validate_redo_targets(commands, _layout, {}));
      commands[0]._payload[4] = std::byte{1};
      expect_error(mapped::validate_redo_targets(commands, _layout, {}));
    }
    const auto task = section_offset(mapped::SectionKind::TaskState);
    const std::array crossing{write_range(task + 72, 16)};
    expect_error(mapped::validate_redo_targets(crossing, _layout, {}));
  }

  // Ring slots are replaceable indexes into retained history, while their allocation
  // envelope stays immutable. A range must fit the slot area without crossing either boundary.
  TEST_F(JournalValidationTest, RingSlotsRemainWritableWithoutExposingTheirEnvelope)
  {
    mapped::RecordEnvelope envelope;
    envelope._kind = 8;
    envelope._totalBytes = 128;
    envelope._generation = 1;
    envelope._readiness = 2;
    const std::array records{require_value(mapped::validate_journal_record(
        _layout._arenaBegin, envelope, {}, _layout._arenaBegin))};
    const std::array slots{write_range(records[0]._mutableBegin, 64)};
    require_value(mapped::validate_redo_targets(slots, _layout, records));
    for (const auto offset : {std::uint64_t{48}, std::uint64_t{56}, std::uint64_t{128}})
    {
      const std::array commands{write_range(records[0]._offset + offset)};
      expect_error(mapped::validate_redo_targets(commands, _layout, records));
    }
  }
} // namespace
