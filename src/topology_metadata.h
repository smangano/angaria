#pragma once

#include "angaria/descriptors.h"
#include "angaria/results.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace angaria::detail::mapped
{
  // Owning values for immutable format-2 topology definitions. Logical slices,
  // stable wire codes, and explicit integers replace native structure layouts.
  struct QueueDefinition
  {
    QueueId _id{};
    QueueDescriptor _descriptor;
    RegistrationMetadata _registration;
    EntityState _state{EntityState::Draft};
  };

  struct SourceDefinition
  {
    SourceQueueSetId _id{};
    SourceQueueSetDescriptor _descriptor;
    RegistrationMetadata _registration;
    EntityState _state{EntityState::Draft};
    std::vector<QueueId> _members;
  };

  struct PublishingDefinition
  {
    PublishingQueueSetId _id{};
    PublishingQueueSetDescriptor _descriptor;
    RegistrationMetadata _registration;
    EntityState _state{EntityState::Draft};
    std::uint64_t _topologyRevision{};
  };

  // Shape-only preflight accepts omitted overrides and incomplete draft owners.
  // Native policy support and registered references belong to the runtime.
  OperationResult validate_queue_descriptor(const QueueDescriptor& descriptor);
  OperationResult validate_source_set_descriptor(const SourceQueueSetDescriptor& descriptor);
  OperationResult validate_publishing_set_descriptor(const PublishingQueueSetDescriptor& descriptor);

  Result<std::vector<std::byte>> encode_queue_definition(const QueueDefinition& definition,
                                                         std::uint64_t recordOffset);
  Result<QueueDefinition> decode_queue_definition(std::span<const std::byte> immutableBody,
                                                   std::uint64_t recordOffset);
  Result<std::vector<std::byte>> encode_source_definition(const SourceDefinition& definition,
                                                          std::uint64_t recordOffset);
  Result<SourceDefinition> decode_source_definition(std::span<const std::byte> immutableBody,
                                                     std::uint64_t recordOffset);
  Result<std::vector<std::byte>> encode_publishing_definition(const PublishingDefinition& definition,
                                                              std::uint64_t recordOffset);
  Result<PublishingDefinition> decode_publishing_definition(std::span<const std::byte> immutableBody,
                                                             std::uint64_t recordOffset);
} // namespace angaria::detail::mapped
