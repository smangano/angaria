#pragma once

#include "journal_validation.h"
#include "native_file.h"

#include <span>
#include <vector>

namespace angaria::detail::mapped
{
  // Expands the Task-only ReleaseClaim subset without changing mapped storage.
  // Delivery state is rejected until receipt and Source cleanup are installed.
  // The caller validates all returned targets before performing any redo write.
  Result<std::vector<RedoCommand>> expand_task_claim_commands(
      NativeFile& file, const FixedLayout& layout,
      std::span<const JournalRecordRange> records,
      std::span<const RedoCommand> commands, ErrorCode errorCode = ErrorCode::IoError);
} // namespace angaria::detail::mapped
