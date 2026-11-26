#include "topology_policy.h"

#include "runtime_state.h"

namespace angaria::detail
{
  QueuePolicy resolve_queue(const QueueDescriptor& descriptor, const SessionOptions& options,
                              bool allowLoss)
  {
    const auto capacity = descriptor._ringCapacity.value_or(options._defaults._ringCapacity);
    const auto delivery = descriptor._delivery.value_or(options._defaults._delivery);
    if (!mapped::ring_storage_bytes(capacity))
    {
      runtime_fail(ErrorCode::InvalidArgument, "queue.ring_capacity", "Queue ring geometry exceeds format bounds");
    }
    if (delivery != DeliveryPolicy::RecoverFromLog &&
        (delivery != DeliveryPolicy::AllowLoss || !allowLoss))
    {
      runtime_fail(ErrorCode::UnsupportedPolicy, "queue.delivery", "Queue delivery policy is not implemented by this native profile");
    }
    return QueuePolicy{capacity, delivery};
  }

  SourcePolicy resolve_source(const SourceQueueSetDescriptor& descriptor, const SessionOptions& options)
  {
    const auto selection = descriptor._selection.value_or(options._defaults._selection);
    const auto wait = descriptor._wait.value_or(options._defaults._wait);
    if (selection != SelectionPolicy::WeightedRandom && descriptor._randomSeed)
    {
      runtime_fail(ErrorCode::InvalidArgument, "source_set.random_seed", "A random seed requires weighted-random selection");
    }
    if (selection != SelectionPolicy::Strict || wait != WaitPolicy::Spin)
    {
      runtime_fail(ErrorCode::UnsupportedPolicy, "source_set.policy", "Source policy is not implemented by this native profile");
    }
    return SourcePolicy{selection, wait};
  }

  AcceptancePolicy resolve_publishing(const PublishingQueueSetDescriptor& descriptor, const SessionOptions& options)
  {
    const auto acceptance = descriptor._acceptance.value_or(options._defaults._acceptance);
    if (acceptance != AcceptancePolicy::AllDestinations)
    {
      runtime_fail(ErrorCode::UnsupportedPolicy, "publishing_set.acceptance", "Publishing acceptance policy is not implemented by this native profile");
    }
    return acceptance;
  }
} // namespace angaria::detail
