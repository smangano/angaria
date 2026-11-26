#pragma once

#include "journal_validation.h"
#include "native_file.h"

#include <span>
#include <vector>

namespace angaria::detail::mapped
{
  // Topology with no deliveries needs only Task cleanup. Active Queue rings
  // may exist, but their slots and all reserved delivery progress must be zero.
  OperationResult validate_task_only_topology(NativeFile& file, const FixedLayout& layout,
                                               std::span<const JournalRecordRange> records,
                                               ErrorCode errorCode = ErrorCode::IoError);

  // Expands ReleaseClaim without changing mapped storage. Core delivery cleanup
  // is explicitly admitted by the caller's already validated native profile.
  // The caller validates all returned targets before performing any redo write.
  Result<std::vector<RedoCommand>> expand_task_claim_commands(
      NativeFile& file, const FixedLayout& layout,
      std::span<const JournalRecordRange> records,
      std::span<const RedoCommand> commands, ErrorCode errorCode = ErrorCode::IoError,
      bool nativeDelivery = false);
} // namespace angaria::detail::mapped
