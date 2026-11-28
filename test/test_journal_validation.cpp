// These private validation examples separate a well-formed command from permission
// to mutate its target. The allowlist protects immutable history, reserved bytes, and
// engine-owned roots; application code submits operations through Session/Task instead.

#include "result_helpers.h"
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

  using angaria::test_support::require_value;

  void expect_error(const OperationResult& result, ErrorCode code = ErrorCode::IoError)
  {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    const auto& error = std::get<Error>(result);
    EXPECT_EQ(error._code, code);
    EXPECT_EQ(error._path, "journal.targets");
    EXPECT_FALSE(error._message.empty());
  }

  TEST(JournalRecordLookup, ResolvesOnlyExactBoundariesAcrossTheOrderedRange)
  {
    // The compact replay reader must retain exact reference validation when
    // using binary search: lower_bound alone could return the next record.
    const std::array<mapped::JournalRecordRange, 3> records{{
        {._offset = 4096}, {._offset = 4352}, {._offset = 4864},
    }};
    EXPECT_EQ(mapped::find_journal_record({}, 4096), nullptr);
    for (const auto& record : records)
    {
      EXPECT_EQ(mapped::find_journal_record(records, record._offset), &record);
    }
    for (const auto reference : {0u, 4095u, 4097u, 4160u, 4353u, 4865u, 8192u})
    {
      EXPECT_EQ(mapped::find_journal_record(records, reference), nullptr);
    }
  }

  mapped::RedoCommand write_range(std::uint64_t target, std::size_t bytes = 8)
  {
    return mapped::RedoCommand{._target = target, ._payload = std::vector<std::byte>(bytes)};
  }

  class JournalValidationTest : public testing::Test
  {
  protected:
    mapped::JournalRecordRange administrative_definition() const
    {
      mapped::RecordEnvelope envelope;
      envelope._kind = 2;
      envelope._totalBytes = 192;
      envelope._immutableBytes = 128;
      envelope._generation = 1;
      envelope._readiness = 2;
      const std::array<std::byte, 128> body{};
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

  // Processing receipts belong to producer/connection files. Their former
  // Session-journal record kinds must never expose mutable receipt targets.
  TEST_F(JournalValidationTest, ProducerMessageAndRetryKindsAreRejectedBeforeExposingMutableRanges)
  {
    for (const auto kind : {9u, 10u})
    {
      mapped::RecordEnvelope envelope;
      envelope._kind = static_cast<std::uint16_t>(kind);
      envelope._totalBytes = 448;
      envelope._immutableBytes = 128;
      envelope._generation = 1;
      envelope._readiness = 2;
      const std::array<std::byte, 128> body{};
      const auto result = mapped::validate_journal_record(_layout._arenaBegin, envelope, body, _layout._arenaBegin);
      ASSERT_TRUE(std::holds_alternative<Error>(result));
      EXPECT_EQ(std::get<Error>(result)._code, ErrorCode::IoError);
      const auto preflight = mapped::validate_journal_record(_layout._arenaBegin, envelope, body,
          _layout._arenaBegin, ErrorCode::InvalidArgument);
      ASSERT_TRUE(std::holds_alternative<Error>(preflight));
      EXPECT_EQ(std::get<Error>(preflight)._code, ErrorCode::UnsupportedPolicy);
    }
  }

  // A valid first word or command cannot excuse an invalid later target.
  // Whole-stream validation is what lets replay reject malformed intent before any partial repair.
  TEST_F(JournalValidationTest, AReservedLaterWordRejectsTheCompleteRangeOrStream)
  {
    const std::array records{administrative_definition()};
    const auto task = section_offset(mapped::SectionKind::TaskState);
    const std::array crossing{write_range(task + 72, 16)};
    expect_error(mapped::validate_redo_targets(crossing, _layout, records));
    const std::array commands{write_range(task), write_range(task + 80)};
    const auto before = commands;
    expect_error(mapped::validate_redo_targets(commands, _layout, records, ErrorCode::InvalidArgument),
                 ErrorCode::InvalidArgument);
    EXPECT_EQ(commands, before);
  }

  // Generic redo must not rewrite accepted history or bypass the journal engine
  // that installs allocation, revision, manifest, and generation roots as one final sequence.
  TEST_F(JournalValidationTest, ImmutableRecordsAndOwnedControlWordsAreNeverRedoTargets)
  {
    const std::array records{administrative_definition()};
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
    for (const auto target : {std::uint64_t{72}, std::uint64_t{112}, std::uint64_t{184}})
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
