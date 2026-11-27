#pragma once

#include "mapped_payload.h"
#include "runtime_state.h"

#include <cstdint>

namespace angaria::detail
{
  struct QueueDelivery
  {
    std::uint64_t _publication{};
    std::uint64_t _link{};
    std::uint64_t _receipt{};
  };

  // Native direct delivery has one destination immediately after its fixed
  // Publication prefix. Resolve an exact retained link without scanning older
  // Publications. The metadata reader still checks the committed record
  // boundary/kind and complete bounded metadata before the identity checks.
  inline QueueDelivery runtime_queue_delivery_at_link(RuntimeAccess& access, QueueId queue,
                                                        std::uint64_t sequence, std::uint64_t link)
  {
    constexpr auto prefixBytes = mapped::record_envelope_bytes + mapped::publication_fixed_bytes;
    if (link < prefixBytes)
    {
      runtime_fail(ErrorCode::IoError, "queue.history", "Queue delivery link precedes its Publication prefix");
    }
    const auto reference = link - prefixBytes;
    const auto publication = runtime_publication_metadata(access, reference);
    if (publication._publishingSet != PublishingQueueSetId::Invalid ||
        publication._destinations.size() != 1 || publication._layout._destinationsOffset != link)
    {
      runtime_fail(ErrorCode::IoError, "queue.history", "Queue link does not identify a direct Publication destination");
    }
    const auto& destination = publication._destinations.front();
    if (destination._queue != queue || destination._queueSequence != sequence)
    {
      runtime_fail(ErrorCode::IoError, "queue.history", "Queue delivery link has a different Queue or sequence");
    }
    return {reference, link, destination._receiptReference};
  }

  // Resolve Queue order from retained immutable metadata, without copying an
  // application payload or depending on an overwriteable ring cache entry.
  inline QueueDelivery runtime_queue_delivery(RuntimeAccess& access, QueueId queue,
                                                std::uint64_t sequence)
  {
    for (const auto reference : access.record_references(9))
    {
      const auto publication = runtime_publication_metadata(access, reference);
      const auto& destination = publication._destinations.front();
      if (destination._queue == queue && destination._queueSequence == sequence)
      {
        return {reference, publication._layout._destinationsOffset, destination._receiptReference};
      }
    }
    runtime_fail(ErrorCode::IoError, "queue.history", "Queue progress has no retained delivery");
  }
} // namespace angaria::detail
