#pragma once

#include "journal_format.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace angaria::detail::mapped
{
  // Validated local framing and the only arena bytes eligible for generic redo.
  // Offsets are absolute logical offsets; these values never alias mapped data.
  struct JournalRecordRange
  {
    std::uint64_t _offset{};
    RecordEnvelope _envelope;
    std::uint64_t _mutableBegin{};
    std::uint64_t _mutableBytes{};
  };

  // The caller validates the full record against its owning file. This local
  // body copy need contain only the fixed kind-specific prefix (at most 128
  // bytes); variable immutable bytes are not interpreted by this layer.
  Result<JournalRecordRange> validate_journal_record(
      std::uint64_t offset, const RecordEnvelope& envelope,
      std::span<const std::byte> immutableBody, std::uint64_t arenaBegin,
      ErrorCode errorCode = ErrorCode::IoError);

  // Validate the complete stream before any redo write. The supplied layout
  // and record ranges must already describe the validated session geometry.
  OperationResult validate_redo_targets(
      std::span<const RedoCommand> commands, const FixedLayout& layout,
      std::span<const JournalRecordRange> records,
      ErrorCode errorCode = ErrorCode::IoError);
} // namespace angaria::detail::mapped
