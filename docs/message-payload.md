# Binary payloads

Milestone 3 implements `BinaryPayloadCodec` in [angaria/payload.h](../include/angaria/payload.h): step 3.1 introduced scalar values, and step 3.3 adds basic logical Fields and int8. A **payload** is the application data carried by a message. The codec converts the values of one exact registered schema into bytes and back. Step 3.4 adds the [portable message envelope](message-record.md); storage, publication, and delivery follow their implementation steps.

The [ninth primer example](../examples/primer/09_payload.cpp) registers two Quote versions. The [tenth example](../examples/primer/10_logical_fields.cpp) uses TOML version 2 and logical values, including an exact caller-owned output buffer. Both operate without a Queue. See the [milestone 3 review plan](milestones/milestone-3.md) for the remaining work.

## Select an exact schema

`BinaryPayloadCodec::from_registry(registry, schema, maxPayloadBytes)` captures one consistent Registry snapshot and resolves the requested `SchemaId`. `from_snapshot(snapshot, schema, maxPayloadBytes)` performs the same compilation using an owning `RegistrySnapshot` supplied by the caller. This second factory also supports a previously captured historical definition set.

The selected message descriptor supplies an ordered list of full Field IDs. Each occurrence resolves to a Field definition, including repeated occurrences of the same ID. The codec owns the resolved descriptors, schema identity, and resulting payload size. It retains no Session or Task handle and keeps its original meaning after new schema versions are registered.

Factories require a nonzero message type and version and select that exact pair. They do not substitute the newest schema version. The selected schema and its referenced Fields must be present and unambiguous. Field identity, name/documentation text, base type, and declared width are validated; a caller-created snapshot is not permission to bypass those checks. Compiling supplied definitions does not authenticate their Registry origin or recreate a Session.

`maxPayloadBytes` is an explicit byte budget for the compiled payload. The sum of the ordered Field widths must fit both this budget and the process's supported allocation range. A zero-byte limit can compile an empty control-message schema. A zero-byte limit cannot compile a schema containing any of the scalar Fields below.

## Layout and supported values

Values appear consecutively in schema order. There are no alignment gaps, host-structure padding, Field ID tags, length prefixes, or terminators between them.

| Registered Field base | `PayloadValue` alternative | Bytes |
| --- | --- | ---: |
| `BASE_CHAR` | `char` | 1 |
| `BASE_BYTE` | `std::uint8_t` | 1 |
| `BASE_INT8` | `std::int8_t` | 1 |
| `BASE_INT16` | `std::int16_t` | 2 |
| `BASE_UINT16` | `std::uint16_t` | 2 |
| `BASE_INT32` | `std::int32_t` | 4 |
| `BASE_UINT32` | `std::uint32_t` | 4 |
| `BASE_INT64` | `std::int64_t` | 8 |
| `BASE_UINT64` | `std::uint64_t` | 8 |
| `BASE_FLOAT32` | `float` | 4 |
| `BASE_FLOAT64` | `double` | 8 |

Multi-byte values use **little-endian** order: the least significant byte comes first. Signed integers use their fixed-width two's-complement representation. Floating-point values use IEEE 754 binary32 or binary64 bits; encoding preserves negative zero, infinities, and NaN bit patterns. A `char` represents one raw eight-bit unit, without character-set conversion. One `char` does not represent an arbitrary Unicode character.

The table above describes ordinary scalar Fields, whose logical kind is `None`. Step 3.3 adds these distinct alternatives when registered logical metadata requires them:

| Logical kind | `PayloadValue` alternative | Constraint |
| --- | --- | --- |
| Boolean | `bool` | Exactly `00` or `01`; a raw byte is not a Boolean input. |
| Bits | `BitFieldValue{unsigned_value}` | The value fits the registered 8/16/32/64-bit unsigned width. |
| Enumeration | `EnumValue{typed_integer}` | The exact underlying width/sign matches and the number has a registered label. For byte storage, use `EnumValue{std::uint8_t{1}}`. |
| FixedAscii | `FixedAsciiValue{string}` | Exactly the declared number of bytes in `00..7f`, preserving NUL and caller-supplied padding. |

There is no implicit wrapping, padding, truncation, or logical inference from names. The [logical Field contract](logical-fields.md) defines the metadata, enum canonicalization, and required file profile. Scalar and logical Fields share the same sequential packed layout; the exact schema supplies their meaning. `ScalarValue` remains an alias for `PayloadValue` so earlier scalar examples still compile.

For Quote v1 with `PriceTicks` as int64 and `Quantity` as uint64, the values `-2` and `7` produce exactly 16 bytes:

```text
fe ff ff ff ff ff ff ff 07 00 00 00 00 00 00 00
```

Appending a uint32 `Instrument` Field to Quote v2 produces a 20-byte payload. Registering that new immutable schema does not change the earlier 16-byte layout.

Payload bytes do not contain their `SchemaId`. Keep the exact message type, version, and source Session with the bytes in the surrounding application context. Decoding with a different same-sized schema can produce different values; the bytes alone cannot identify that mistake. Step 3.4's [message record](message-record.md#decode-with-exact-schema-context) supplies this context and checks it before typed binary decoding.

## Encode, decode, and lifetime

`encode(values)` accepts a span of `PayloadValue` alternatives in Field order. An initializer-list overload also permits `codec.encode({values...})`. The number of values and each exact C++ alternative must match the compiled schema. For example, a uint64 Field needs `std::uint64_t{7}`; `std::int64_t{7}` is a type mismatch even though the number is positive. The codec performs no implicit numeric conversion.

Encoding returns an owning `std::vector<std::byte>`. Decoding accepts a byte span of exactly `payload_size()` bytes and returns an owning `std::vector<PayloadValue>` in the same Field order. Truncated buffers and extra trailing bytes are errors. An empty schema encodes and decodes empty vectors.

`encode_into(values, output)` writes into a caller-owned byte span whose length must equal `payload_size()`. It validates and constructs temporary encoded bytes before copying the complete result into output. Invalid values, wrong output length, unavailable memory, and invalid codec state leave output unchanged. The convenience APIs use the same compiled byte cap and never construct a partially accepted message or publication.

The returned vectors remain valid independently of the codec and Registry. `fields()` returns a borrowed span of the codec's ordered Field descriptors. Keep the codec alive while reading that span, and reacquire it after moving or replacing the codec.

All encoding and decoding here occurs in application memory. It does not append a publication, reserve a queue entry, attach a Task, or send a message.

## Errors and scope

| Result | Meaning |
| --- | --- |
| `InvalidArgument` | Invalid or ambiguous selected metadata, an invalid declared width, a value count/type mismatch, or a payload whose byte count is not exact. |
| `NotFound` | The exact schema or one of its referenced Fields is absent. |
| `UnsupportedPolicy` | A recognized Field representation is not implemented by this codec, or the required native floating-point representation is unavailable. |
| `CapacityExceeded` | The compiled payload exceeds its byte/allocation bound, or process memory is insufficient. |

Errors carry a path and message. Value errors identify the payload input, including an indexed value when applicable; exact-length failures use `payload.size`, and the configured byte-bound failure uses `payload.capacity`.

The Registry factory can also propagate errors from its snapshot operation, such as invalid ownership or failure to validate the Session. A payload codec does not bypass those Registry checks.

Step 3.3 supports the eleven scalar bases and four logical kinds above. Compound Fields, pools, and additional payload formats follow later milestones. Recognition of another Field base in persisted metadata does not implement its payload encoding. The legacy [field templates](../include/angaria/field.h) and v0 queue demo have a separate earlier format and do not define this codec's layout.
