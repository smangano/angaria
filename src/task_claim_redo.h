#pragma once

#include "journal_validation.h"
#include "native_file.h"

#include <span>
#include <vector>

namespace angaria::detail::mapped
{
  // Draft topology has no deliveries to release. Its reserved progress rows
  // must still be empty before Task-only cleanup can change a claim.
  OperationResult validate_task_only_topology(NativeFile& file, const FixedLayout& layout,
                                               ErrorCode errorCode = ErrorCode::IoError);

  // Expands the Task-only ReleaseClaim subset without changing mapped storage.
  // Delivery state is rejected until receipt and Source cleanup are installed.
  // The caller validates all returned targets before performing any redo write.
  Result<std::vector<RedoCommand>> expand_task_claim_commands(
      NativeFile& file, const FixedLayout& layout,
      std::span<const JournalRecordRange> records,
      std::span<const RedoCommand> commands, ErrorCode errorCode = ErrorCode::IoError);
} // namespace angaria::detail::mapped
