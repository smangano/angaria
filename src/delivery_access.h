#pragma once

#include "delivery_words.h"
#include "runtime_state.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace angaria::detail
{
  // Copy live mutable words through RuntimeAccess's native atomic path, then
  // serialize only the local values for a representation codec.
  inline std::vector<std::byte> runtime_atomic_bytes(RuntimeAccess& access,
                                                      std::uint64_t offset,
                                                      std::size_t words)
  {
    const auto values = access.read_words(offset, words);
    std::vector<std::byte> bytes(words * 8);
    runtime_value(mapped::write_local_words(bytes, values));
    return bytes;
  }
} // namespace angaria::detail
