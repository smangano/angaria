#include "angaria/runtime.h"
#include "angaria/session.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <variant>

namespace
{
  using namespace angaria;

  static_assert(!std::is_copy_constructible_v<Task>);
  static_assert(std::is_move_constructible_v<Task>);
  static_assert(!std::is_copy_constructible_v<SourceQueueSet>);
  static_assert(!std::is_copy_constructible_v<PublishingQueueSet>);
  static_assert(!std::is_copy_constructible_v<QueueWriter>);
  static_assert(std::is_same_v<decltype(MessageView::_payload), std::span<const std::byte>>);
  static_assert(!std::is_aggregate_v<CapacityLimits>);
  static_assert(std::is_same_v<decltype(std::declval<CapacityLimits&>().tasks(1)), CapacityLimits&>);
  static_assert(std::is_same_v<decltype(CapacityLimits{}.tasks(1)), CapacityLimits>);

  TEST(ApiContract, DescriptorsExposeTheAgreedDefaults)
  {
    const PolicyDefaults defaults;
    const QueueDescriptor queue;
    const SourceQueueSetDescriptor source;
    const PublishingQueueSetDescriptor publisher;
    EXPECT_EQ(defaults._delivery, DeliveryPolicy::RecoverFromLog);
    EXPECT_FALSE(queue._delivery);
    EXPECT_FALSE(queue._ringCapacity);
    EXPECT_EQ(defaults._selection, SelectionPolicy::Strict);
    EXPECT_EQ(defaults._wait, WaitPolicy::Spin);
    EXPECT_FALSE(source._selection);
    EXPECT_FALSE(source._wait);
    EXPECT_FALSE(source._randomSeed);
    EXPECT_EQ(defaults._acceptance, AcceptancePolicy::AllDestinations);
    EXPECT_FALSE(publisher._acceptance);
  }

  TEST(ApiContract, CapacityOverridesPreserveDefaultsAndOwnTheirValues)
  {
    constexpr auto configured = CapacityLimits{}.tasks(8).queues(64).source_sets(4);
    static_assert(configured._tasks == 8);
    static_assert(configured._queues == 64);
    static_assert(configured._sourceSets == 4);

    const SessionOptions defaults;
    EXPECT_EQ(configured._publishingSets, defaults._limits._publishingSets);
    EXPECT_EQ(configured._fields, defaults._limits._fields);
    EXPECT_EQ(configured._stringPoolBytes, defaults._limits._stringPoolBytes);

    auto limits = configured;
    auto& updated = limits.tasks(2).queues(12);
    EXPECT_EQ(&updated, &limits);
    EXPECT_EQ(limits._tasks, 2u);
    EXPECT_EQ(limits._queues, 12u);
    EXPECT_EQ(configured._tasks, 8u);

    const auto& owned = CapacityLimits{}.tasks(3).queues(7);
    EXPECT_EQ(owned._tasks, 3u);
    EXPECT_EQ(owned._queues, 7u);

    const auto disabled = CapacityLimits{}.symbols(0).string_pool_bytes(0);
    EXPECT_EQ(disabled._symbols, 0u);
    EXPECT_EQ(disabled._stringPoolBytes, 0u);
    EXPECT_EQ(disabled._tasks, defaults._limits._tasks);
  }

  TEST(ApiContract, PartialPublicationPreservesPerDestinationReasons)
  {
    const PublicationId publication{{{1, 2}, TaskId{3}, 1}, 4};
    const std::array<DestinationOutcome, 2> outcomes{
        DestinationOutcome{QueueId{1}, DestinationStatus::Accepted, ErrorCode::None},
        DestinationOutcome{QueueId{2}, DestinationStatus::Rejected, ErrorCode::LogFull},
    };
    const PublicationReport report{publication, PublicationStatus::Partial, 5, outcomes, 2, ErrorCode::None};

    EXPECT_EQ(report._publication, publication);
    EXPECT_EQ(report._topologyRevision, 5u);
    ASSERT_EQ(report._destinations.size(), 2u);
    EXPECT_EQ(report._destinations[0]._status, DestinationStatus::Accepted);
    EXPECT_EQ(report._destinations[1]._queue, QueueId{2});
    EXPECT_EQ(report._destinations[1]._reason, ErrorCode::LogFull);
  }

  TEST(ApiContract, ReceiveEventsKeepGapsTimeoutsAndErrorsDistinct)
  {
    ReceiveResult result = DeliveryGap{QueueId{7}, 2, 9};
    ASSERT_TRUE(std::holds_alternative<DeliveryGap>(result));
    EXPECT_EQ(std::get<DeliveryGap>(result)._lastSequence, 9u);

    result = ReceiveTimeout{};
    EXPECT_TRUE(std::holds_alternative<ReceiveTimeout>(result));

    result = Error{ErrorCode::StaleDelivery, "stale delivery", ""};
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    EXPECT_EQ(std::get<Error>(result)._code, ErrorCode::StaleDelivery);
  }
} // namespace
