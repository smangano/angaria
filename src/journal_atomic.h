#pragma once

#include "native_atomic.h"
#include "native_file.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include <variant>
#include <vector>

namespace angaria::detail::mapped
{
  // Compact-command readers hold the Session gate and supply validated ranges.
  // Copy live words through the native adapter, never ordinary mapped loads.
  inline Result<std::vector<std::uint64_t>> read_journal_words(
      NativeFile& file, std::uint64_t offset, std::size_t count)
  {
    try
    {
      if (count == 0)
      {
        return std::vector<std::uint64_t>{};
      }
      if (count > std::numeric_limits<std::size_t>::max() / 8)
      {
        return Error{ErrorCode::IoError, "Atomic journal range exceeds host bounds", "journal.words"};
      }
      auto mapping = file.map_read_write(offset, count * 8);
      if (const auto* error = std::get_if<Error>(&mapping))
      {
        return *error;
      }
      auto bytes = std::get<MappedRegion>(mapping).writable_bytes();
      if (const auto* error = std::get_if<Error>(&bytes))
      {
        return *error;
      }
      const auto view = std::get<std::span<std::byte>>(bytes);
      std::vector<std::uint64_t> result;
      result.reserve(count);
      for (std::size_t index = 0; index != count; ++index)
      {
        auto word = AtomicWord::bind(view, index * 8);
        if (const auto* error = std::get_if<Error>(&word))
        {
          return *error;
        }
        result.push_back(std::get<AtomicWord>(word).load_acquire());
      }
      return result;
    }
    catch (const std::bad_alloc&)
    {
      return Error{ErrorCode::CapacityExceeded, "Insufficient memory for atomic journal words", "journal.words"};
    }
  }
} // namespace angaria::detail::mapped
