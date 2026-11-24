#pragma once

#include "angaria/descriptors.h"

#include <cstdint>

namespace angaria::detail
{
  struct QueuePolicy
  {
    std::uint64_t _capacity{};
    DeliveryPolicy _delivery{};
  };

  struct SourcePolicy
  {
    SelectionPolicy _selection{};
    WaitPolicy _wait{};
  };

  // Registration and file preflight share effective policy resolution and the
  // native support boundary. These functions throw the private RuntimeFailure.
  QueuePolicy resolve_queue(const QueueDescriptor& descriptor, const SessionOptions& options);
  SourcePolicy resolve_source(const SourceQueueSetDescriptor& descriptor, const SessionOptions& options);
  AcceptancePolicy resolve_publishing(const PublishingQueueSetDescriptor& descriptor,
                                        const SessionOptions& options);
} // namespace angaria::detail
