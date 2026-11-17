#pragma once

#include "angaria/types.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace angaria
{
  inline bool set_var_char(VARCHAR_t& dest, const std::string& src)
  {
    const auto size = std::min<std::size_t>(dest._length, src.size());
    if (size != 0)
    {
      std::memcpy(dest._text, src.data(), size);
    }
    return size == src.size();
  }
} // namespace angaria
