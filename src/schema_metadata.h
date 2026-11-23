#pragma once

#include "angaria/descriptors.h"
#include "angaria/results.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace angaria::detail::mapped
{
  // Owning metadata values; the portable bodies use explicit format-2 fields,
  // logical slices, and little-endian integers rather than native structures.
  struct FieldDefinition
  {
    FieldDescriptor _descriptor;
    RegistrationMetadata _registration;
  };

  struct SchemaDefinition
  {
    MessageDescriptor _descriptor;
    RegistrationMetadata _registration;
  };

  // Pure metadata preflight. Native support/size policy and Registry reference
  // resolution are checked by the runtime, not by these portable codecs.
  OperationResult validate_field_descriptor(const FieldDescriptor& descriptor);
  OperationResult validate_message_descriptor(const MessageDescriptor& descriptor);
  Result<std::vector<std::byte>> encode_field_definition(const FieldDefinition& definition,
                                                        std::uint64_t recordOffset);
  Result<FieldDefinition> decode_field_definition(std::span<const std::byte> immutableBody,
                                                  std::uint64_t recordOffset);
  Result<std::vector<std::byte>> encode_schema_definition(const SchemaDefinition& definition,
                                                         std::uint64_t recordOffset);
  Result<SchemaDefinition> decode_schema_definition(std::span<const std::byte> immutableBody,
                                                    std::uint64_t recordOffset);
} // namespace angaria::detail::mapped
