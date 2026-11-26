#pragma once

#include <array>
#include <cstddef>
#include <ostream>
#include <string_view>

namespace angaria::detail
{
  // One source byte expands to at most four printable ASCII bytes. Formatting
  // owners choose whether quotes are special and retain their own byte budgets
  // or streaming buffers; this primitive allocates no memory and performs no I/O.
  struct EscapedByte
  {
    std::array<char, 4> _bytes{};
    std::size_t _size{};
  };

  inline constexpr char hex_digit(unsigned value)
  {
    constexpr std::string_view digits{"0123456789abcdef"};
    return digits[value & 0xf];
  }

  inline constexpr EscapedByte escape_ascii_byte(unsigned char byte, bool quoted = false)
  {
    if (byte == '\\' || (quoted && byte == '"'))
    {
      return EscapedByte{{'\\', static_cast<char>(byte)}, 2};
    }
    if (byte >= 0x20 && byte <= 0x7e)
    {
      return EscapedByte{{static_cast<char>(byte)}, 1};
    }
    return EscapedByte{{'\\', 'x', hex_digit(byte >> 4), hex_digit(byte)}, 4};
  }

  // Keep untrusted diagnostics printable without allocating a second string.
  // The caller chooses line boundaries; quotes stay ordinary printable bytes.
  inline void write_diagnostic_text(std::ostream& output, std::string_view text)
  {
    std::array<char, 1024> buffer{};
    std::size_t used{};
    for (const auto character : text)
    {
      if (buffer.size() - used < 4)
      {
        output.write(buffer.data(), static_cast<std::streamsize>(used));
        used = 0;
      }
      const auto escaped = escape_ascii_byte(static_cast<unsigned char>(character));
      for (std::size_t index = 0; index < escaped._size; ++index)
      {
        buffer[used++] = escaped._bytes[index];
      }
    }
    output.write(buffer.data(), static_cast<std::streamsize>(used));
  }
} // namespace angaria::detail
