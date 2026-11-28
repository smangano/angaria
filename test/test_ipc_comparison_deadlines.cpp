// Private benchmark helper contracts, not public Angaria usage examples.
// These tests exercise the exception-type boundary used by the adapter's
// measured-phase handlers. They do not inject failures into a running Worker
// or claim to test its complete report/coordinator lifecycle.
#include "ipc_comparison_support.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

namespace
{
  namespace ipc = angaria::benchmarks::ipc;
  using angaria::benchmarks::BenchmarkFailure;

  ipc::Options parse_options(std::initializer_list<const char*> extra)
  {
    std::vector<std::string> arguments{"ipc_comparison", "--profile", "nanomsg", "--role", "producer",
                                       "--directory", "/unused-qualification-fixture"};
    for (const auto* argument : extra)
    {
      arguments.emplace_back(argument);
    }
    std::vector<char*> pointers;
    for (auto& argument : arguments)
    {
      pointers.push_back(argument.data());
    }
    return ipc::parse_options(static_cast<int>(pointers.size()), pointers.data());
  }

  TEST(IpcComparisonQualification, ExtendedStreamsRequireExplicitConcurrentQualification)
  {
    // Larger runs must not silently change the old bounded comparison. Warmup
    // belongs to the same attachment and must leave a nonempty measured slice.
    EXPECT_THROW((void)parse_options({"--messages", "129"}), BenchmarkFailure);
    EXPECT_THROW((void)parse_options({"--history", "129"}), BenchmarkFailure);
    const auto options = parse_options({"--qualification", "1", "--messages", "262144",
        "--history", "65536", "--warmup-messages", "8192", "--pacing-spin", "1"});
    EXPECT_TRUE(options._qualification);
    EXPECT_TRUE(options._pacingSpin);
    EXPECT_EQ(options._messages, 262144u);
    EXPECT_EQ(options._history, 65536u);
    EXPECT_EQ(options._warmupMessages, 8192u);
    EXPECT_THROW((void)parse_options({"--qualification", "1", "--messages", "262145"}), BenchmarkFailure);
    EXPECT_THROW((void)parse_options({"--qualification", "1", "--history", "65537"}), BenchmarkFailure);
    EXPECT_THROW((void)parse_options({"--qualification", "1", "--warmup-messages", "16"}), BenchmarkFailure);
    EXPECT_THROW((void)parse_options({"--qualification", "1", "--scenario", "backlog"}), BenchmarkFailure);
    EXPECT_THROW((void)parse_options({"--qualification", "true"}), BenchmarkFailure);
  }

  TEST(IpcComparisonQualification, ConsumerWorkAndPauseHaveExplicitMeasuredBounds)
  {
    const auto options = parse_options({"--qualification", "1", "--warmup-messages", "4",
        "--processing-delay-ns", "1000", "--pause-at", "3", "--pause-ns", "20000000"});
    EXPECT_EQ(options._processingDelayNs, 1000u);
    EXPECT_EQ(options._pauseAt, 3u);
    EXPECT_EQ(options._pauseNs, 20000000u);
    EXPECT_THROW((void)parse_options({"--warmup-messages", "1"}), BenchmarkFailure);
    EXPECT_THROW((void)parse_options({"--pacing-spin", "1"}), BenchmarkFailure);
    EXPECT_THROW((void)parse_options({"--qualification", "1", "--pause-at", "3"}), BenchmarkFailure);
    EXPECT_THROW((void)parse_options({"--qualification", "1", "--pause-ns", "1"}), BenchmarkFailure);
    EXPECT_THROW((void)parse_options({"--qualification", "1", "--warmup-messages", "4",
        "--pause-at", "12", "--pause-ns", "1"}), BenchmarkFailure);
    EXPECT_THROW((void)parse_options({"--qualification", "1", "--budget-ms", "1",
        "--processing-delay-ns", "1000000"}), BenchmarkFailure);
  }

