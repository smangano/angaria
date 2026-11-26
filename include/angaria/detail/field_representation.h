#pragma once

#include "angaria/types.h"

#include <cstdint>
#include <optional>

namespace angaria::detail
{
  // Storage facts only. Logical Field rules, capability admission, and the
  // caller's diagnostic/error policy remain with the owning operation.
  struct IntegerShape
  {
    bool _signed{};
    unsigned _bytes{};

    bool operator==(const IntegerShape&) const = default;
  };

  constexpr std::optional<IntegerShape> integer_shape(EFieldBase base)
  {
    switch (base)
    {
    case EFieldBase::BASE_INT8: return IntegerShape{true, 1};
    case EFieldBase::BASE_BYTE: return IntegerShape{false, 1};
    case EFieldBase::BASE_INT16: return IntegerShape{true, 2};
    case EFieldBase::BASE_UINT16: return IntegerShape{false, 2};
    case EFieldBase::BASE_INT32: return IntegerShape{true, 4};
    case EFieldBase::BASE_UINT32: return IntegerShape{false, 4};
    case EFieldBase::BASE_INT64: return IntegerShape{true, 8};
    case EFieldBase::BASE_UINT64: return IntegerShape{false, 8};
    default: return std::nullopt;
    }
  }

  constexpr std::optional<std::uint32_t> scalar_width(EFieldBase base)
  {
    if (const auto integer = integer_shape(base))
    {
      return integer->_bytes;
    }
    switch (base)
    {
    case EFieldBase::BASE_CHAR: return 1;
    case EFieldBase::BASE_FLOAT32: return 4;
    case EFieldBase::BASE_FLOAT64: return 8;
    default: return std::nullopt;
    }
  }
} // namespace angaria::detail
