#pragma once

#include "angaria/identities.h"

#include <cstddef>
#include <iomanip>
#include <iostream>
#include <locale>
#include <span>
#include <sstream>
#include <string>

namespace primer
{
  // Keep the lessons' byte dumps and opaque Session identity display identical.
  // These are presentation helpers, independent of the binary record codecs.
  inline void print_hex(std::span<const std::byte> bytes)
  {
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
      if (index != 0)
      {
        std::cout << ' ';
      }
      std::cout << std::hex << std::setfill('0') << std::setw(2)
                << std::to_integer<unsigned>(bytes[index]);
    }
    std::cout << std::dec << std::setfill(' ') << '\n';
  }

  inline std::string session_text(angaria::SessionId session)
  {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::hex << std::setfill('0') << std::setw(16) << session._high
           << std::setw(16) << session._low;
    return output.str();
  }
} // namespace primer
