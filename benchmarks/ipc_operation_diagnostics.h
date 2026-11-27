#pragma once

#include "diagnostic_totals.h"

#include <array>
#include <cstdint>
#include <limits>
#include <ostream>
#include <utility>

namespace angaria::benchmarks::ipc
{
  namespace observation = angaria::detail::diagnostics;

#if defined(ANGARIA_ENABLE_DIAGNOSTICS) && ANGARIA_ENABLE_DIAGNOSTICS
  using OperationTotals = DiagnosticTotals;
#else
  struct OperationTotals
  {
  };
#endif

  // Ordinary builds inline the direct call: no Sample initialization, clocks,
  // allocation or aggregation is performed on their measured path.
  template<class Operation>
  auto observe_operation(OperationTotals& totals, observation::Collection collection, Operation&& operation)
  {
#if defined(ANGARIA_ENABLE_DIAGNOSTICS) && ANGARIA_ENABLE_DIAGNOSTICS
    observation::Sample sample;
    struct Record
    {
      OperationTotals& _totals;
      const observation::Sample& _sample;
      ~Record() noexcept
      {
        _totals.add(_sample);
      }
    } record{totals, sample};
    // Capture is destroyed before Record, including exceptional returns.
    observation::Capture capture{sample, collection};
#else
    (void)totals;
    (void)collection;
#endif
    return std::forward<Operation>(operation)();
  }

  inline void write_diagnostics(std::ostream& output, const std::array<OperationTotals, 4>& operations)
  {
    output << "{\"operations\":[";
#if defined(ANGARIA_ENABLE_DIAGNOSTICS) && ANGARIA_ENABLE_DIAGNOSTICS
    constexpr std::array<const char*, 4> names{"issue", "send", "receive", "acknowledge"};
    for (std::size_t index = 0; index < operations.size(); ++index)
    {
      if (index != 0)
      {
        output << ',';
      }
      const auto& operation = operations[index];
      const auto& sample = operation._sample;
      output << "{\"name\":\"" << names[index] << "\",\"calls\":" << operation._calls
             << ",\"wall_ns\":" << sample._wallNs << ",\"cpu_ns\":" << sample._cpuNs
             << ",\"valid\":" << (sample._valid ? "true" : "false")
             << ",\"overflow\":" << (sample._overflow ? "true" : "false") << ",\"phases\":{";
      for (std::size_t phase = 0; phase < observation::phase_count; ++phase)
      {
        if (phase != 0)
        {
          output << ',';
        }
        const auto& value = sample._phases[phase];
        output << '"' << observation::phase_names[phase] << "\":{\"calls\":" << value._calls
               << ",\"inclusive_ns\":" << value._inclusiveNs
               << ",\"exclusive_ns\":" << value._exclusiveNs << '}';
      }
      output << "},\"counters\":{";
      for (std::size_t counter = 0; counter < observation::counter_count; ++counter)
      {
        if (counter != 0)
        {
          output << ',';
        }
        output << '"' << observation::counter_names[counter] << "\":" << sample._counters[counter];
      }
      output << "}}";
    }
#else
    (void)operations;
#endif
    output << "]}";
  }
}
