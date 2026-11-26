#pragma once

#include "angaria/identities.h"

#include <cstdint>

namespace angaria::detail
{
  // Stable keys and application-ID shape, independent of Registry existence.
  constexpr std::uint64_t schema_key(SchemaId schema)
  {
    return (std::uint64_t{static_cast<std::uint16_t>(schema._type)} << 32) | schema._version;
  }

  constexpr bool valid_field_id(std::uint32_t value)
  {
    return (value & 0xff000000u) == 0xee000000u;
  }

  constexpr bool valid_field_id(FieldId id)
  {
    return valid_field_id(static_cast<std::uint32_t>(id));
  }
} // namespace angaria::detail
