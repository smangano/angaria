#pragma once

#include "session_metadata.h"

#include "angaria/detail/byte_operations.h"
#include "angaria/detail/checked_arithmetic.h"
#include "angaria/detail/error_support.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angaria::detail::mapped::definition
{
  // Shared mechanics for Registry definitions, including their common name/doc
  // slices and diagnostics. These bodies permit eight-byte alignment; native
  // Publication allocation deliberately has a separate stricter contract.
  inline void check_text(std::string_view name, std::string_view doc, const char* namePath,
                  const char* docPath, ErrorCode code)
  {
    value_or_throw<CodecFailure>(validate_metadata_text(name, true, true, namePath, code));
    value_or_throw<CodecFailure>(validate_metadata_text(doc, false, false, docPath, code));
  }

  inline void check_registration(const RegistrationMetadata& registration, ErrorCode code)
  {
    codec_require(registration._createdBy != TaskId::Invalid && registration._revision != 0,
            code, "definition.registration", "Definition creator and revision must be nonzero");
  }

  inline std::uint64_t body_begin(std::uint64_t offset, std::uint64_t length, ErrorCode code)
  {
    codec_require(offset >= superblock_bytes && offset % record_alignment == 0 &&
                contains_range(max_log_bytes, offset, record_envelope_bytes),
            code, "definition.offset", "Invalid definition record offset");
    const auto begin = offset + record_envelope_bytes;
    codec_require(contains_range(max_log_bytes, begin, length), code, "definition.offset",
            "Definition body exceeds format bounds");
    return begin;
  }

  inline std::size_t host_size(std::uint64_t value, ErrorCode code)
  {
    codec_require(angaria::detail::fits_host_size(value),
            code, "definition.length", "Definition length exceeds host bounds");
    return static_cast<std::size_t>(value);
  }

  inline std::vector<std::byte> encode_text(std::uint64_t prefixBytes, std::string_view name,
                                     std::string_view doc, std::uint64_t offset)
  {
    const auto withName = checked_add(prefixBytes, name.size());
    const auto total = withName ? checked_add(*withName, doc.size()) : std::nullopt;
    codec_require(total.has_value(), ErrorCode::InvalidArgument, "definition.length", "Definition length overflows");
    const auto length = host_size(*total, ErrorCode::InvalidArgument);
    const auto begin = body_begin(offset, *total, ErrorCode::InvalidArgument);
    std::vector<std::byte> bytes(length);
    auto cursor = host_size(prefixBytes, ErrorCode::InvalidArgument);
    const std::array<std::string_view, 2> strings{name, doc};
    for (std::size_t index = 0; index < strings.size(); ++index)
    {
      if (strings[index].empty())
      {
        continue;
      }
      write_unsigned_le<std::uint64_t>(bytes, 32 + index * 16, begin + cursor);
      write_unsigned_le<std::uint64_t>(bytes, 40 + index * 16, strings[index].size());
      for (const auto character : strings[index])
      {
        bytes[cursor++] = static_cast<std::byte>(static_cast<unsigned char>(character));
      }
    }
    return bytes;
  }

  inline std::array<std::string, 2> decode_text(std::span<const std::byte> bytes, std::size_t cursor,
                                         std::uint64_t begin, const char* namePath, const char* docPath)
  {
    std::array<std::string_view, 2> strings;
    for (std::size_t index = 0; index < strings.size(); ++index)
    {
      const auto reference = read_unsigned_le<std::uint64_t>(bytes, 32 + index * 16);
      const auto length = read_unsigned_le<std::uint64_t>(bytes, 40 + index * 16);
      if (length == 0)
      {
        codec_require(reference == 0, ErrorCode::IoError, "definition.strings", "Empty string slice has a nonzero offset");
        continue;
      }
      codec_require(reference == begin + cursor && length <= bytes.size() - cursor,
              ErrorCode::IoError, "definition.strings", "String slice is outside its canonical owning body");
      strings[index] = std::string_view{reinterpret_cast<const char*>(bytes.data() + cursor),
                                        static_cast<std::size_t>(length)};
      cursor += static_cast<std::size_t>(length);
    }
    codec_require(bytes.size() - cursor < section_alignment && all_zero(bytes.subspan(cursor)),
            ErrorCode::IoError, "definition.padding", "Unexpected definition allocation padding");
    check_text(strings[0], strings[1], namePath, docPath, ErrorCode::IoError);
    return {std::string{strings[0]}, std::string{strings[1]}};
  }
} // namespace angaria::detail::mapped::definition
