#include "performance_diagnostics.h"

#include "angaria/detail/checked_arithmetic.h"

#include <cerrno>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <time.h>

namespace angaria::detail::diagnostics
{
  namespace
  {
    thread_local Capture* activeCapture{};

#if defined(ANGARIA_ENABLE_DIAGNOSTICS) && ANGARIA_ENABLE_DIAGNOSTICS
    std::atomic<std::uint64_t> captureEpoch{};

    std::uint64_t next_capture_epoch() noexcept
    {
      auto previous = captureEpoch.load(std::memory_order_relaxed);
      for (;;)
      {
        // Never reuse an identity, even after theoretical counter exhaustion.
        if (previous == std::numeric_limits<std::uint64_t>::max())
        {
          return 0;
        }
        if (captureEpoch.compare_exchange_weak(previous, previous + 1,
                                               std::memory_order_relaxed, std::memory_order_relaxed))
        {
          return previous + 1;
        }
      }
    }
#endif

    std::uint64_t wall_ns() noexcept
    {
      const auto saved = errno;
      const auto result = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
      errno = saved;
      return result;
    }

    bool cpu_ns(std::uint64_t& result) noexcept
    {
      // Observation must not change errno consumed by a native error path.
      const auto saved = errno;
      timespec value{};
      const auto status = ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value);
      errno = saved;
      if (status != 0 || value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000)
      {
        return false;
      }
      const auto seconds = checked_multiply(static_cast<std::uint64_t>(value.tv_sec), 1000000000);
      const auto total = seconds ? checked_add(*seconds, static_cast<std::uint64_t>(value.tv_nsec))
                                 : std::nullopt;
      if (!total)
      {
        return false;
      }
      result = *total;
      return true;
    }

