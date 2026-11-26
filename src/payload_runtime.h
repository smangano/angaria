#pragma once

#include "runtime_state.h"

#include <cstdint>

namespace angaria::detail
{
  // Resolve owning scalar/logical decoding metadata under the caller's gate.
  // This copies definitions, never application payload bytes or Task ownership.
  inline BinaryPayloadCodec runtime_payload_codec(RuntimeAccess& access, const LoadedSchema& schema,
                                                     std::uint64_t maxPayloadBytes)
  {
    RegistrySnapshot snapshot;
    snapshot._messages.push_back({schema._definition._descriptor, schema._definition._registration});
    for (const auto& field : runtime_fields(access))
    {
      snapshot._fields.push_back({field._definition._descriptor, field._definition._registration});
    }
    return runtime_value(BinaryPayloadCodec::from_snapshot(snapshot,
        schema._definition._descriptor._schema, maxPayloadBytes));
  }
} // namespace angaria::detail
