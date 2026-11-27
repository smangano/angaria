// Private benchmark helper contracts, not public Angaria usage examples.
// These tests exercise the exception-type boundary used by the adapter's
// measured-phase handlers. They do not inject failures into a running Worker
// or claim to test its complete report/coordinator lifecycle.
#include "ipc_comparison_support.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace
{
  namespace ipc = angaria::benchmarks::ipc;
  using angaria::benchmarks::BenchmarkFailure;

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
  }

  TEST(IpcComparisonDeadlines, InvalidScheduleAfterExpirationRetainsItsFailureCause)
  {
    ASSERT_THROW(ipc::check_deadline(0), ipc::DeadlineExceeded);
    // A malformed target is not a normal work-budget stop, even though that
    // target/deadline is also in the past.
    EXPECT_THROW((void)ipc::wait_until(0, 0), BenchmarkFailure);
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
