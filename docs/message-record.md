# Canonical message records

Milestone 3, step 3.4, framework version `0.3.4`, implements `MessageRecordCodec` in [angaria/message_record.h](../include/angaria/message_record.h). A **message record** is a portable header followed by application payload bytes. The header identifies the exact schema and producer publication, states the payload format and byte count, and carries the initial publication time and application flags. The [eleventh primer program](../examples/primer/11_message_record.cpp) builds and decodes one record using the existing binary payload codec.

These operations work in application memory. They do not send, publish, admit a destination, append a native Publication, or create a delivery receipt. `Task::next_publication_id()` can supply an issued identity, but issuance alone is not acceptance. A pure codec checks the shape of an identity; it cannot authenticate its origin, current claim, issued counter, or admission state.

## Header meaning and identity

`MessageHeader` owns the following process-local values. Its C++ layout is not the byte format.

| Member | Meaning |
| --- | --- |
| `_schema` | Exact nonzero message type and schema version. There is no newest-version substitution. |
| `_format` | A known `PayloadFormat`: Binary, Json, Xml, Csv, or Text. |
| `_publication` | Full `PublicationId`: Session identity, producer Task, producer incarnation, and producer-local sequence. |
| `_publicationTime` | Initial publication/enqueue UTC time, as signed nanoseconds since the Unix epoch. The caller supplies it to this codec. |
| `_flags` | All 32 application flag bits, uninterpreted by the codec. |
| `_payloadBytes` | Exact application payload byte count, excluding the fixed header. |

Message type, version, producer Task, incarnation, and sequence must be nonzero. The 128-bit Session identity must not be all zero; either half may individually be zero. All signed 64-bit UTC values, including epoch zero and negative times, are valid, as are all uint32 flag values.

An initial publication time is distinct from each destination's acceptance/enqueue time: a later partial retry can accept another destination later. `DeliveryView::_enqueuedAt` remains destination-specific. Dequeue, acknowledgment, release, and loss metadata belong to the destination [receipts](mapped-format.md#rings-publications-and-receipts), outside this immutable header. Future native publication operations establish their authoritative times under their own acceptance contract; serializing a caller-supplied timestamp does not establish those events.

## Encoding version 1

All integers are explicitly little-endian. Signed UTC time uses 64-bit two's-complement bits. The fixed header is **80 bytes**. Its magic and encoding version are independent of framework, TOML, schema, and native mapped-format versions.

| Offset | Bytes | Value |
| ---: | ---: | --- |
| 0 | 8 | Magic `ANGAMSG\0`, bytes `41 4e 47 41 4d 53 47 00`. |
| 8 | 2 | Record encoding version, `1`. |
| 10 | 2 | Header byte count, `80`. |
| 12 | 2 | Payload format: `1` binary, `2` JSON, `3` XML, `4` CSV, `5` text. |
| 14 | 2 | Producer Task ID. |
| 16 | 2 | Message type. |
| 18 | 2 | Reserved, zero. |
| 20 | 4 | Schema version. |
| 24 | 4 | Application flags. |
| 28 | 4 | Reserved, zero. |
| 32 | 8 | Session identity, high word. |
| 40 | 8 | Session identity, low word. |
| 48 | 8 | Producer incarnation. |
| 56 | 8 | Producer publication sequence. |
| 64 | 8 | Initial publication UTC nanoseconds. |
| 72 | 8 | Application payload bytes. |
| 80 | As declared | Application payload, with no trailing padding. |

The total byte count is `80 + payload_bytes`, checked for arithmetic and native-container overflow. A zero-byte control payload still needs the complete 80-byte header. Encoders never copy native structs or assume an aligned input address. Reserved bytes are always zero; readers reject nonzero values.

`MessageRecordCodec::header_bytes` and `encoding_version` expose these constants. `HeaderBytes` is the fixed-size byte array used by the header encoder. A different version or header layout requires explicit support; a reader never guesses at extra fields or silently drops unknown semantics.

## Bounds, construction, and reading

Every header/record encode or decode takes a mandatory `maxRecordBytes` budget. It includes **both header and payload**, so a 16-byte quote requires at least 96 bytes. A very large budget is a ceiling, not a request to allocate that many bytes. Lengths, identity shape, known format, reserved bytes, and budget/native bounds are checked before copying payload data.

| Operation | Result and boundary |
| --- | --- |
| `encode_header(header, cap)` | Fixed `HeaderBytes`; validates the declared total against the cap without needing payload storage. |
| `decode_header(bytes, cap)` | Owning `MessageHeader` from **exactly 80 bytes**. Validates the announced total before a reader allocates/reads the payload. It does not prove those payload bytes exist. |
| `encode(header, payload, cap)` | Owning byte vector. The declared payload count must equal the supplied byte span. |
| `encode_into(header, payload, output, cap)` | `OperationResult`; output must have the exact total size. Errors leave every caller output byte unchanged. Overlap between payload input and output is supported through a temporary encoding. |
| `decode_view(record, cap)` | `MessageRecordView` with an owning header value and a borrowed payload span. Requires exactly one complete record. |
| `decode(record, cap)` | `MessageRecord` with its own header and payload vector, independent of the encoded input's lifetime. |

