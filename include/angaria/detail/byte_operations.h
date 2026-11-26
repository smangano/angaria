#pragma once

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace angaria::detail
{
  static_assert(CHAR_BIT == 8);

  // Local byte copies only: callers establish the requested range is in bounds
  // and a dynamic width is in 0..8. Typed calls use sizeof(T) for that width.
  // These functions handle unaligned buffers and explicitly encode little endian.
  // Live mapped synchronization words must use the native atomic adapter instead.
  constexpr std::uint64_t read_unsigned_le(std::span<const std::byte> bytes, std::size_t offset,
                                           std::size_t width)
  {
    std::uint64_t value{};
    for (std::size_t byte = 0; byte < width; ++byte)
    {
      value |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + byte])} << (8 * byte);
    }
    return value;
  }

  constexpr void write_unsigned_le(std::span<std::byte> bytes, std::size_t offset,
                                     std::uint64_t value, std::size_t width)
  {
    for (std::size_t byte = 0; byte < width; ++byte)
    {
      bytes[offset + byte] = static_cast<std::byte>((std::uint64_t{value} >> (8 * byte)) & 0xff);
    }
  }

  template <typename T>
  constexpr T read_unsigned_le(std::span<const std::byte> bytes, std::size_t offset)
  {
    static_assert(std::is_unsigned_v<T> && !std::is_same_v<T, bool> && sizeof(T) <= 8);
    return static_cast<T>(read_unsigned_le(bytes, offset, sizeof(T)));
  }

  template <typename T>
  constexpr void write_unsigned_le(std::span<std::byte> bytes, std::size_t offset, T value)
  {
    static_assert(std::is_unsigned_v<T> && !std::is_same_v<T, bool> && sizeof(T) <= 8);
    write_unsigned_le(bytes, offset, static_cast<std::uint64_t>(value), sizeof(T));
  }

  // The prototype stores uint8_t buffers. Keep the same byte implementation
  // without coupling its separate record format to the native mapped format.
  template <typename T>
  inline T read_unsigned_le(std::span<const std::uint8_t> bytes, std::size_t offset)
  {
    return read_unsigned_le<T>(std::as_bytes(bytes), offset);
  }

  template <typename T>
  inline void write_unsigned_le(std::span<std::uint8_t> bytes, std::size_t offset, T value)
  {
    write_unsigned_le(std::as_writable_bytes(bytes), offset, value);
  }

  constexpr bool all_zero(std::span<const std::byte> bytes)
  {
    return std::all_of(bytes.begin(), bytes.end(), [](auto value)
    {
      return value == std::byte{};
    });
  }

  constexpr bool all_zero(std::span<const std::uint64_t> words)
  {
    return std::all_of(words.begin(), words.end(), [](auto value)
    {
      return value == 0;
    });
  }

  // Interpret a stored two's-complement integer without an out-of-range
  // unsigned-to-signed conversion or a dependency on the host representation.
  template <typename Signed>
  constexpr Signed signed_from_bits(std::make_unsigned_t<Signed> bits)
  {
    static_assert(std::is_signed_v<Signed> && std::is_integral_v<Signed> && sizeof(Signed) <= 8);
    using Unsigned = std::make_unsigned_t<Signed>;
    if (bits <= static_cast<Unsigned>(std::numeric_limits<Signed>::max()))
    {
      return static_cast<Signed>(bits);
    }
    return static_cast<Signed>(-Signed{1} - static_cast<Signed>(std::numeric_limits<Unsigned>::max() - bits));
  }
} // namespace angaria::detail
