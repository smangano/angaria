#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace angaria::test_support
{
  // Independent reference bytes for test fixtures. Do not delegate to production
  // byte_operations.h: expected records must still detect an encoder mistake.
  // Suites keep their field offsets, stored codes, and expected values explicit.
  inline void put_integer(std::span<std::byte> bytes, std::size_t offset,
                            std::uint64_t value, std::size_t width = 8)
  {
    if (width > 8 || offset > bytes.size() || width > bytes.size() - offset)
    {
      throw std::runtime_error("Invalid hand-authored integer byte range");
    }
    for (std::size_t byte = 0; byte < width; ++byte)
    {
      bytes[offset + byte] = static_cast<std::byte>((value >> (8 * byte)) & 0xff);
    }
  }

  inline std::uint64_t get_integer(std::span<const std::byte> bytes, std::size_t offset,
                                    std::size_t width = 8)
  {
    if (width > 8 || offset > bytes.size() || width > bytes.size() - offset)
    {
      throw std::runtime_error("Invalid hand-authored integer byte range");
    }
    std::uint64_t value{};
    for (std::size_t byte = 0; byte < width; ++byte)
    {
      value |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + byte])} << (8 * byte);
    }
    return value;
  }

  inline void append_text(std::vector<std::byte>& bytes, std::string_view text)
  {
    for (const auto character : text)
    {
      bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
  }

  inline std::vector<std::byte> octets(std::initializer_list<unsigned> values)
  {
    std::vector<std::byte> result;
    for (const auto value : values)
    {
      if (value > 255)
      {
        throw std::runtime_error("Invalid hand-authored golden byte");
      }
      result.push_back(static_cast<std::byte>(value));
    }
    return result;
  }
} // namespace angaria::test_support
