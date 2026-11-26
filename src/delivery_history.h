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
