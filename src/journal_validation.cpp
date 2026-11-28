#include "journal_validation.h"

#include "angaria/detail/byte_operations.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace angaria::detail::mapped
{
  namespace
  {
    Error framing_error(ErrorCode code, std::string message)
    {
      return Error{code, std::move(message), "journal.framing"};
    }

    Error target_error(ErrorCode code, std::string message)
    {
      return Error{code, std::move(message), "journal.targets"};
    }

    ErrorCode unsupported_code(ErrorCode code)
    {
      return code == ErrorCode::InvalidArgument ? ErrorCode::UnsupportedPolicy : code;
    }

    struct WordAccess
    {
      bool _allowed{};
      bool _reservedUpperHalf{};
    };

    WordAccess fixed_word_access(const FixedLayout& layout, std::uint64_t target)
    {
      if (target == 72 || target == 112 || target == catalog_seal_offset)
      {
        return {true, false};
      }
      if (target >= layout._arenaBegin)
      {
        return {};
      }
      for (const auto& section : layout._sections)
      {
        if (section._stride == 0 || target < section._offset ||
            !contains_range(section._byteLength, target - section._offset, 8))
        {
          continue;
        }
        const auto rowOffset = (target - section._offset) % section._stride;
        switch (section._kind)
        {
        case SectionKind::TaskIndex:
        case SectionKind::QueueIndex:
        case SectionKind::SourceIndex:
        case SectionKind::PublishingIndex:
        case SectionKind::FieldIndex:
        case SectionKind::SchemaIndex:
          return {section._stride == index_entry_bytes && rowOffset < 32, false};
        case SectionKind::TaskState:
          return {section._stride == task_state_bytes && rowOffset < 80, false};
        case SectionKind::QueueState:
          return {section._stride == queue_state_bytes &&
                      (rowOffset < 56 || (rowOffset >= 64 && rowOffset < 96)), false};
        case SectionKind::SourceState:
          return {section._stride == source_state_bytes && rowOffset < 128, rowOffset == 16};
        case SectionKind::PublishingState:
          return {section._stride == publishing_state_bytes && rowOffset < 32, false};
        case SectionKind::MembershipState:
          return {section._stride == membership_bytes && rowOffset < 24, rowOffset == 0};
        case SectionKind::SymbolIndex:
        case SectionKind::StringPool:
          return {};
        }
        return {};
      }
      return {};
    }

    bool arena_word_allowed(std::span<const JournalRecordRange> records, std::uint64_t target)
    {
      for (const auto& record : records)
      {
        if (target >= record._mutableBegin &&
            contains_range(record._mutableBytes, target - record._mutableBegin, 8))
        {
          return record._envelope._kind == 8;
        }
      }
      return false;
    }
  } // namespace

  Result<JournalRecordRange> validate_journal_record(
      std::uint64_t offset, const RecordEnvelope& envelope,
      std::span<const std::byte> immutableBody, std::uint64_t arenaBegin,
      ErrorCode errorCode)
  {
    if (offset < arenaBegin || offset % section_alignment != 0 ||
        envelope._totalBytes < record_envelope_bytes || envelope._totalBytes % section_alignment != 0 ||
        !contains_range(max_log_bytes, offset, envelope._totalBytes))
    {
      return framing_error(errorCode, "Record offset or allocation is outside aligned format bounds");
    }
    if (envelope._encoding != 1 || envelope._generation == 0 || envelope._readiness > 2 ||
        envelope._immutableBytes % record_alignment != 0 ||
        envelope._immutableBytes > envelope._totalBytes - record_envelope_bytes)
    {
      return framing_error(errorCode, "Invalid record encoding, generation, readiness, or immutable length");
    }
    if (envelope._previous != 0 &&
        (envelope._previous < arenaBegin || envelope._previous % section_alignment != 0 ||
         envelope._previous >= offset))
    {
      return framing_error(errorCode, "Record predecessor is not an earlier aligned arena reference");
    }

    std::uint64_t fixedBytes{};
    switch (envelope._kind)
    {
    case 1:
      fixedBytes = 144;
      break;
    case 2:
      fixedBytes = 72;
      break;
    case 3:
      fixedBytes = 80;
      break;
    case 4:
    case 5:
      fixedBytes = 96;
      break;
    case 6:
    case 7:
      fixedBytes = 88;
      break;
    case 8:
      break;
    case 9:
    case 10:
      return framing_error(unsupported_code(errorCode), "Format 3 messages belong to producer-owned logs");
    case 11:
      fixedBytes = 64;
      break;
    case 12:
      return framing_error(unsupported_code(errorCode), "Symbol value record encoding is not implemented");
    default:
      return framing_error(errorCode, "Unknown journal record kind");
    }
    if (envelope._immutableBytes < fixedBytes || immutableBody.size() < fixedBytes)
    {
      return framing_error(errorCode, "Record omits its fixed immutable body");
    }

    JournalRecordRange result{offset, envelope};
    const auto suffixBytes = envelope._totalBytes - record_envelope_bytes - envelope._immutableBytes;
    if (envelope._kind == 8)
    {
      if (envelope._immutableBytes != 0 || suffixBytes == 0 || suffixBytes % ring_slot_bytes != 0)
      {
        return framing_error(errorCode, "Ring storage requires an aligned nonempty mutable slot array");
      }
      result._mutableBegin = offset + record_envelope_bytes;
      result._mutableBytes = suffixBytes;
    }
    else if (suffixBytes != 0)
    {
      return framing_error(errorCode, "This record kind cannot contain a mutable suffix");
    }
    return result;
  }

  OperationResult validate_redo_targets(
      std::span<const RedoCommand> commands, const FixedLayout& layout,
      std::span<const JournalRecordRange> records, ErrorCode errorCode)
  {
    for (const auto& command : commands)
    {
      if (command._opcode != RedoOpcode::WriteRange)
      {
        switch (command._opcode)
        {
        case RedoOpcode::ResetBudgets:
        case RedoOpcode::LoseRange:
        case RedoOpcode::ReleaseClaim:
          return target_error(unsupported_code(errorCode), "Compact domain redo is not implemented");
        default:
          return target_error(errorCode, "Unknown redo opcode");
        }
      }
      if (command._target % record_alignment != 0 || command._payload.empty() ||
          command._payload.size() % record_alignment != 0 ||
          !contains_range(max_log_bytes, command._target, command._payload.size()))
      {
        return target_error(errorCode, "WriteRange requires a complete aligned nonempty word range");
      }
      for (std::size_t index = 0; index < command._payload.size(); index += 8)
      {
        const auto target = command._target + index;
        const auto access = fixed_word_access(layout, target);
        if (!access._allowed &&
            (target < layout._arenaBegin || !arena_word_allowed(records, target)))
        {
          return target_error(errorCode, "WriteRange target is not a declared mutable word");
        }
        if (access._reservedUpperHalf && (read_unsigned_le<std::uint64_t>(command._payload, index) >> 32) != 0)
        {
          return target_error(errorCode, "WriteRange sets reserved bits in a packed state word");
        }
      }
    }
    return std::monostate{};
  }
} // namespace angaria::detail::mapped
