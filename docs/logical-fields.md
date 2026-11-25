# Basic logical Field design

**Review contract for milestone 3, step 3.2, framework version `0.3.2`.** The representations below are planned for step 3.3. This PR does not extend `FieldDescriptor`, the TOML loader, persisted definitions, or `BinaryPayloadCodec`. The current implementation still uses configuration version 1, mapped format 2.0, and the ten [binary scalar representations](message-payload.md#layout-and-supported-values).

The [Wiki's Fields section](https://github.com/smangano/angaria/wiki/Angaria#fields) calls for booleans, bit fields, user-defined enumerations, and fixed-length ASCII sequences. A **logical representation** describes what a Field's bytes mean and which values are valid. Its **storage base** describes the underlying integer or character bytes. For example, a Boolean and a byte both occupy one byte, but only a Boolean restricts that byte to zero or one.

This contract separates logical meaning from the existing base codes. Codes `0..25` retain their established meaning; booleans, bit fields, and enumerations do not reuse a code with a different meaning. The future descriptor adds owning, tagged logical metadata. Ordinary scalar Fields have no logical metadata. Registration, snapshots, historical inspection, and codec compilation must preserve and validate it together with the storage base and declared byte size.

A Field's name never supplies missing type information: an untagged byte named `TradingEnabled` remains an ordinary byte. An untagged `BASE_CHARS` definition remains a recognized unsupported representation rather than acquiring ASCII rules by inference. Arbitrary attribute dictionaries and optional units/range/regex constraints retain their later milestone scope.

## Representations and bounds

| Logical representation | Storage base and declared size | Accepted values and bytes |
| --- | --- | --- |
| Boolean | `BASE_BYTE`, 1 byte | An explicit Boolean value; false encodes `00`, true encodes `01`. Decoding any other byte fails. Ordinary byte values remain a distinct input type. |
| Basic bit field | `BASE_BYTE`, `BASE_UINT16`, `BASE_UINT32`, or `BASE_UINT64`; 1, 2, 4, or 8 bytes | Exactly 8, 16, 32, or 64 bits respectively. A distinct bit-field value holds an unsigned integer within that width. Bit zero is the least significant bit. Multi-byte values are little-endian. No C++ bit-field object layout is serialized. |
| Enumeration | One of the integer bases listed below; its fixed width | A distinct enum value with the underlying integer's exact signedness and width. The number must have one registered label. Bytes use the underlying integer's little-endian representation. |
| Fixed ASCII | `BASE_CHARS`, declared size `N`, where `1 <= N <= UINT32_MAX` | An owning character sequence of exactly `N` bytes, each in `00..7f`. Input length counts bytes. All bytes, including NUL (`00`) and spaces, are preserved. |

The Wiki also lists signed 8-bit integers, which the current scalar codec lacks. Step 3.3 adds `BASE_INT8` at the new explicit code **26**, with `std::int8_t` values and one-byte two's-complement encoding. Existing codes remain unchanged. Enum storage may then be `byte` (unsigned 8-bit), `int8`, `int16`, `uint16`, `int32`, `uint32`, `int64`, or `uint64`. `char`, floating point, Boolean, another enumeration, and pooled representations cannot be enum storage bases. Signed bounds are `-2^(bits-1)..2^(bits-1)-1`; unsigned bounds are `0..2^bits-1`.

Bit-field widths follow the Wiki's discrete widths. They are independent Fields, without packing adjacent Fields or naming individual bits in this step. Widths 128 and 256 and additional bit metadata remain milestone 9 work.

Fixed ASCII has no automatic terminator, padding, trimming, truncation, or Unicode conversion. A caller wanting `IBM` in a six-byte Field supplies `IBM` followed by three chosen ASCII bytes. Supplying three bytes to a six-byte Field fails. A generic inspector must escape control bytes so embedded NULs and line breaks remain visible. Variable-length UTF-8 strings, symbols, and shared pools remain milestone 9 work.

## Enumeration definitions and identity

An enumeration is a **closed set**: both encoding and decoding reject numbers absent from its registered mapping. A label identifies one numeric value, and a numeric value has one label. There are no aliases, unnamed values, implicit ordinal assignments, or automatic conversions from label text during binary encoding. Application adapters for C++ `enum` types supply their explicit underlying values.

Definitions require at least one entry. Labels follow the existing nonempty UTF-8 metadata-name rules, are case-sensitive, and cannot contain NUL. Labels and numeric values must each be unique. Validation checks every value against the storage base before conversion. Entries are normalized into ascending numeric order using the base's signedness. Input declaration order is not semantic; equivalent reordered mappings re-register idempotently and snapshots expose the canonical order. Message Field order and repetition retain their existing meaning.

Logical kind, storage base, size, enum labels/values, name, and documentation are all part of immutable Field identity. A different mapping under an existing Field ID or name returns `Conflict`, even if the numeric width is unchanged. Adding an enumerator, renaming a label, changing ASCII size, or changing a byte into a Boolean requires a **new Field ID and name**, followed by a new message-schema version referencing it. A new schema version alone does not permit changing a Field. Retained definitions and already compiled codecs keep the old meaning.

## Planned TOML version 2

Step 3.3 introduces `config_version = 2` while continuing to accept existing version-1 scalar configurations. Version 2 retains the existing Session, capacity, topology, and message keys. It adds the Field forms below and the `int8` scalar type. Unknown or inapplicable keys are errors; the loader must not ignore a logical constraint.

| Field `type` | Additional required keys | Effective metadata |
| --- | --- | --- |
| `bool` | None | Boolean logical kind, `BASE_BYTE`, size 1. |
| `bits` | `bits`, integer 8, 16, 32, or 64 | Bit-field logical kind; base and size derived from width. |
| `enum` | `underlying`, integer type name; `enumerators`, nonempty array of tables | Enum logical kind; size derived from the base; each entry has only `name` and `value`. |
| `chars` | `size`, integer in `1..UINT32_MAX` | Fixed-ASCII logical kind, `BASE_CHARS`, exact byte size. |

All forms also require the existing `name` and full `0xEE`-prefixed `id`, with optional `doc`. For example, `size` on a Boolean, `bits` on an enum, or `underlying` on a scalar is invalid. Version 1 does not acquire these keys or logical meanings; using a version-2 declaration in it produces `InvalidConfiguration` with the offending key/type and required configuration version.

TOML integers are signed 64-bit values. An enum entry's `value` therefore accepts either a TOML integer or a canonical decimal string, so the complete uint64 range is expressible. Decimal strings have the grammar `0`, a nonzero digit followed by digits, or `-` followed by a nonzero digit and digits. Whitespace, `+`, leading zeros, `-0`, decimal points, exponents, and hex strings are invalid. Negative values are invalid for unsigned storage. All other numeric settings retain the current strict TOML-integer rules. Converters can emit decimal strings for every enumerator; both accepted forms normalize to the same numeric definition.

The following is a **design fragment**, not a runnable configuration. A complete file also needs the current `session` and `limits` tables. The current loader rejects `config_version = 2`.

```toml
config_version = 2

[[fields]]
name = "TradingEnabled"
id = 0xEE000010
type = "bool"

[[fields]]
name = "OrderFlags"
id = 0xEE000011
type = "bits"
bits = 16

[[fields]]
name = "Side"
id = 0xEE000012
type = "enum"
underlying = "byte"
enumerators = [
  { name = "Buy", value = 1 },
  { name = "Sell", value = "2" }
]

[[fields]]
name = "InstrumentCode"
id = 0xEE000013
type = "chars"
size = 6

[[fields]]
name = "ExternalStatus"
id = 0xEE000014
type = "enum"
underlying = "uint64"
enumerators = [{ name = "AllBitsSet", value = "18446744073709551615" }]

[[messages]]
name = "OrderSettings"
type = 2
version = 1
fields = ["TradingEnabled", "OrderFlags", "Side", "InstrumentCode"]
```

cpptoml at the project's pinned revision remains the sole loader. YAML, JSON, and XML converters must preserve the enum's full numeric range, ASCII size, logical kind, and exact Field/schema identities when producing TOML.

## Planned persisted metadata

This layout is a design for format 2.1, not bytes written by step 3.2. Keep the Field body's existing 88-byte fixed prefix and explicit storage-base codes. Ordinary scalar definitions retain flags zero, an empty attributes slice, and their exact earlier body layout. An `int8` scalar also has no attributes, but uses new base code 26 and the required compatibility profile below.

Every logical Field sets **Field flag `0x0001`**, meaning required logical metadata. Its `attributes:ByteSlice` owns the following block immediately after the 88-byte prefix, before the Field name and documentation. This is a Field-local flag; it is separate from the Session's feature mask.

| Attribute-relative offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 2 | Attribute encoding version, `1`. |
| 2 | 2 | Logical kind: `1` Boolean, `2` bit field, `3` enum, `4` fixed ASCII. |
| 4 | 4 | Reserved, zero. |
| 8 | 8 | Enum entry count (`u64`), present only for kind 3. |
| 16 | `24 * count` | Enum entries, present only for kind 3: numeric bits (`u64`) then label (`ByteSlice`, 16 bytes). |

Boolean, bit-field, and fixed-ASCII blocks are exactly eight bytes; the base/size determine their widths. An enum block appends its label bytes immediately after its entry array in canonical entry order, without terminators or gaps. Its attribute byte count is exactly `16 + 24 * count + total_label_bytes`. Label slices use absolute logical offsets as in the existing mapped format, point only into this block, and cover those bytes consecutively. Signed enum numeric values are sign-extended to 64-bit two's-complement bits; unsigned values are zero-extended. A decoder rejects noncanonical extension bits and out-of-width values.

All integers are little-endian. Field name and documentation follow the complete attributes block using their existing slices; only the final record-allocation padding is zero-filled. Flags zero require an empty attributes slice. Flag `0x0001` requires a valid nonempty block. Unknown flags, kinds, versions, nonzero reserved bytes, invalid base/kind combinations, malformed slices, overlaps, gaps, or trailing attribute bytes fail live validation. An offline tool may report unsupported metadata as opaque only when its enclosing record is safely bounded; it must not decode it as an ordinary scalar.

Logical metadata, enum entry arrays, and labels consume immutable arena/log bytes, not symbol entries or the application string pool. Count/length multiplication and addition, native allocation limits, and the complete record/manifest/padding budget are checked before copying metadata or committing registration. There is no separate 16-bit enum-count limit. Entry and byte availability remain bounded by the Session's effective capacities and `max_log_bytes`.

## Compatibility boundary

Step 3.3 introduces **mapped format 2.1** and reserves required Session feature bit **`0x2`**, `basic_fields_v1`, for logical attributes and the int8 base. The existing journal bit is `0x1`; newly created 2.1 Sessions require mask `0x3`. The feature is selected at Session creation, even before a logical Field is registered. Both programmatic creation and creation from either accepted TOML version use that new profile. Configuration version and mapped format remain independent: a version-1 scalar TOML file can create a format-2.1 Session.

| Session | Step-3.3-capable library | Earlier format-2.0 library |
| --- | --- | --- |
| Existing format 2.0, journal feature only | Attach and retain scalar behavior; reject logical/int8 registration with `UnsupportedPolicy` and no state change. | Existing behavior. |
| New format 2.1, required mask `0x3` | Validate metadata and use supported scalar/logical Fields. | Reject unsupported format/features before recovery or mutation. |
| Unsupported format or unknown required feature | Reject before recovery or mutation. | Reject before recovery or mutation. |

A capable library must deliberately retain the 2.0 decoder/validation path; accepting a minor range without checking the per-version layout is insufficient. Format 2.1 requires `basic_fields_v1`; 2.0 cannot advertise it. Logical attributes or base code 26 in a 2.0 file are invalid stored data. The record envelope, ABI profile, synchronization version, Field IDs, and message-schema identity rules do not change merely because framework version advances.

There is no in-place upgrade, rewriting of retained definitions, or dynamic change to an existing Session's immutable feature words. Using new representations with old history requires a new Session and explicit application mapping; general migration tooling remains milestone 10 work. Already compiled scalar codecs and their captured descriptors retain their meaning independently of file attachment.

## Step 3.3 implementation and qualification

Implement descriptor metadata, its equality/canonicalization, TOML version 2, persisted codecs, profile negotiation, Registry/snapshot validation, payload values, and bounded construction together through focused review. If needed, split step 3.3 into smaller PRs while preserving these boundaries. Merely recognizing a logical kind must never advertise payload support.

| Failure | Required result |
| --- | --- |
| Malformed configuration, wrong TOML types, duplicate labels/values, out-of-range values | `InvalidConfiguration`, with an indexed Field/enumerator key path. |
| Malformed programmatic metadata or invalid payload value/bytes | `InvalidArgument`, identifying the Field/value and violated constraint. |
| Known representation unavailable in this build or Session profile | `UnsupportedPolicy`, before mutation. |
| Conflicting immutable Field ID/name | `Conflict`; schema identity conflicts retain `SchemaConflict`. |
| Entry-capacity exhaustion or codec byte/allocation limit | `CapacityExceeded`. |
| Complete registration allocation does not fit remaining log | `LogFull`, without a partial definition or Registry revision. |
| Malformed persisted definition | `IoError`, before exposing usable handles; compatible journal recovery may already have run. |
| Incompatible format or required feature | `IoError`, before attempting recovery writes. |

Qualification must cover independent golden metadata/payload bytes, unchanged earlier scalar bodies and codes, both configuration versions, uint64 extrema in enum strings, signed int8 extrema, exact ASCII lengths/control bytes, invalid Boolean bytes, discrete bit widths, closed-enum rejection, canonical re-registration, descriptor ownership after handle closure, historical schema decoding, and allocation/commit failures. Compatibility tests must prove older readers reject 2.1 before writes and capable readers preserve valid 2.0 behavior. Update the primer with runnable logical-Field programs only when those APIs work.

[Milestone 3 plan](milestones/milestone-3.md) · [Current configuration](configuration.md) · [Current mapped format](mapped-format.md) · [Programmer primer](primer.md)
