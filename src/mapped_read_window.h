#pragma once

#include "native_atomic.h"
#include "native_file.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <variant>

namespace angaria::detail::mapped
{
  // Private operation-local metadata reader. The caller holds the mutation
  // gate and supplies a stable recovered end; a window never maps append space
  // beyond that end. Copies own their bytes, and mutable words remain atomic.
  // No borrowed mapping span leaves this reader.
  class MappedReadWindow
  {
  public:
    MappedReadWindow(NativeFile& file, std::uint64_t limit) noexcept
        : _file(&file), _limit(limit)
    {
    }

    MappedReadWindow(const MappedReadWindow&) = delete;
    MappedReadWindow& operator=(const MappedReadWindow&) = delete;

    MappedReadWindow(MappedReadWindow&& other) noexcept
        : _file(std::exchange(other._file, nullptr)),
          _limit(std::exchange(other._limit, 0)),
          _windowBegin(std::exchange(other._windowBegin, 0)),
          _mapping(std::move(other._mapping)),
          _bytes(std::exchange(other._bytes, {}))
    {
    }

    MappedReadWindow& operator=(MappedReadWindow&& other) noexcept
    {
      if (this != &other)
      {
        _bytes = {};
        _mapping = std::move(other._mapping);
        _file = std::exchange(other._file, nullptr);
        _limit = std::exchange(other._limit, 0);
        _windowBegin = std::exchange(other._windowBegin, 0);
        _bytes = std::exchange(other._bytes, {});
      }
      return *this;
    }

    OperationResult copy_bytes(std::uint64_t offset, std::span<std::byte> output)
    {
      if (!contains_range(_limit, offset, output.size()))
      {
        return range_error();
      }
      while (!output.empty())
      {
        if (auto result = ensure_window(offset); std::holds_alternative<Error>(result))
        {
          return std::get<Error>(std::move(result));
        }
        const auto delta = static_cast<std::size_t>(offset - _windowBegin);
        const auto count = std::min(output.size(), _bytes.size() - delta);
        std::copy_n(_bytes.begin() + delta, count, output.begin());
        offset += count;
        output = output.subspan(count);
      }
      return std::monostate{};
    }

    OperationResult load_words(std::uint64_t offset, std::span<std::uint64_t> output)
    {
      if (offset % sizeof(std::uint64_t) != 0 ||
          output.size() > std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t) ||
          !contains_range(_limit, offset, std::uint64_t{output.size()} * sizeof(std::uint64_t)))
      {
        return range_error();
      }
      while (!output.empty())
      {
        if (auto result = ensure_window(offset); std::holds_alternative<Error>(result))
        {
          return std::get<Error>(std::move(result));
        }
        const auto delta = static_cast<std::size_t>(offset - _windowBegin);
        const auto count = std::min(output.size(), (_bytes.size() - delta) / sizeof(std::uint64_t));
        for (std::size_t index = 0; index < count; ++index)
        {
          auto word = AtomicWord::bind(_bytes, delta + index * sizeof(std::uint64_t));
          if (const auto* error = std::get_if<Error>(&word))
          {
            return *error;
          }
          output[index] = std::get<AtomicWord>(word).load_acquire();
        }
        offset += count * sizeof(std::uint64_t);
        output = output.subspan(count);
      }
      return std::monostate{};
    }

    Result<std::uint64_t> load_acquire(std::uint64_t offset)
    {
      std::array<std::uint64_t, 1> value{};
      if (auto result = load_words(offset, value); std::holds_alternative<Error>(result))
      {
        return std::get<Error>(std::move(result));
      }
      return value[0];
    }

  private:
    static Error range_error()
    {
      return Error{ErrorCode::IoError, "Metadata read exceeds its recovered range", "mapped_window"};
    }

    OperationResult ensure_window(std::uint64_t offset)
    {
      if (!_bytes.empty() && offset >= _windowBegin &&
          contains_range(_bytes.size(), offset - _windowBegin, 1))
      {
        return std::monostate{};
      }
      _bytes = {};
      _mapping = {};
      const auto begin = offset - offset % _windowBytes;
      const auto length = std::min(_windowBytes, _limit - begin);
      auto mapping = _file->map_read_write(begin, length);
      if (const auto* error = std::get_if<Error>(&mapping))
      {
        return *error;
      }
      _mapping = std::get<MappedRegion>(std::move(mapping));
      auto bytes = _mapping.writable_bytes();
      if (const auto* error = std::get_if<Error>(&bytes))
      {
        return *error;
      }
      _bytes = std::get<std::span<std::byte>>(bytes);
      _windowBegin = begin;
      return std::monostate{};
    }

    static constexpr std::uint64_t _windowBytes = 64 * 1024;
    NativeFile* _file{};
    std::uint64_t _limit{};
    std::uint64_t _windowBegin{};
    MappedRegion _mapping;
    std::span<std::byte> _bytes;
  };
} // namespace angaria::detail::mapped