Whole-record decoding rejects truncated payloads, trailing bytes, concatenated records, and inconsistent lengths. There is no implicit stream resynchronization. Step 3.5's [capture reader](capture-inspection.md) reads 80 bytes, validates its header and announced size against record/aggregate budgets, then reads that payload as one record.

The codec recognizes all five stable format codes as **opaque application bytes**. It does not validate JSON/XML/CSV/text syntax or implement serializers for those formats. Recognizing their framing also does not enable future Queue policies or payload encoders. Binary value validation is a separate operation below.

## Decode with exact schema context

`MessageRecordCodec::decode_binary(record, codec, schemaSession)` uses an existing `BinaryPayloadCodec`. It requires binary format, an exact matching schema type/version, and a header Session identity matching the independently supplied `schemaSession`. The expected Session comes from the source Registry's Session, not from the record being checked. This prevents accidentally using a same-numbered schema from another Session.

For example, retain the source Session identity and payload codec while setting up a program. After decoding record bytes, pass that retained identity and codec to `decode_binary`. Do not use the untrusted record's own Session field as the expected identity: comparing it with itself would not check the source of the definitions.

The helper also validates manually constructed `MessageRecordView` metadata and declared byte count before interpreting payloads. The payload codec enforces exact byte size and scalar/logical representation constraints, including Boolean canonical bytes, closed enum values, and ASCII bounds. A different same-sized schema version is still rejected before values are read.

Neither this explicit context nor `from_snapshot` authenticates a caller-provided definition set. Applications must select the correct Registry/retained definitions for that Session. No cryptographic integrity or live ownership check is implied by successful decoding. Step 3.5's [generic inspector](capture-inspection.md) retains this independent Session context while resolving exact schemas from owning definitions.

## Ownership and existing message views

`MessageRecordView` owns its header and borrows only the payload bytes. Keep the encoded input alive and unchanged while reading the view. Moving, replacing, or destroying that backing storage can invalidate its span. `decode` produces an independent owning copy instead.

`MessageRecord::view()` borrows its owning vector and copies its header; the record must outlive that view. Calling it on a temporary is disabled. `MessageRecordView::message()` returns the existing `MessageView` containing only schema, format, payload, and flags. Its payload remains borrowed. That adapter does not carry receipt state or establish a send; delivery APIs remain later work.

## Relationship to native Session storage

This portable record is distinct from the native [64-byte arena record envelope](mapped-format.md#arena-records-and-definitions) and [128-byte Publication body](mapped-format.md#rings-publications-and-receipts). Those structures include native storage references, routing snapshots, and receipts. A portable message header contains no offsets into a Session and no destination state.

Native formats **2.0 and 2.1, their feature masks, ABI/protocol versions, and Publication layout are unchanged by step 3.4**. Future native storage maps the same logical header values from the Publication body plus its Session identity. Export can reconstruct this self-contained record. The Publication's payload `ByteSlice` continues to point to application payload bytes alone; it must not nest this portable header inside the schema payload. Native storage/export implementation remains with its roadmap steps.

## Errors and qualification

| Result | Meaning |
| --- | --- |
| `InvalidArgument` | Bad magic, wrong fixed header size, nonzero reserved bytes, unknown payload format, invalid identity/schema, declared-length overflow, incomplete/extra bytes, mismatched supplied payload/output sizes, or a zero expected schema Session. |
| `UnsupportedPolicy` | Unsupported record encoding version, or asking the binary helper to interpret another known payload format. |
| `CapacityExceeded` | Declared header-plus-payload size exceeds the explicit/native bound, or process memory is insufficient. |
| `SchemaConflict` | The binary helper's expected Session or exact schema differs from the record. |
| `InvalidState` | The supplied payload codec has been moved from. |

Diagnostics identify the failing `message_record.*` component; delegated payload failures retain their `payload.*` paths. Pure byte codecs do not use `IoError` for malformed in-memory data. Native file errors belong to their I/O operations.

Tests use independently authored golden bytes, identity/time/flag boundaries, every stable format, precise malformed/unsupported cases, header-only preflight, exact record caps, overlap and unchanged-output guarantees, owning/borrowed lifetimes, and exact Session/schema matching. The runnable primer demonstrates actual Registry definitions and ID issuance while keeping local record construction distinct from admission and delivery.

[Binary payloads](message-payload.md) · [Public API](api.md) · [Milestone 3 plan](milestones/milestone-3.md)
