#pragma once

#include "angaria/descriptors.h"
#include "angaria/results.h"
#include "mapped_superblock.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace angaria::detail::mapped
{
  // Owning process-local values. Bodies use the explicit format-2 layout, not
  // these C++ objects. The stored directory is the original diagnostic path.
  struct SessionDefinition
  {
    SessionMetadata _metadata;
    SessionOptions _options;
    std::uint64_t _policyFeatures{};
  };

  struct TaskDefinition
  {
    TaskId _id{};
    TaskDescriptor _descriptor;
    RegistrationMetadata _registration;
  };

  Result<std::vector<std::byte>> encode_session_definition(const SessionDefinition& definition,
                                                           std::uint64_t recordOffset);
  Result<SessionDefinition> decode_session_definition(std::span<const std::byte> immutableBody,
                                                       std::uint64_t recordOffset,
                                                       const SuperblockConfiguration& configuration);
  Result<std::vector<std::byte>> encode_task_definition(const TaskDefinition& definition,
                                                        std::uint64_t recordOffset);
  // Shared programmatic/configuration preflight without serializing or copying
  // descriptor strings. Parent existence and actor ownership belong to runtime.
  OperationResult validate_task_descriptor(const TaskDescriptor& descriptor);
  Result<TaskDefinition> decode_task_definition(std::span<const std::byte> immutableBody,
                                                 std::uint64_t recordOffset);
} // namespace angaria::detail::mapped
