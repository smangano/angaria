#pragma once

#include "journal_validation.h"
#include "native_file.h"

#include <functional>
#include <span>

namespace angaria::detail::mapped
{
  bool has_loss_command(std::span<const RedoCommand> commands) noexcept;

  // Validate the complete two-command FixedState intent before any live write.
  // Replay admits only the interrupted old/new values of this exact operation.
  // Record ranges are validated and ordered by allocation offset.
  OperationResult validate_loss_commands(NativeFile& file, const FixedLayout& layout,
      std::span<const JournalRecordRange> records, std::span<const RedoCommand> commands,
      TaskId actor, std::uint64_t incarnation, bool allowLoss, bool replay,
      ErrorCode errorCode = ErrorCode::IoError);

  // Stream bounded metadata and one receipt at a time. No command list grows
  // with the skipped range. The callback runs after each native atomic store.
  OperationResult apply_loss_command(NativeFile& file, const FixedLayout& layout,
      std::span<const JournalRecordRange> records, const RedoCommand& command,
      const std::function<void()>& checkpoint);
} // namespace angaria::detail::mapped
