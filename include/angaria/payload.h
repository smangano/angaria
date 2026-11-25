#pragma once

#include "angaria/descriptors.h"
#include "angaria/results.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace angaria
{
  class Registry;

  using IntegerValue = std::variant<std::int8_t, std::uint8_t, std::int16_t, std::uint16_t,
                                     std::int32_t, std::uint32_t, std::int64_t, std::uint64_t>;

  struct BitFieldValue
  {
    std::uint64_t _value{};
    bool operator==(const BitFieldValue&) const = default;
  };

  struct EnumValue
  {
    IntegerValue _value{};
    bool operator==(const EnumValue&) const = default;
  };

  struct FixedAsciiValue
  {
    std::string _text;
    bool operator==(const FixedAsciiValue&) const = default;
  };

  // Alternatives distinguish both exact scalar widths and logical meaning.
  // A byte, Boolean, bit field, and enum cannot silently stand in for each other.
  using PayloadValue = std::variant<char, std::uint8_t, std::int16_t, std::uint16_t,
                                     std::int32_t, std::uint32_t, std::int64_t,
                                     std::uint64_t, float, double, std::int8_t, bool,
                                     BitFieldValue, EnumValue, FixedAsciiValue>;
  using ScalarValue = PayloadValue;

  // An owning, resolved exact schema for binary payloads. Bytes follow
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

    Result<std::vector<std::byte>> encode(std::span<const PayloadValue> values) const;
    Result<std::vector<std::byte>> encode(std::initializer_list<PayloadValue> values) const;
    // The output must have exactly payload_size() bytes. Any error leaves it
    // unchanged; successful encoding replaces its complete contents.
    OperationResult encode_into(std::span<const PayloadValue> values, std::span<std::byte> output) const;
    Result<std::vector<PayloadValue>> decode(std::span<const std::byte> payload) const;

  private:
    BinaryPayloadCodec(SchemaId schema, std::vector<FieldDescriptor> fields, std::uint64_t payloadSize);
    void check_valid() const;

    SchemaId _schema;
    std::vector<FieldDescriptor> _fields;
    std::uint64_t _payloadSize{};
  };
} // namespace angaria
