#pragma once

#include "angaria/detail/byte_operations.h"
#include "delivery_state.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace angaria::detail::mapped
{
  // Bridges native atomic word copies to local codec bytes. These functions
  // never read a live mapped span or provide synchronization themselves.
  inline OperationResult write_local_words(std::span<std::byte> bytes,
                                             std::span<const std::uint64_t> words)
  {
    if (bytes.size() % 8 != 0 || bytes.size() / 8 != words.size())
    {
      return Error{ErrorCode::InvalidArgument, "Local byte and word counts disagree", "delivery.words"};
    }
    for (std::size_t index = 0; index < words.size(); ++index)
    {
      write_unsigned_le(bytes, index * 8, words[index]);
    }
    return std::monostate{};
  }

  inline Result<Receipt> decode_receipt_words(std::span<const std::uint64_t> words,
                                               ReceiptProfile profile = ReceiptProfile::Core)
  {
    ReceiptBytes bytes{};
    const auto result = write_local_words(bytes, words);
    if (const auto* error = std::get_if<Error>(&result))
    {
      return *error;
    }
    return decode_receipt(bytes, profile);
  }

  inline Result<std::array<std::uint64_t, 16>> encode_receipt_words(
      const Receipt& receipt, ReceiptProfile profile = ReceiptProfile::Core)
  {
    const auto encoded = encode_receipt(receipt, profile);
    if (const auto* error = std::get_if<Error>(&encoded))
    {
      return *error;
    }
    const auto& bytes = std::get<ReceiptBytes>(encoded);
    std::array<std::uint64_t, 16> words{};
    for (std::size_t index = 0; index < words.size(); ++index)
    {
      words[index] = read_unsigned_le<std::uint64_t>(bytes, index * 8);
    }
    return words;
  }
} // namespace angaria::detail::mapped
