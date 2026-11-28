#pragma once

// Small reporting helpers for isolated cost studies. Preserve each raw block;
// the odd-sized median is only a summary and never a CI timing threshold.

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>

namespace angaria::test_support
{
  template <std::size_t Count>
  double median(std::array<double, Count> values)
  {
    static_assert(Count > 0 && Count % 2 == 1);
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
  }

  template <typename Value, std::size_t Count>
  void write_samples(const std::array<Value, Count>& values)
  {
    std::cout << '[';
    for (std::size_t index = 0; index != values.size(); ++index)
    {
      if (index != 0)
      {
        std::cout << ',';
      }
      std::cout << values[index];
    }
    std::cout << ']';
  }
} // namespace angaria::test_support