  TEST(IpcComparisonQualification, ResourceWindowsSubtractEveryCounterAndRejectRegression)
  {
    // Independent snapshots demonstrate that warmup counters disappear from
    // both CPU categories and faults, rather than only from a combined total.
    const ipc::ProcessResources start{100, 200, 3, 4};
    const auto delta = ipc::resource_difference({500, 900, 8, 6}, start);
    EXPECT_EQ(delta._userNs, 400u);
    EXPECT_EQ(delta._systemNs, 700u);
    EXPECT_EQ(delta._minorFaults, 5u);
    EXPECT_EQ(delta._majorFaults, 2u);
    EXPECT_THROW((void)ipc::resource_difference({99, 900, 8, 6}, start), BenchmarkFailure);
    EXPECT_THROW((void)ipc::resource_difference({500, 199, 8, 6}, start), BenchmarkFailure);
    EXPECT_THROW((void)ipc::resource_difference({500, 900, 2, 6}, start), BenchmarkFailure);
    EXPECT_THROW((void)ipc::resource_difference({500, 900, 8, 3}, start), BenchmarkFailure);
  }

  TEST(IpcComparisonDeadlines, ExpirationGuardsHaveTheirOwnExceptionType)
  {
    // The sampled monotonic instant is already reached. No elapsed-time
    // threshold or sleep is needed to establish the expired deadline.
    const auto expired = ipc::monotonic_ns();
    ASSERT_GT(expired, 0u);
    EXPECT_THROW(ipc::check_deadline(expired), ipc::DeadlineExceeded);
    EXPECT_THROW((void)ipc::remaining_ms(expired), ipc::DeadlineExceeded);
    // The target is valid (strictly before its deadline), but the deadline has
    // elapsed by the time this wait checks the clock.
    EXPECT_THROW((void)ipc::wait_until(0, expired), ipc::DeadlineExceeded);
    EXPECT_THROW((void)ipc::spin_until(0, expired), ipc::DeadlineExceeded);
  }

  TEST(IpcComparisonDeadlines, InvalidScheduleAfterExpirationRetainsItsFailureCause)
  {
    ASSERT_THROW(ipc::check_deadline(0), ipc::DeadlineExceeded);
    // A malformed target is not a normal work-budget stop, even though that
    // target/deadline is also in the past.
    EXPECT_THROW((void)ipc::wait_until(0, 0), BenchmarkFailure);
    EXPECT_THROW((void)ipc::spin_until(0, 0), BenchmarkFailure);
  }

  TEST(IpcComparisonDeadlines, InvalidClockRangeAfterExpirationRetainsItsFailureCause)
  {
    ASSERT_THROW(ipc::check_deadline(0), ipc::DeadlineExceeded);
    timespec invalid{};
    invalid.tv_nsec = 1'000'000'000L;
    EXPECT_THROW((void)ipc::timespec_ns(invalid), BenchmarkFailure);
    EXPECT_THROW((void)ipc::deadline_ns(std::numeric_limits<std::uint64_t>::max(), 1),
                 BenchmarkFailure);
  }

  TEST(IpcComparisonDeadlines, CorruptPayloadAfterExpirationRetainsItsFailureCause)
  {
    // Independent valid wire bytes: version/producer=1, flags=0, sequence=1,
    // scheduled=32, logical-start=48, followed by the specified body pattern.
    std::array<std::byte, 64> payload{};
    payload[0] = std::byte{1};
    payload[2] = std::byte{1};
    payload[8] = std::byte{1};
    payload[16] = std::byte{32};
    payload[24] = std::byte{48};
    for (std::size_t index = 32; index < payload.size(); ++index)
    {
      payload[index] = static_cast<std::byte>(1 + index);
    }
    ASSERT_NO_THROW((void)ipc::validate_payload(payload, 1));
    ASSERT_THROW(ipc::check_deadline(0), ipc::DeadlineExceeded);
    payload[42] ^= std::byte{1};
    EXPECT_THROW((void)ipc::validate_payload(payload, 1), BenchmarkFailure);
  }

  TEST(IpcComparisonDeadlines, PublicationErrorAfterExpirationRetainsItsFailureCause)
  {
    ASSERT_THROW(ipc::check_deadline(0), ipc::DeadlineExceeded);
    // Exercise the adapter's real PublicationReport error handling. A late
    // native I/O failure must not acquire the deadline-exhaustion type.
    angaria::PublicationReport failed;
    failed._error = angaria::ErrorCode::IoError;
    EXPECT_THROW(angaria::benchmarks::check_report(failed, {}, {}), BenchmarkFailure);
  }
}