    void accumulate(Sample& sample, std::uint64_t& target, std::uint64_t amount) noexcept
    {
      if (const auto result = checked_add(target, amount))
      {
        target = *result;
      }
      else
      {
        target = std::numeric_limits<std::uint64_t>::max();
        sample._overflow = true;
        sample._valid = false;
      }
    }
  } // namespace

  Capture::Capture(Sample& sample, Collection collection) noexcept
      : _sample(&sample), _previous(activeCapture),
        _collection(instrumentation_enabled ? collection : Collection::None)
  {
    // Reusing an active sample would destroy its outer counters and timing
    // stack. Suspend collection instead and make that misuse visible.
    for (auto* outer = _previous; outer != nullptr; outer = outer->_previous)
    {
      if (outer->_sample == &sample)
      {
        sample._valid = false;
        _sample = nullptr;
        _collection = Collection::None;
        activeCapture = this;
        return;
      }
    }
    sample = Sample{};
#if defined(ANGARIA_ENABLE_DIAGNOSTICS) && ANGARIA_ENABLE_DIAGNOSTICS
    if (_collection != Collection::None)
    {
      _epoch = next_capture_epoch();
      if (_epoch == 0)
      {
        sample._valid = false;
        _collection = Collection::None;
      }
    }
#endif
    _startedNs = wall_ns();
    _cpuAvailable = cpu_ns(_startedCpuNs);
    activeCapture = this;
  }

  Capture::~Capture() noexcept
  {
    std::uint64_t finishedCpu{};
    const auto cpuAvailable = cpu_ns(finishedCpu);
    const auto finished = wall_ns();
    activeCapture = _previous;
    if (_sample == nullptr)
    {
      return;
    }
    if (_scope != nullptr || finished < _startedNs)
    {
      _sample->_valid = false;
      return;
    }
    _sample->_wallNs = finished - _startedNs;
    _sample->_cpuAvailable = _cpuAvailable && cpuAvailable && finishedCpu >= _startedCpuNs;
    if (_sample->_cpuAvailable)
    {
      _sample->_cpuNs = finishedCpu - _startedCpuNs;
    }
    if (_collection != Collection::None)
    {
      auto& other = _sample->_phases[static_cast<std::size_t>(Phase::Other)];
      other._calls = 1;
      if (_collection == Collection::Timings)
      {
        std::uint64_t accounted{};
        for (std::size_t index = 0; index != phase_count; ++index)
        {
          if (index != static_cast<std::size_t>(Phase::Other))
          {
            accumulate(*_sample, accounted, _sample->_phases[index]._exclusiveNs);
          }
        }
        if (accounted > _sample->_wallNs)
        {
          _sample->_valid = false;
        }
        else
        {
          other._inclusiveNs = _sample->_wallNs - accounted;
          other._exclusiveNs = other._inclusiveNs;
        }
      }
    }
  }

  Scope::Scope(Phase phase) noexcept : _phase(phase)
  {
    auto* capture = activeCapture;
    if (capture == nullptr || capture->_sample == nullptr || capture->_collection == Collection::None)
    {
      return;
    }
    const auto index = static_cast<std::size_t>(phase);
    if (index >= phase_count || phase == Phase::Other)
    {
      capture->_sample->_valid = false;
      return;
    }
    _capture = capture;
    _parent = capture->_scope;
    capture->_scope = this;
    accumulate(*capture->_sample, capture->_sample->_phases[index]._calls, 1);
    if (capture->_collection == Collection::Timings)
    {
      _startedNs = wall_ns();
    }
  }

  Scope::~Scope() noexcept
  {
    if (_capture == nullptr)
    {
      return;
    }
    auto& sample = *_capture->_sample;
    if (_capture->_scope != this || activeCapture != _capture)
    {
      sample._valid = false;
      return;
    }
    _capture->_scope = _parent;
    if (_capture->_collection != Collection::Timings)
    {
      return;
    }
    const auto finished = wall_ns();
    if (finished < _startedNs || finished - _startedNs < _childNs)
    {
      sample._valid = false;
      return;
    }
    const auto duration = finished - _startedNs;
    auto& statistics = sample._phases[static_cast<std::size_t>(_phase)];
    accumulate(sample, statistics._inclusiveNs, duration);
    accumulate(sample, statistics._exclusiveNs, duration - _childNs);
    if (_parent)
    {
      accumulate(sample, _parent->_childNs, duration);
    }
  }

  void count(Counter counter, std::uint64_t amount) noexcept
  {
    auto* capture = activeCapture;
    if (!instrumentation_enabled || capture == nullptr || capture->_sample == nullptr ||
        capture->_collection == Collection::None)
    {
      return;
    }
    const auto index = static_cast<std::size_t>(counter);
    if (index >= counter_count)
    {
      capture->_sample->_valid = false;
      return;
    }
    accumulate(*capture->_sample, capture->_sample->_counters[index], amount);
  }

  void GateHold::start() noexcept
  {
#if defined(ANGARIA_ENABLE_DIAGNOSTICS) && ANGARIA_ENABLE_DIAGNOSTICS
    _epoch = 0;
    auto* capture = activeCapture;
    if (capture == nullptr || capture->_sample == nullptr || capture->_collection == Collection::None)
    {
      return;
    }
    _epoch = capture->_epoch;
    if (capture->_collection == Collection::Timings)
    {
      _startedNs = wall_ns();
    }
#endif
  }

  void GateHold::finish() noexcept
  {
#if defined(ANGARIA_ENABLE_DIAGNOSTICS) && ANGARIA_ENABLE_DIAGNOSTICS
    auto* capture = activeCapture;
    // Dereference solely this thread's live Capture. A globally unique epoch
    // prevents a later Capture at a recycled address from matching the token.
    const bool matches = capture != nullptr && _epoch != 0 && capture->_epoch == _epoch;
    _epoch = 0;
    if (!matches || capture->_sample == nullptr || capture->_collection == Collection::None)
    {
      return;
    }
    auto& sample = *capture->_sample;
    const auto holds = static_cast<std::size_t>(Counter::NativeGateHolds);
    accumulate(sample, sample._counters[holds], 1);
    if (capture->_collection == Collection::Timings)
    {
      const auto finished = wall_ns();
      if (finished < _startedNs)
      {
        sample._valid = false;
        return;
      }
      const auto nanoseconds = static_cast<std::size_t>(Counter::NativeGateHoldNs);
      accumulate(sample, sample._counters[nanoseconds], finished - _startedNs);
    }
#endif
  }

#if defined(ANGARIA_ENABLE_DIAGNOSTICS) && ANGARIA_ENABLE_DIAGNOSTICS
  void record_allocation(std::size_t bytes) noexcept
  {
    count(Counter::Allocations);
    count(Counter::AllocatedBytes, static_cast<std::uint64_t>(bytes));
  }
#endif
} // namespace angaria::detail::diagnostics
