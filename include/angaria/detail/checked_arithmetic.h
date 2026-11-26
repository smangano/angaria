#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace angaria::detail
{
  // Pure arithmetic, independent of format ceilings or an operation's errors.
  constexpr std::optional<std::uint64_t> checked_add(std::uint64_t left, std::uint64_t right)
  {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
    {
      return std::nullopt;
    }
    return left + right;
  }

  constexpr std::optional<std::uint64_t> checked_multiply(std::uint64_t left, std::uint64_t right)
  {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
    {
      return std::nullopt;
    }
    return left * right;
  }

  constexpr std::optional<std::uint64_t> checked_align(std::uint64_t value, std::uint64_t alignment)
  {
    if (alignment == 0)
    {
      return std::nullopt;
    }
    const auto remainder = value % alignment;
    return checked_add(value, remainder == 0 ? 0 : alignment - remainder);
  }

  // Empty ranges may end at the boundary. Required references check emptiness
  // separately. Subtraction avoids overflow for hostile offset/length values.
  constexpr bool contains_range(std::uint64_t extent, std::uint64_t offset, std::uint64_t length)
  {
    return offset <= extent && length <= extent - offset;
  }

  constexpr bool fits_size_t(std::uint64_t value)
  {
    return value <= std::numeric_limits<std::size_t>::max();
  }

  constexpr bool fits_host_size(std::uint64_t value)
  {
    return fits_size_t(value) &&
           value <= static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max());
  }
} // namespace angaria::detail
