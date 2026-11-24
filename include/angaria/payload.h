#pragma once

#include "angaria/descriptors.h"
#include "angaria/results.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace angaria
{
  class Registry;

  // Each alternative identifies an exact declared Field base, not a value to
  // coerce into whichever numeric width the schema happens to require.
  using ScalarValue = std::variant<char, std::uint8_t, std::int16_t, std::uint16_t,
                                   std::int32_t, std::uint32_t, std::int64_t,
                                   std::uint64_t, float, double>;

  // An owning, resolved exact schema for binary scalar payloads. Bytes follow
  // Field order, without native padding or embedded Field/schema identities.
  class BinaryPayloadCodec
  {
  public:
    static Result<BinaryPayloadCodec> from_registry(const Registry& registry, SchemaId schema,
                                                     std::uint64_t maxPayloadBytes);
    static Result<BinaryPayloadCodec> from_snapshot(const RegistrySnapshot& snapshot, SchemaId schema,
                                                     std::uint64_t maxPayloadBytes);

    BinaryPayloadCodec(const BinaryPayloadCodec&) = default;
    BinaryPayloadCodec& operator=(const BinaryPayloadCodec& other);
    BinaryPayloadCodec(BinaryPayloadCodec&& other) noexcept;
    BinaryPayloadCodec& operator=(BinaryPayloadCodec&& other) noexcept;

    SchemaId schema() const;
    std::uint64_t payload_size() const;
    std::span<const FieldDescriptor> fields() const;

    Result<std::vector<std::byte>> encode(std::span<const ScalarValue> values) const;
    Result<std::vector<ScalarValue>> decode(std::span<const std::byte> payload) const;

  private:
    BinaryPayloadCodec(SchemaId schema, std::vector<FieldDescriptor> fields, std::uint64_t payloadSize);
    void check_valid() const;

    SchemaId _schema;
    std::vector<FieldDescriptor> _fields;
    std::uint64_t _payloadSize{};
  };
} // namespace angaria
