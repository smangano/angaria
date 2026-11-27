#pragma once

#include "runtime_state.h"

#include <cstdint>
#include <span>

namespace angaria::detail
{
  // Reuse the selected schema and complete checked Fields from one unchanged
  // metadata context. This performs no live reads and carries no Task authority;
  // the codec owns its dependencies rather than borrowing either input.
  inline BinaryPayloadCodec runtime_payload_codec(const LoadedSchema& schema,
                                                     std::span<const LoadedField> fields,
                                                     std::uint64_t maxPayloadBytes)
  {
    RegistrySnapshot snapshot;
    snapshot._messages.push_back({schema._definition._descriptor, schema._definition._registration});
    for (const auto& field : fields)
    {
      snapshot._fields.push_back({field._definition._descriptor, field._definition._registration});
    }
    return runtime_value(BinaryPayloadCodec::from_snapshot(snapshot,
        schema._definition._descriptor._schema, maxPayloadBytes));
  }

  // Generic callers still load and validate current Fields under their access.
  inline BinaryPayloadCodec runtime_payload_codec(RuntimeAccess& access, const LoadedSchema& schema,
                                                     std::uint64_t maxPayloadBytes)
  {
    const auto fields = runtime_fields(access);
    return runtime_payload_codec(schema, fields, maxPayloadBytes);
  }
} // namespace angaria::detail
