# Producer log format and private codecs

Milestone 4 step **4.31**, framework **0.4.33**, implements step 2 of the [independent messaging workstream](recorded-unacknowledged-delivery-plan.md#implementation-steps-for-review): local byte codecs for producer-log headers, cross-log references and immutable Publication records. The [protocol design](independent-messaging-protocol.md) assigns ownership and commit/recovery behavior.

**Current status — 0.4.39:** these codecs now underlie the [public recorded runtime](public-runtime-admission.md). The private [producer_log_format.h](../src/producer_log_format.h) remains an implementation detail; applications use the public API and [primer](primer.md). A pure decoded record/reference establishes representation, not live acceptance. Producer-owned append/recovery and connection/progress protocols establish those runtime guarantees.

## Representation and boundaries

Integers use little-endian explicit widths. Times are signed 64-bit UTC nanoseconds with two's-complement encoding; zero and negative times are valid. Struct layouts, pointers, enum ordinals and host padding are never serialized. Reserved bytes and allocation padding are zero. File offsets are unsigned 64-bit values checked against overflow, host address limits and the owning producer's reservation.

The codecs operate on complete local snapshots or immutable committed bytes supplied by their caller. They never perform a plain read of a live atomic word. A parsed reference proves representation and log identity, not that it starts at a real record boundary or that the producer accepted it. Runtime code must establish the exact boundary, ownership, schema/Queue context and acquired committed range before exposing a message.

Successful codec operations allocate no heap storage. The encoder validates the request and output capacity before writing and touches only the planned record extent. Its input objects/spans must be disjoint from output. Decoder views borrow the input bytes; their owner must retain the complete buffer and not mutate it while using a view. These private views are not public incoming `MessageView` objects and do not infer decoding dependencies or create a delivery token.

## Producer file prefix

The reserved prefix is 4,096 bytes. Its first 256 bytes are immutable metadata. The live committed-tail word starts at byte 256 in a separate 256-byte control region; the local header codec excludes that word entirely. Later storage code initializes and accesses it through the native atomic adapter. Data records begin at byte 4,096.

| Offset | Bytes | Immutable field |
| ---: | ---: | --- |
| 0 | 8 | ASCII magic `ANGAPLOG`. |
| 8 / 10 | 2 each | Format major `3`, minor `0`. |
| 12 | 4 | Complete reserved prefix bytes, `4096`. |
| 16 / 24 | 8 each | Session identity high/low halves. At least one half is nonzero. |
| 32 | 2 | Nonzero producer Task ID. |
| 34 | 6 | Reserved zero. |
| 40 | 8 | Nonzero sealed catalog revision. |
| 48 | 8 | Total reserved producer file bytes, including prefix. |
| 56 | 8 | Nonzero publication capacity. |
| 64 | 4 | Nonzero native ABI profile. |
| 68 | 2 | Synchronization revision `2`. |
| 70 | 2 | Atomic word width, `8` bytes. |
| 72 | 8 | Required feature mask `1`, producer-owned commit. |
| 80 | 8 | Optional features, currently zero. |
| 88 | 8 | Data beginning, `4096`. |
| 96 | 160 | Reserved zero. |

The file reservation is a multiple of 64, fits the host's addressable range, and contains at least one minimum 192-byte record after the prefix. Publication capacity cannot exceed `(file_bytes - 4096) / 192`; byte and record-count limits remain independent at runtime. The header does not store a current producer incarnation: retained records can belong to earlier claims of the same Task.

`decode_producer_header()` requires exactly the immutable 256 bytes. It rejects unsupported format/synchronization/feature encodings separately from malformed bytes. It permits a foreign nonzero ABI for offline inspection. `live_profile_compatibility()` separately requires equality with an already qualified native profile; decoding alone admits no live access.

`validate_committed_tail()` checks a previously acquired value: aligned to 64 and between `4096` and the reserved file end, inclusive. It does not load the word, walk records or certify that this value is the end of a valid committed prefix. There is no creation of an old-format reader or migration path.

## Cross-log reference

A **log reference** identifies a Session/producer file and the exclusive range of one record. It deliberately omits incarnation: the referenced record supplies the producing claim and remains in the same retained file across Task replacement.

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 / 8 | 8 each | Session high/low halves. |
| 16 | 2 | Producer Task ID. |
| 18 | 2 | Reference encoding `1`. |
| 20 | 4 | Reserved zero. |
| 24 / 32 | 8 each | Record offset and exclusive end. |
| 40 | 8 | Reserved zero. |

The 48-byte codec binds both identities to the supplied producer header. Offset/end are 64-byte aligned, offset is at or after `4096`, end is within reserved file bytes, and the extent is at least 192 bytes. Empty, reversed, overflowing and wrong-log references reject. A ring can later use a smaller prebound descriptor; this standalone encoding does not define live ring synchronization.

## Publication records

A record consists of a 128-byte prefix, ordered 32-byte destination rows, application payload bytes and zero padding to 64-byte allocation alignment. All offsets in this table are relative to the record; the stored record offset is relative to its producer file. There is no format-2 envelope, mutable receipt suffix, readiness word or Session manifest.

| Offset | Bytes | Fixed field |
| ---: | ---: | --- |
| 0 | 4 | ASCII magic `ANPR`. |
| 4 / 6 | 2 each | Kind `1` Publication; encoding `1`. |
| 8 | 8 | Complete record bytes including alignment padding. |
| 16 | 8 | Record's producer-file offset. |
| 24 / 32 | 8 each | Nonzero producer incarnation and publication sequence. |
| 40 | 8 | Publication UTC timestamp. |
| 48 | 8 | Catalog revision, equal to the producer header. |
| 56 | 8 | Exact schema-definition reference in the sealed catalog. |
| 64 / 66 | 2 each | Message type and payload-format wire code. |
| 68 / 72 | 4 each | Nonzero schema version and application flags. |
| 76 / 78 | 2 each | Publishing Queue Set ID, zero for direct send; acceptance-policy code. |
| 80 | 8 | Nonzero topology revision. |
| 88 | 4 | Original destination count, `1..65535`. |
| 92 | 4 | Reserved zero. |
| 96 | 8 | Application payload byte count, including zero. |
| 104 | 8 | Record ordinal in `1..publication_capacity`. |
| 112 | 2 | Producer Task ID, equal to the producer header. |
| 114 | 14 | Reserved zero. |

Schema references are nonzero, at least `4096`, eight-byte aligned and within host address bounds. They belong to a different catalog file and are not bounded by the producer file's byte capacity. Runtime schema resolution remains required.

| Offset within destination | Bytes | Field |
| ---: | ---: | --- |
| 0 / 2 / 4 | 2 each | Queue ID, acceptance status, rejection reason. |
| 6 | 2 | Reserved zero. |
| 8 | 8 | Assigned Queue sequence or zero for rejection. |
| 16 | 8 | Enqueue UTC time or zero for rejection. |
| 24 | 8 | Reserved zero. |

Acceptance status codes are `1` Accepted and `2` Rejected. Accepted rows require nonzero Queue sequence within the producer publication capacity and reason `None`; their timestamp may be zero or negative. Rejected rows require a known reason other than `None`/`OutcomeUnknown`, zero sequence and zero timestamp. There is no accepted record with zero accepted destinations. Queue IDs are nonzero and unique.

Acceptance policies, payload formats and reason codes reuse the explicit [wire-code tables](../include/angaria/detail/wire_codes.h), not C++ ordinals. `AllDestinations` requires every row Accepted; `Partial` requires at least one. Direct publications have one row and `AllDestinations`; Publishing representations have a nonzero set ID. Multiple-destination representation tests do not implement Publishing delivery or partial retry.

For `D` destination rows and `P` payload bytes, the payload starts at `128 + 32*D` and total bytes are `align64(128 + 32*D + P)`. The minimum is 192 bytes. The plan includes the entire extent in producer reservation checks. Decoding requires exactly that extent, checks every destination and the canonical zero padding, and returns the payload span without copying it. The one-destination path initializes no uniqueness bitmap.

The private input/output uses the common `MessageHeader` value. Only immutable publication fields belong in a record header: schema, format, publication ID/time, payload length and flags. Input containing delivery-specific header fields rejects rather than dropping them. Accepted destination context is stored in its row; consumer/dequeue context is established later by receive. Borrowed decoding reproduces the complete logical publication header exactly.

## Error and performance evidence

Malformed stored representations use `IoError`; unsupported format/encoding/capability or native-profile mismatch uses `UnsupportedPolicy`. Invalid encoder inputs use `InvalidArgument`. Exhausted publication ordinal/byte reservation uses `LogFull`; inadequate caller output uses `OutputBufferTooSmall`. Known encoding rejection leaves output unchanged. A syntactically valid record/reference is never a substitute for live commit evidence.

[Functional fixtures](../test/test_producer_log_format.cpp) independently construct golden bytes, including extreme identities, signed timestamps and empty payloads. They cover malformed/truncated/reserved fields, wrong log/catalog bindings, destination consistency, overflow/bounds, untouched rejection buffers and borrowed header/payload behavior. Ordinary local snapshots may be unaligned; live atomic alignment remains a separate runtime requirement.

The isolated [focused performance test](../test/test_producer_log_performance.cpp) runs the current codec in ordinary CI. For 64/256/1,024-byte payloads it checks complete logical results, matching payload/identity checksums, a borrowed payload pointing into the caller's buffer and zero allocations over warmed encode/decode cycles. A separate positive allocation calibration proves the counter is active. Nine timed blocks report median block-mean nanoseconds/cycle, codec cycles/second and all block values; these are component checks, not per-message latency quantiles or IPC capacity.

The [retained original codec study](../benchmarks/results/milestone-4-producer-log-codecs/README.md) compared the then-new buffered codec with the format-2 owning codec at its pinned source, before public admission. Its historical measurements and plots remain reproducible there. The current build removes that superseded codec and no longer keeps an old implementation solely as a timing control. [Public runtime qualification](public-runtime-performance.md) now measures delivered latency/throughput against nanomsg with recording and acknowledgment contracts labeled. Deterministic allocation/borrow guards fail CI; noisy wall time has no pass/fail threshold.

Run the focused test with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target angaria_producer_log_performance --parallel 3
./build/angaria_producer_log_performance --gtest_output=xml:build/producer-log-performance.xml
```

Step **4.32**, framework **0.4.34**, supplies [producer-owned mapped append, atomic acceptance and recovery/replay](producer-log.md), with separate failure and performance evidence. The byte codec alone cannot establish those guarantees. The [public admission contract](public-runtime-admission.md) now integrates these primitives with normal application messaging.
