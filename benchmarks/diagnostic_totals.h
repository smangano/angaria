#pragma once

#include "performance_diagnostics.h"
#include "angaria/detail/checked_arithmetic.h"

#include <cstddef>
#include <cstdint>

namespace angaria::benchmarks
{
  namespace diagnostics = angaria::detail::diagnostics;
  struct DiagnosticTotals
  {
    void add(const diagnostics::Sample& sample)
    {
      const bool first = _calls == 0;
      const auto append = [&](std::uint64_t& target, std::uint64_t amount)
      {
        const auto added = detail::checked_add(target, amount);
        if (added)
        {
          target = *added;
        }
        else
        {
          _sample._overflow = true;
          _sample._valid = false;
        }
      };
      append(_calls, 1);
      append(_sample._wallNs, sample._wallNs);
      append(_sample._cpuNs, sample._cpuNs);
      _sample._cpuAvailable = first ? sample._cpuAvailable : _sample._cpuAvailable && sample._cpuAvailable;
      _sample._valid = _sample._valid && sample._valid;
      _sample._overflow = _sample._overflow || sample._overflow;
      for (std::size_t index = 0; index < diagnostics::phase_count; ++index)
      {
        append(_sample._phases[index]._calls, sample._phases[index]._calls);
        append(_sample._phases[index]._inclusiveNs, sample._phases[index]._inclusiveNs);
        append(_sample._phases[index]._exclusiveNs, sample._phases[index]._exclusiveNs);
      }
      for (std::size_t index = 0; index < diagnostics::counter_count; ++index)
      {
        append(_sample._counters[index], sample._counters[index]);
      }
    }

    std::uint64_t _calls{};
    diagnostics::Sample _sample;
  };

}
