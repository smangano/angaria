// Private measurement plumbing, not application setup. These tests qualify
// nested accounting and collection isolation without asserting machine speed.
#include "performance_diagnostics.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <new>
#include <stdexcept>
#include <thread>

#include <time.h>

namespace
{
  namespace diagnostics = angaria::detail::diagnostics;

  TEST(PerformanceDiagnosticsTest, DisabledCollectionKeepsOnlyTheRootClocks)
  {
    diagnostics::Sample sample;
    timespec cpu{};
    const auto cpuAvailable = ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) == 0;
    {
      diagnostics::Capture capture(sample, diagnostics::Collection::None);
      diagnostics::GateHold hold;
      hold.start();
      ANGARIA_DIAGNOSTIC_SCOPE(Topology);
      ANGARIA_DIAGNOSTIC_COUNT(PrefixRecords, 19);
      // Wait for an observed clock transition, with no elapsed-time target.
      // This verifies that root clocks remain active in a disabled build.
      const auto first = std::chrono::steady_clock::now();
      while (std::chrono::steady_clock::now() == first)
      {
        std::this_thread::yield();
      }
      hold.finish();
    }
    EXPECT_TRUE(sample._valid);
    EXPECT_FALSE(sample._overflow);
    EXPECT_GT(sample._wallNs, 0u);
    EXPECT_EQ(sample._cpuAvailable, cpuAvailable);
    EXPECT_EQ(std::accumulate(sample._counters.begin(), sample._counters.end(), std::uint64_t{}), 0u);
    for (const auto& phase : sample._phases)
    {
      EXPECT_EQ(phase._calls, 0u);
      EXPECT_EQ(phase._inclusiveNs, 0u);
      EXPECT_EQ(phase._exclusiveNs, 0u);
    }
  }

  TEST(PerformanceDiagnosticsTest, CountsHaveNoPhaseClocksAndCapturesResetTheirSample)
  {
    diagnostics::Sample sample;
    sample._counters[static_cast<std::size_t>(diagnostics::Counter::PrefixRecords)] = 99;
    {
      diagnostics::Capture capture(sample, diagnostics::Collection::Counts);
      diagnostics::GateHold hold;
      hold.start();
      ANGARIA_DIAGNOSTIC_SCOPE(Topology);
      ANGARIA_DIAGNOSTIC_COUNT(PrefixRecords, 7);
      {
        ANGARIA_DIAGNOSTIC_SCOPE(PrefixScan);
        ANGARIA_DIAGNOSTIC_COUNT(PrefixRecords, 5);
      }
      hold.finish();
      hold.finish(); // A completed interval must not be counted twice.
    }
    EXPECT_TRUE(sample._valid);
    const auto expected = diagnostics::instrumentation_enabled ? 12u : 0u;
    EXPECT_EQ(sample._counters[static_cast<std::size_t>(diagnostics::Counter::PrefixRecords)], expected);
    EXPECT_EQ(sample._phases[static_cast<std::size_t>(diagnostics::Phase::Topology)]._calls,
              diagnostics::instrumentation_enabled ? 1u : 0u);
    EXPECT_EQ(sample._counters[static_cast<std::size_t>(diagnostics::Counter::NativeGateHolds)],
              diagnostics::instrumentation_enabled ? 1u : 0u);
    EXPECT_EQ(sample._counters[static_cast<std::size_t>(diagnostics::Counter::NativeGateHoldNs)], 0u);
    for (const auto& phase : sample._phases)
    {
      EXPECT_EQ(phase._inclusiveNs, 0u);
      EXPECT_EQ(phase._exclusiveNs, 0u);
    }
  }

  TEST(PerformanceDiagnosticsTest, NestedAndRepeatedPhasesPartitionTheRootWallTime)
  {
    diagnostics::Sample sample;
    {
      diagnostics::Capture capture(sample, diagnostics::Collection::Timings);
      diagnostics::GateHold hold;
      hold.start();
      ANGARIA_DIAGNOSTIC_SCOPE(Topology);
      {
        ANGARIA_DIAGNOSTIC_SCOPE(PrefixScan);
        ANGARIA_DIAGNOSTIC_SCOPE(PrefixScan);
        ANGARIA_DIAGNOSTIC_COUNT(PrefixRecords, 3);
      }
      hold.finish();
    }
    EXPECT_TRUE(sample._valid);
    EXPECT_FALSE(sample._overflow);
    std::uint64_t exclusive{};
    for (const auto& phase : sample._phases)
    {
      EXPECT_LE(phase._exclusiveNs, phase._inclusiveNs);
      exclusive += phase._exclusiveNs;
    }
    if (diagnostics::instrumentation_enabled)
    {
      EXPECT_EQ(exclusive, sample._wallNs);
      EXPECT_EQ(sample._phases[static_cast<std::size_t>(diagnostics::Phase::PrefixScan)]._calls, 2u);
      EXPECT_EQ(sample._phases[static_cast<std::size_t>(diagnostics::Phase::Other)]._calls, 1u);
      EXPECT_EQ(sample._counters[static_cast<std::size_t>(diagnostics::Counter::NativeGateHolds)], 1u);
      EXPECT_LE(sample._counters[static_cast<std::size_t>(diagnostics::Counter::NativeGateHoldNs)], sample._wallNs);
    }
    else
    {
      EXPECT_EQ(exclusive, 0u);
    }
  }

  TEST(PerformanceDiagnosticsTest, ExceptionUnwindingRestoresTheOuterPhase)
  {
    diagnostics::Sample sample;
    {
      diagnostics::Capture capture(sample, diagnostics::Collection::Timings);
      ANGARIA_DIAGNOSTIC_SCOPE(Topology);
      try
      {
        ANGARIA_DIAGNOSTIC_SCOPE(PrefixScan);
        throw std::runtime_error("Private diagnostic unwind scenario");
      }
      catch (const std::runtime_error&)
      {
        ANGARIA_DIAGNOSTIC_COUNT(PrefixRecords, 4);
      }
    }
    EXPECT_TRUE(sample._valid);
    EXPECT_EQ(sample._counters[static_cast<std::size_t>(diagnostics::Counter::PrefixRecords)],
              diagnostics::instrumentation_enabled ? 4u : 0u);
    EXPECT_EQ(sample._phases[static_cast<std::size_t>(diagnostics::Phase::Topology)]._calls,
              diagnostics::instrumentation_enabled ? 1u : 0u);
    EXPECT_EQ(sample._phases[static_cast<std::size_t>(diagnostics::Phase::PrefixScan)]._calls,
              diagnostics::instrumentation_enabled ? 1u : 0u);
  }

  TEST(PerformanceDiagnosticsTest, SeparateNestedCapturesAndThreadsKeepIndependentSamples)
  {
    diagnostics::Sample outer;
    diagnostics::Sample nested;
    diagnostics::Sample worker;
    ANGARIA_DIAGNOSTIC_COUNT(PrefixRecords, 100);
    {
      diagnostics::Capture capture(outer, diagnostics::Collection::Counts);
      ANGARIA_DIAGNOSTIC_COUNT(PrefixRecords, 2);
      {
        diagnostics::Capture inner(nested, diagnostics::Collection::Counts);
        ANGARIA_DIAGNOSTIC_COUNT(PrefixRecords, 7);
      }
      std::thread thread([&]
      {
        diagnostics::Capture separate(worker, diagnostics::Collection::Counts);
        ANGARIA_DIAGNOSTIC_COUNT(PrefixRecords, 11);
      });
      thread.join();
      ANGARIA_DIAGNOSTIC_COUNT(PrefixRecords, 3);
    }
    const auto index = static_cast<std::size_t>(diagnostics::Counter::PrefixRecords);
    EXPECT_TRUE(outer._valid);
    EXPECT_TRUE(nested._valid);
    EXPECT_TRUE(worker._valid);
    EXPECT_EQ(outer._counters[index], diagnostics::instrumentation_enabled ? 5u : 0u);
    EXPECT_EQ(nested._counters[index], diagnostics::instrumentation_enabled ? 7u : 0u);
    EXPECT_EQ(worker._counters[index], diagnostics::instrumentation_enabled ? 11u : 0u);
  }

  TEST(PerformanceDiagnosticsTest, CounterOverflowMarksTheSampleInsteadOfWrapping)
  {
    diagnostics::Sample sample;
    {
      diagnostics::Capture capture(sample, diagnostics::Collection::Counts);
      ANGARIA_DIAGNOSTIC_COUNT(PrefixRecords, std::numeric_limits<std::uint64_t>::max());
      ANGARIA_DIAGNOSTIC_COUNT(PrefixRecords, 1);
    }
    EXPECT_EQ(sample._valid, !diagnostics::instrumentation_enabled);
    EXPECT_EQ(sample._overflow, diagnostics::instrumentation_enabled);
    EXPECT_EQ(sample._counters[static_cast<std::size_t>(diagnostics::Counter::PrefixRecords)],
              diagnostics::instrumentation_enabled ? std::numeric_limits<std::uint64_t>::max() : 0u);
  }

  TEST(PerformanceDiagnosticsTest, MacroArgumentsAreAbsentFromDisabledBuilds)
  {
    unsigned evaluations{};
    ANGARIA_DIAGNOSTIC_COUNT(PrefixRecords, ++evaluations);
    EXPECT_EQ(evaluations, diagnostics::instrumentation_enabled ? 1u : 0u);
  }

  TEST(PerformanceDiagnosticsTest, ObservationPreservesNativeErrorContext)
  {
    diagnostics::Sample sample;
    errno = E2BIG;
    {
      diagnostics::Capture capture(sample, diagnostics::Collection::Timings);
      ANGARIA_DIAGNOSTIC_SCOPE(NativeStat);
      ANGARIA_DIAGNOSTIC_COUNT(FileStatCalls, 1);
    }
    // A real native operation reads errno after a failed syscall. Merely
    // observing its surrounding phase must not replace that error context.
    EXPECT_EQ(errno, E2BIG);
    EXPECT_TRUE(sample._valid);
  }

  TEST(PerformanceDiagnosticsTest, NativeGateTokensIgnoreExpiredAndReusedCaptureStorage)
  {
    diagnostics::GateHold token;
    diagnostics::Sample original;
    diagnostics::Sample replacement;
    alignas(diagnostics::Capture) std::array<std::byte, sizeof(diagnostics::Capture)> storage{};
    auto* first = ::new (storage.data()) diagnostics::Capture(original, diagnostics::Collection::Timings);
    token.start();
    first->~Capture();
    // Reuse the exact address of the expired Capture. Only a unique epoch,
    // rather than an address comparison, can reject this stale interval.
    auto* next = ::new (storage.data()) diagnostics::Capture(replacement, diagnostics::Collection::Timings);
    token.finish();
    next->~Capture();
    EXPECT_TRUE(original._valid);
    EXPECT_TRUE(replacement._valid);
    const auto holds = static_cast<std::size_t>(diagnostics::Counter::NativeGateHolds);
    EXPECT_EQ(original._counters[holds], 0u);
    EXPECT_EQ(replacement._counters[holds], 0u);

    // A token can also outlive its Sample entirely; finish has no saved Sample
    // pointer to follow after the caller's storage has ceased to exist.
    {
      diagnostics::Sample ephemeral;
      diagnostics::Capture capture(ephemeral, diagnostics::Collection::Timings);
      token.start();
    }
    token.finish();
    EXPECT_TRUE(original._valid);
  }

  TEST(PerformanceDiagnosticsTest, NativeGateTokensBelongOnlyToTheirCurrentCapture)
  {
    diagnostics::Sample outer;
    diagnostics::Sample nested;
    diagnostics::Sample foreign;
    diagnostics::GateHold token;
    {
      diagnostics::Capture capture(outer, diagnostics::Collection::Counts);
      token.start();
      {
        diagnostics::Capture inside(nested, diagnostics::Collection::Counts);
        token.finish(); // The outer interval must not be charged to this Sample.
        diagnostics::GateHold own;
        own.start();
        own.finish();
      }
      token.start();
      std::thread worker([&]
      {
        diagnostics::Capture other(foreign, diagnostics::Collection::Counts);
        token.finish(); // Another thread's active Capture must not receive it.
      });
      worker.join();
      token.start();
      token.finish();
    }
    const auto holds = static_cast<std::size_t>(diagnostics::Counter::NativeGateHolds);
    EXPECT_TRUE(outer._valid);
    EXPECT_TRUE(nested._valid);
    EXPECT_TRUE(foreign._valid);
    EXPECT_EQ(outer._counters[holds], diagnostics::instrumentation_enabled ? 1u : 0u);
    EXPECT_EQ(nested._counters[holds], diagnostics::instrumentation_enabled ? 1u : 0u);
    EXPECT_EQ(foreign._counters[holds], 0u);
  }
} // namespace
