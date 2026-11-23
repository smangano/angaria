# Mapped layout and compatibility contract

Step 1.4 specifies the format for the shared Registry and future mapped queues. This is the proposed **format 2.0**, not a format that the current executable creates. The runnable prototype still uses [format 1](session-format.md). The [public API](api.md) contains process-local objects; its C++ object layout is not a file format.

The byte counts and checked capacity arithmetic are compile-checked in [mapped_format.h](../include/angaria/detail/mapped_format.h). Record encoding, mappings, and attachment validation arrive in the implementation milestones. The [step 1.5 concurrency contract](concurrency.md) assigns the journal, transaction, and recovery protocols, using the reserved space without changing the fixed reservation totals. A record's presence alone never establishes that it committed.

The [terminology glossary](glossary.md) defines the storage, encoding, and recovery terms used here.

## Representation rules

- Bytes have eight bits. Unsigned integers have the stated width; signed integers use two's complement. Multibyte values are little-endian.
- No pointer, `size_t`, `time_t`, native enum, `bool`, chrono object, STL container, mutex, or C++ struct padding is persisted. Encoders write individual fields. A mapped integer used for synchronization must also meet the live ABI's alignment and access rules.
- A reference is an unsigned 64-bit **logical byte offset** in this session. Zero is null. Fixed sections start on 64-byte boundaries, arena records on 8-byte boundaries, and synchronization words on 8-byte boundaries. Alignment does not imply a particular CPU cache-line size.
- `ByteSlice` is `(offset:u64, byte_length:u64)`. `ArraySlice` is `(offset:u64, element_count:u64)` with element width defined by its enclosing record. Empty slices are `(0,0)`. Nonempty slices must fit wholly inside their owning record or their explicitly named pool section.
- Registry names, documentation, and version/user strings are length-delimited UTF-8 without a terminating zero. Names are nonempty and cannot contain U+0000. Byte lengths differ from character counts. Documentation and optional version strings can be empty.
- Reserved bytes are zero when written. Unknown required features or enum codes fail live attachment. An offline reader can report an unsupported payload as opaque when its enclosing length is valid.

The session identity is two `u64` words, high then low, matching `SessionId`. It is opaque, nonzero, and stable for the session. Records in a session inherit that identity; external publication IDs and delivery tokens include it explicitly.

## IDs, counters, and schemas

| Value | Stored representation and limit |
| --- | --- |
| Task and Queue IDs | Separate `u16` namespaces, 1 through 65,535. Zero is invalid. |
| Source and Publishing Queue Set IDs | Distinct kinds sharing one `u16` allocation namespace, 1 through 65,535 in total. Kind is checked on every lookup. |
| Message type | `u16`, 1 through 65,535. A type retains one name across its versions. |
| Schema version | `u32`, 1 through 4,294,967,295. The pair `(message_type, version)` is the schema identity. |
| Field ID | Full `u32` with high byte `0xEE`; the low 24 bits are the application identity, including zero. At most 16,777,216 distinct IDs exist. |
| Schema and symbol table counts | `u32` capacities. Schema capacity counts versions, independently of the number of message types. Symbol zero is invalid; IDs run from 1 through the configured capacity. |
| Task incarnation, publication sequence, queue sequence, delivery issuance, Registry/topology revision, notification generation | `u64`. Zero is unissued/unset. Exhaustion returns `CapacityExceeded`; counters never wrap or reuse an issued value. |

Committed entity IDs and schema versions are not reused during a session. Field definitions are immutable after registration. Changing a Field's meaning requires another Field ID; changing a message definition requires another schema version. The legacy 16-bit `EFieldType` cannot represent these Field IDs and is not the format-2 Field encoding.

A schema record contains its exact type/version, ordered **32-bit** Field IDs, name, documentation, and registration metadata. Field order and repetitions are preserved. Its array length is `u64`, bounded by the containing record and available log bytes; this introduces no undocumented 16-bit field-count limit. Publications retain a reference to this immutable schema as well as its type/version. Schema and Field records required by any retained publication remain available even after all its deliveries terminate.

`0xEE000000` is a valid Field identity: the all-zero raw ID is invalid, but the effective 24-bit portion may be zero. Field IDs and schema type/version values are application identities, not allocation-slot numbers. Message types retain a one-to-one name pairing across their versions. Numeric schema versions need not be registered in ascending order.

## Time

Stored timestamps are signed 64-bit UTC nanoseconds since the Unix epoch, matching `UtcTimestamp`. Epoch zero is a valid time. Optional values have explicit presence flags. Out-of-range conversions fail before committing; native `time_t` and the prototype's seconds/nanoseconds structs are never copied into records.

Creation, registration, attachment, acceptance/enqueue, dequeue, acknowledgment, and loss times use this encoding. Per-destination acceptance times can differ during partial retry. Dequeue and acknowledgment times belong to destination receipts, rather than to the immutable message payload. A receipt records its most recent issuance and terminal result; it is not an unlimited journal of every receive/release attempt.

Timeouts and elapsed waits use a process-local monotonic clock. UTC clock adjustments do not change queue order, identity, or timeout behavior. Queue-local sequences establish delivery order.

## Session files and logical offsets

`session.ang` begins with the [superblock](#superblock), a fixed 4,096-byte session control area, followed by the fixed sections described by its directory. For `two_plane`, its variable arena follows those sections in the same file, so logical offsets are file offsets. Queue rings also occupy arena space. A reference never points into a directory name or a native virtual address.

For `segmented`, the fixed sections stay in `session.ang`; immutable arena records and growing queue storage use extent files. Each extent has a 4,096-byte header recording magic, format, session identity, logical start, allocated length, and feature/ABI requirements. Its filename encodes its logical start. Extent logical intervals cannot overlap each other or the fixed sections. Translation is `file_offset = logical_offset - extent_logical_start`. The extent header occupies the first 4,096 bytes of that interval and is not a record target.

An extent header contains magic `ANGAREXT` at byte 0, major/minor (`u16` each) at 8/10, header bytes (`u32`) at 12, session identity at 16, logical start (`u64`) at 32, allocated file length (`u64`) at 40, required features (`u64`) at 48, live ABI profile (`u32`) at 56, synchronization version (`u16`) at 60, and storage code (`u16`) at 62. The eight-byte readiness word at 64 is `0` incomplete, `1` Ready, or `2` Committed under the journal protocol; bytes 72 through 4095 remain reserved. The allocated length includes this header.

The configured `segment_bytes` is an allocation quantum, not a maximum message size. It must be a multiple of 64 and large enough for an extent header plus a record (at least 4,160 bytes). An extent can allocate multiple quanta to keep one complete record, including its payload and receipts, contiguous. Mapping an extent at different virtual addresses in different processes does not alter stored references. Milestone 8 implements extent discovery, publication, interrupted creation, and reclamation against these rules.

Logical offsets and individual file lengths must fit `INT64_MAX`, the supported file-offset range. Before translating a reference, validate its owning extent, alignment, target kind, and full byte range using subtraction or checked arithmetic. Never form a pointer or allocate a buffer first. Mapping windows and returned byte spans must also fit the host's `size_t` and address space. No single mapping of the entire configured log is required.

Offsets remain stable while their targets are retained. Initial implementations may retain all committed history until `LogFull`; reclamation is not required to accept another publication. Future reclamation must preserve every live reference, schema, pending recovery obligation, in-flight pin, and retry record it retains. It cannot reuse a committed logical address for another record; never-published aborted space can be reused after rollback. Reclaimed publication history returns `HistoryUnavailable` on lookup or retry.

Queue state reserves a retained-history floor sequence and link. Traversal stops at that link and never follows its immutable predecessor into a reclaimed prefix. A floor advances only when no recovery obligation or in-flight pin needs the prefix, and only alongside a recoverable retention checkpoint. Cross-destination and retry references can require retaining entire publications even when one queue has finished them. Milestone 8 implements this protocol; the initial no-reclamation implementation leaves its floors unchanged.

## Superblock

In Angaria, the **superblock** is the fixed 4,096-byte session control area at byte offset zero of `session.ang`. It is the starting point for identifying the session, interpreting its format, and locating its other storage sections. It contains session identity, format and compatibility information, storage/capacity settings, the fixed-section directory, mutable roots/control values, and the reserved operation journal.

The directory always contains thirteen entries describing the fixed sections. Nonempty sections allocate storage after the superblock; zero-capacity sections allocate no bytes and have offset zero. Section contents, the Registry's variable-sized definition records, and message payloads are stored outside the superblock. The superblock is therefore the session's control area, not the entire Registry or the entire fixed reservation. Its immutable configuration metadata and mutable recovery/control fields have different validation and access rules.

Offsets below are from the start of `session.ang`. The initial immutable settings are separate from mutable roots. The [journal protocol](concurrency.md#operation-protocol-and-crash-boundaries) defines their authority and recovery.

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | Magic `ANGARIA\0`. |
| 8 | 2 | Format major, `2`. This position also distinguishes prototype major `1`. |
| 10 | 2 | Format minor, `0`. |
| 12 | 4 | Superblock bytes, `4096`. |
| 16 | 16 | Session identity. |
| 32 | 8 | Maximum accounted session bytes. |
| 40 | 8 | Fixed-section extent length / initial arena base. |
| 48 | 2 | Storage mapping code. |
| 50 | 2 | Synchronization protocol version. |
| 52 | 4 | Live ABI profile. |
| 56 | 8 | Required feature bits. |
| 64 | 8 | Optional, safely skippable feature bits. |
| 72 | 8 | Current session-definition reference. |
| 80 | 8 | Committed Registry revision root. |
| 88 | 8 | Allocation tail logical offset. |
| 96 | 8 | Last committed append-transaction manifest reference. |
| 104 | 4 | Section count, `13` for this layout. |
| 108 | 1 each | Address width (`64`), byte order (`1` = little-endian), atomic word width (`8`), reserved zero. |
| 112 | 8 | Live session-state flags. |
| 120 | 8 | Last completed operation generation. |
| 128 | 32 | Eight `u32` limits: Tasks, Queues, Source sets, Publishing sets, memberships per set, Fields, schema versions, symbols. |
| 160 | 8 | UTF-8 application string-pool byte capacity. |
| 168 | 8 | Segment allocation quantum; zero for two-plane storage. |
| 176 | 8 | Current accounted session bytes. |
| 184 | 72 | Reserved. |
| 256 | 416 | Thirteen 32-byte section directory entries. |
| 672 | 2048 | Operation journal: 256-byte header and 1792-byte inline command area. |
| 2720 | 1376 | Reserved. |

Each section entry contains `kind:u16`, `flags:u16`, `stride:u32`, `offset:u64`, `count:u64`, and `byte_length:u64`. A zero-count section has zero offset and byte length. Other sections must agree with their recorded capacity, stride, and checked byte count. Fixed sections occur in the following order and cannot overlap.

| Kind | Section | Stride | Count |
| ---: | --- | ---: | --- |
| 1 | Task index | 64 | `tasks` |
| 2 | Queue index | 64 | `queues` |
| 3 | Source Queue Set index | 64 | `source_sets` |
| 4 | Publishing Queue Set index | 64 | `publishing_sets` |
| 5 | Field index | 64 | `fields` |
| 6 | Schema index | 64 | `message_schemas` |
| 7 | Task runtime state | 256 | `tasks` |
| 8 | Queue progress state | 256 | `queues` |
| 9 | Source runtime state | 256 | `source_sets` |
| 10 | Publishing runtime state | 128 | `publishing_sets` |
| 11 | Membership/scheduling state | 32 | `(source_sets + publishing_sets) × memberships_per_set` |
| 12 | Symbol index | 32 | `symbols` |
| 13 | UTF-8 application string pool | 1 | `string_pool_bytes` |

Index slots are allocation slots, not direct array positions derived from sparse application Field IDs or schema versions. A 64-byte index entry contains `key:u64`, `current_definition:u64`, `revision:u64`, `state:u64`, and 32 reserved bytes. Narrow IDs occupy the low bits of the key; schema keys are `(u64(message_type) << 32) | version`. Empty slots have zero keys. Registry lookups validate kind, identity, uniqueness, and the referenced committed definition. Process-local lookup indexes can be rebuilt from these slots.

## Arena records and definitions

Every arena record starts with this 64-byte envelope. All lengths include required padding, and `total_bytes` is a multiple of eight. The immutable region follows the envelope; any mutable suffix has an explicit kind-specific layout. When a suffix exists, `immutable_region_bytes` includes trailing padding to align the suffix start to eight bytes. A payload slice reports its actual bytes, excluding that padding.

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 2 | Record kind. |
| 2 | 2 | Kind-specific encoding version, initially `1`. |
| 4 | 4 | Envelope bytes, `64`. |
| 8 | 8 | Total allocated record bytes. |
| 16 | 8 | Immutable-region bytes, excluding envelope. |
| 24 | 8 | Operation generation from the journal. |
| 32 | 8 | Previous retained record of the same revision/attempt chain, or zero. |
| 40 | 8 | Associated Registry revision, zero if inapplicable. |
| 48 | 8 | Atomic readiness: `0` incomplete, `1` Ready, `2` Committed. Ready alone establishes no acceptance. |
| 56 | 8 | Reserved recovery word. |

Record kinds are `1` Session definition, `2` Task definition, `3` Queue definition, `4` Source definition, `5` Publishing definition, `6` Field definition, `7` Message schema, `8` Ring storage, `9` Publication, `10` Retry acceptance, `11` Append transaction manifest, and `12` Symbol value. Kind 11 covers Registry, publication, and retry batches with the [transaction body and redo stream](concurrency.md#redo-commands-and-append-manifests). Symbol value bodies and feature negotiation arrive in milestone 9. Readers cannot infer acceptance from an uncommitted body or a ring slot alone.

Entity definitions start with a common 64-byte body: `key:u64`, `created_by:u16`, `entity_state:u8`, five reserved bytes, `created_at:i64`, `definition_revision:u64`, `name:ByteSlice`, and `doc:ByteSlice`. Creation attribution/time stay unchanged across revisions. The metadata API's revision identifies the current committed definition. Non-lifecycle entities use state zero. Kind-specific fields follow that body; inline arrays and UTF-8 bytes follow those fields.

| Definition | Additional fields, in byte order |
| --- | --- |
| Task | `task_kind:u16`, `parent_process:u16`, four reserved bytes. Parent is zero for process Tasks. |
| Queue | `producer:u16`, `source_set:u16`, `priority:u8`, `delivery_policy:u8`, two reserved bytes, `ring_capacity:u64`. A draft can have an unset source; activation requires a complete definition. Ring capacity is zero for segmented mapping. |
| Source set | `owner:u16`, `selection:u16`, `wait:u16`, two reserved bytes, `effective_random_seed:u64`, `members:ArraySlice` of `u16` Queue IDs. Nonrandom selectors store seed zero. |
| Publishing set | `owner:u16`, `acceptance:u16`, four reserved bytes, `destinations:ArraySlice` of `u16` Queue IDs, `topology_revision:u64`. |
| Field | `base_type:u16`, `flags:u16`, `declared_size:u32`, `attributes:ByteSlice`. Attributes are empty for the initial scalar definitions; later required encodings need feature/version negotiation. |
| Message schema | `message_type:u16`, two reserved bytes, `schema_version:u32`, `fields:ArraySlice` of full `u32` Field IDs. |

Step 2.7's Field and schema bodies each have 88 fixed bytes. Field flags and attributes are zero/empty; its name and documentation follow the fixed body in order. A schema places its encoded Field array immediately after the fixed body, followed by name and documentation. Array order and repetitions are preserved. An empty array has zero offset and count; optional empty documentation likewise uses a zero slice. Short zero padding completes the record allocation. Portable codecs recognize all stable base codes, while the native implementation currently enforces fixed scalar byte widths (1 for char/byte, 2 for 16-bit integers, 4 for 32-bit integers/floats, 8 for 64-bit integers/floats). Later representations require their specified encodings and feature negotiation.

Immutable Field definitions have zero envelope predecessor. A new schema version links to the latest retained definition for its message type in registration order, independently of numeric version or allocation-slot order. Definitions in one atomic Registry Append may share a revision; logical record order distinguishes their predecessor chain. Readers require matching body/index/envelope identities and revisions, registered creators, a consistent type/name pairing, and Field references that existed by the schema's registration revision. All versions retain their index entries and original definitions.

Resolved policies, priorities, capacities, and generated seeds are persisted. Inspection reconstructs owning descriptors from them. Each definition owns its variable bytes inside its immutable record. Registry names/docs and ordered schema Field arrays consume arena/log bytes, **not** the application string pool. New definitions and topology revisions append records and advance index roots through one Registry transaction; existing publications continue to reference their original definitions.

Step 2.8 implements Queue, Source, and Publishing bodies with 80, 96, and 96 fixed bytes respectively. Source members and Publishing destinations are unique ordered `u16` arrays immediately after the fixed body, followed by name and documentation; empty slices use zero offset and count. All policies are resolved before encoding. Nonrandom Sources store seed zero and expose no random-seed override on inspection. New entities store Draft in both body and index. A Queue's Source membership update appends a new Source revision linked to its retained predecessor in the same batch. Current indexes must cover every retained identity and point to its latest definition; historical references and inverse Source membership are checked at their registration revision. The [native contract](native-storage.md#draft-queues-and-queue-sets) specifies initial draft-state mirrors and policy support.

The Session definition has its own 128-byte fixed body: creator Task (`u16`), storage (`u16`), four default policy codes (`u16` each, delivery/selection/wait/acceptance), four reserved bytes, creation time (`i64`), default ring capacity (`u64`), segment quantum (`u64`), required-policy feature mask (`u64`), then five `ByteSlice`s for name, framework version, application version, creator user, and original directory. The slices occupy offsets 48 through 127. The directory is diagnostic; session identity and references survive relocation. `Session::options()._directory` describes the directory actually attached.

## Preallocated live state

Live state contains fixed-width data and recovery space. Native lock objects and file descriptors stay outside these records. The shared journal handles interrupted updates without allocating during acknowledgment; the remaining row reserves permit later compatible extensions.

| State | Required contents |
| --- | --- |
| Task, 256 bytes | First 64 bytes: incarnation, last issued publication sequence, restart count, PID, native thread ID, last attachment UTC time, flags including timestamp presence, and parent incarnation; all eight-byte fields. Offset 64 reserves the current claim's retired-publication-sequence floor, 72 stores last detach UTC time, and 80 through 255 remain reserved. OS ownership checks remain authoritative; a recorded PID or attached bit alone proves no live claim. |
| Queue, 256 bytes | Producer region at 0: last assigned queue sequence, latest delivery-link reference, ring reference, ring capacity, producer incarnation, retained-history floor sequence, retained-history floor link, reserved word; all `u64`. Consumer region at 64: terminal sequence watermark, oldest pending link, last acknowledgment UTC time, flags, then 32 reserved bytes. Bytes 128 through 255 reserve recovery state. The watermark can advance only past a contiguous prefix of acknowledged or explicitly lost deliveries. |
| Source set, 256 bytes | At 0: issuance counter (`u64`), consumer incarnation (`u64`), selected Queue (`u16`), flags (`u16`), four reserved bytes, selected queue sequence (`u64`), publication reference (`u64`), receipt reference (`u64`), dequeue time (`i64`), delivery-link reference (`u64`). At 64: scheduling round, tie cursor, four RNG words, notification generation, and `wait_armed`; all `u64`. Bytes 128 through 255 remain reserved. |
| Publishing set, 128 bytes | Topology revision, snapshot definition reference, producer incarnation, flags; all `u64`. Remaining bytes reserve recovery state. |
| Membership, 32 bytes | Queue (`u16`), priority (`u8`), flags (`u8`), four reserved bytes, topology revision (`u64`), remaining fair credits (`u64`), reserved word (`u64`). Rows are grouped by Source allocation slot, then Publishing slot, each with `memberships_per_set` entries. |

A Source's in-flight slot pins one receipt and its payload. Its issuance counter remains valid across Source handle close/reopen within a claim. A new consumer incarnation distinguishes replacement claims. Re-reading the in-flight slot preserves its token. Recovery cannot treat a stale in-flight flag from a dead claim as proof of acknowledgment.

Task state records the latest attachment plus monotonic restart/incarnation information. Immutable definition revisions preserve Task registration and topology lifecycle history; publications and receipts retain the claims that performed their operations. This is not an unlimited journal of every attach/detach event. Replacement claims and progress recovery must remain possible when publication storage is full; step 1.5 resolves them using reserved state.

The Session flags word at superblock offset 112 uses bit 0 (`1`) for completed bootstrap; remaining bits are reserved. Public attachment requires this flag after recovery. The Task flags word at row offset 48 uses bit 0 (`1`) for recorded attachment, bit 1 (`2`) for presence of last-attachment time, and bit 2 (`4`) for presence of last-detachment time; remaining bits are reserved. A never-attached Task row is all zero. Process Tasks have zero parent incarnation; an attached Thread Task records its parent's incarnation. Detachment preserves that tag so interrupted cleanup can rediscover the ending parent's children during replay. Reserved Task bytes and state in empty index slots must be zero.

The 32-byte symbol row reserves ID (`u32`), flags (`u32`), value reference (`u64`), UTF-8 byte length (`u32`), character count (`u32`), and a reserved word (`u64`). Symbol values are immutable arena records and retain the wiki's 64-character maximum. Application strings occupy the separately reserved pool and use pool-relative offsets and lengths. Milestone 9 defines pool entry headers, retention, encoding constraints, and registration operations; its references must retain these lifetime and capacity rules. Registry metadata does not reduce the configured application pool capacity.

## Rings, publications, and receipts

A two-plane ring allocation has a 64-byte record envelope followed by `ring_capacity` slots of 32 bytes, rounded up to 64 bytes. Each slot contains `queue_sequence:u64`, `delivery_link:u64`, `publication_ref:u64`, and `generation/state:u64`. A ring slot is an index into retained history. Its overwrite is neither an acknowledgment nor permission to reclaim a pinned payload. Recover-from-log queues reconstruct missing entries from retained delivery links and receipts. Allow-loss queues use overwritten sequence ranges while preserving an already issued pin.

Segmented storage has no ring-overwrite boundary. Under this initial contract it grows until the session byte cap, where new publications return `LogFull`; `allow_loss` does not introduce implicit eviction. Both delivery policies preserve pending deliveries in that mapping. A future eviction policy would need an explicit boundary, gap semantics, and feature negotiation.

A Publication's immutable region starts with a 128-byte body:

| Offset in body | Bytes | Field |
| ---: | ---: | --- |
| 0 | 2 each | Producer Task, Publishing set (zero for direct send), message type, payload format. |
| 8 | 4 each | Schema version, application flags. |
| 16 | 8 each | Producer incarnation, producer publication sequence. |
| 32 | 8 each | Initial publication UTC time, original topology revision. |
| 48 | 4 | Destination count. |
| 52 | 4 | Reserved. |
| 56 | 16 | Payload `ByteSlice`. |
| 72 | 8 | Exact immutable schema reference. |
| 80 | 16 | Destination `ArraySlice`, element width 48. |
| 96 | 16 | Receipt `ArraySlice`, element width 128, in the mutable suffix. |
| 112 | 2 | Acceptance policy code. |
| 114 | 14 | Reserved. |

The original full destination snapshot and encoded payload are immutable. Each 48-byte destination row contains Queue ID (`u16`), status (`u16`), reason (`u16`), flags (`u16`), accepted queue sequence (`u64`), previous delivery link for that queue (`u64`), acceptance/enqueue UTC time (`i64`), receipt reference (`u64`), and a reserved word (`u64`). Rejected destinations have no queue sequence/link or acceptance timestamp. A direct send has one destination. An accepted delivery link points to its immutable destination row, which connects queue-local history independently of ring wraparound.

Every committed publication reserves a 128-byte receipt slot for **every destination in its original snapshot**, including those initially rejected under partial acceptance. Unused slots carry no delivery obligation. The first 64 bytes hold state (`u64`), consumer Task (`u16`), Source set (`u16`), presence flags (`u32`), consumer incarnation (`u64`), issuance (`u64`), latest dequeue time (`i64`), acknowledgment time (`i64`), terminal/loss time (`i64`), and a reserved word (`u64`). The second 64 bytes reserve receipt recovery/shadow state. Receipt states are unused, pending, issued, acknowledged, and lost; their visibility protocol is finalized in step 1.5.

Acknowledgment, release, and loss progress update these reserved receipts, Queue state, and Source state. They need no new history allocation when the log is full. This reserves historical dequeue/acknowledgment metadata as well as the queue's progress watermark. Receipt history lives as long as its publication is retained.

A successful partial retry appends a Retry acceptance record referencing the original publication and previous attempt, with its attempt UTC time and an array of the same 48-byte destination rows. It references the original payload/schema and preallocated receipt slots. It assigns new queue sequences only to newly accepted destinations. Its fixed body is 64 bytes: original publication reference (`u64`), previous attempt reference (`u64`), attempt time (`i64`), attempted count (`u32`), four reserved bytes, outcomes `ArraySlice`, actor Task (`u16`), six reserved bytes, and actor incarnation (`u64`). Committed attempt records establish cumulative acceptance; already accepted destinations acquire no new obligation. A retry that accepts nothing appends nothing and returns its attempt's reasons while preserving the previous committed acceptance state.

A known fully rejected new publication allocates no committed record and changes no queue/log state. Required space includes the envelope, fixed body, full snapshot, payload, all receipt slots, and alignment. Step 1.5 defines preflight, reservation, atomic visibility, and rollback so allocation does not weaken that rejection contract. Interrupted calls can still return `OutcomeUnknown` and require recovery lookup.

## Stable codes

These are explicit wire codes, independent of C++ enum ordinals:

| Family | Codes |
| --- | --- |
| Task kind | `1` process, `2` thread. |
| Entity state | `0` not applicable/unset, `1` draft, `2` active. |
| Storage | `1` two-plane, `2` segmented. |
| Selection | `1` strict, `2` fair, `3` weighted random. |
| Waiting | `1` spin, `2` eventfd. |
| Delivery | `1` recover from log, `2` allow loss. |
| Acceptance | `1` all destinations, `2` partial. |
| Payload format | `1` binary, `2` JSON, `3` XML, `4` CSV, `5` text. |
| Destination outcome | `1` accepted, `2` rejected. Unknown is a recovery/API outcome, not a committed acceptance. |
| Receipt state | `0` unused, `1` pending, `2` issued, `3` acknowledged, `4` lost. |

Field base codes are `0` opaque/none, `1` char, `2` byte, `3` int16, `4` uint16, `5` int32, `6` uint32, `7` int64, `8` uint64, `9` float32, `10` float64, `11` date, `12` timestamp, `13` years, `14` months, `15` days, `16` hours, `17` minutes, `18` seconds, `19` milliseconds, `20` microseconds, `21` nanoseconds, `22` chars, `23` bytes, `24` symbol, `25` string. Declared fixed Field size is `u32`; variable representations use explicit lengths and are not C++ object sizes. Milestones 3 and 9 specify payload/compound encodings and their required feature bits.

Persisted rejection reasons use `0` None, `1` InvalidArgument, `2` InvalidState, `3` InvalidConfiguration, `4` ConfigurationMismatch, `5` NotFound, `6` Conflict, `7` CapacityExceeded, `8` OwnershipBusy, `9` WrongOwner, `10` ReadOnlyRegistry, `11` NotActive, `12` UnsupportedPolicy, `13` SchemaConflict, `14` LogFull, `15` StaleDelivery, `16` OutputBufferTooSmall, `17` HistoryUnavailable, `18` IoError, and `19` OutcomeUnknown. Encoders translate explicitly. A committed accepted destination has reason None; an unknown-outcome code cannot establish a committed rejection.

## Capacity accounting and validation

The format ceilings are the ID/count widths above and a positive `max_log_bytes <= INT64_MAX`. A build may advertise lower ceilings or fewer policy features; attachment checks the **recorded effective capacities**, not merely the number of currently used entries. Such build ceilings must be reported in configuration diagnostics. Supporting a policy code in the format does not imply that a particular build implements it.

Creation computes all thirteen section sizes with checked multiplication/addition, aligns each nonempty section and the final arena base to 64 bytes, and validates the result before creating files. Zero capacities reserve nothing for that facility; C++ callers select them explicitly through named setters or `CapacityLimits::empty()`. Default construction uses the [bounded C++ profile](configuration.md#capacity-limits). Creator registration still requires at least one Task slot. The combined Source/Publishing capacities must fit 65,535; memberships per set must fit 65,535 even if few queues are currently declared.

`max_log_bytes` accounts for fixed sections and their padding, metadata/schema records and strings, symbols and the application string-pool reservation, activated ring allocations, extent headers/slack, publication payloads/snapshots/receipts, retry history, and transaction/recovery records. It bounds allocated session data, including reserved but unused space, rather than only user payload. Lock files and process-local lookup/mapping caches are coordination resources outside this byte budget. Reclaimed extents can release accounted space only once retention and recovery allow their removal; logical addresses remain unreused.

Rings allocate on queue activation using the resolved queue capacity, not for every reserved Queue slot. An activation fails atomically if its storage cannot be allocated. Initial configuration creation preflights its definitions, initial topology, and all resources needed for configured activation. Runtime definitions and retries compete with messages for remaining arena bytes. A Field-count/schema-count reservation alone promises no arbitrary amount of variable metadata storage.

For the default C++ capacity profile and [trading.toml](../examples/trading.toml), the fixed reservation is **1,197,056 bytes**. Each 1,024-slot ring adds **32,832 bytes**; the trading example's three configured queues add **98,496 bytes**. The initial trading total is therefore **1,295,552 bytes before variable definitions, transaction records, and message history**, within the configured 67,108,864-byte cap. The arithmetic tests preserve this example and reject overflow and alignment edge cases.

An attaching reader validates magic/version/features/ABI, all directory lengths and nonoverlap, extent identity/ranges, committed record boundaries, slice ownership, reference kinds, exact schema identities, entity ownership/membership, and counter ranges before exposing usable handles. Signed TOML values are range-checked before unsigned conversion. Known invalid configuration returns `InvalidConfiguration`; incompatible supplied settings return `ConfigurationMismatch`; unsupported policies return `UnsupportedPolicy`; format/ABI/corrupt-file diagnostics return `IoError` with the failing path and condition. No repair writes occur before compatibility is established.

## Versioning and migration

| Version | Meaning |
| --- | --- |
| TOML `config_version` | Configuration keys/defaults, currently `1`; independent of binary layout. |
| Format major/minor | Portable session/container layout. Major changes require migration. Minor changes can add only negotiated, length-delimited extensions. |
| Record encoding version | Layout of a particular record kind. Unknown required encodings fail live attachment. |
| Feature masks | Required codecs/policies/extensions and optional safely skippable information. Missing required bits fail attachment. |
| Live ABI profile and synchronization version | Native atomic-access requirements, OS/architecture profile, and the shared-state/recovery protocol. These must match for live attachment even when offline record decoding is possible. |
| Message type/version | Immutable application schema identity, unrelated to framework versions. |
| Framework/application strings | Diagnostic metadata; they do not substitute for format or ABI negotiation. |

Initial live profiles target 64-bit little-endian Linux and macOS, with separately qualified x86-64 and ARM64 profiles and naturally aligned 64-bit shared synchronization words. Eventfd requires Linux. The [concurrency contract](concurrency.md#live-profiles-and-atomic-words) assigns protocol/profile identifiers and requires actual native primitive qualification before runtime support is advertised. Portable offline decoding does not require the originating CPU, STL ABI, or synchronization profile.

A live attacher requires a supported format major/minor, supported required record/features, matching ABI/protocol, and sufficient build capacities. A newer minor is not automatically safe for an older writer; it must explicitly advertise compatibility or be rejected. Offline readers may skip recognized optional extensions using recorded sizes, and must never silently reinterpret an unsupported required encoding.

Prototype format 1 remains readable by its existing tools and tests. Format 2 does not attach to, append to, or automatically upgrade it. Migration/replay creates a new session with new identity and explicit schema/Task/topology mappings; it does not preserve ownership locks or delivery tokens. That tooling follows milestone 10. Copying a quiescent format-2 session preserves its identity and history for inspection; its copy must not run as an independent live session with that same identity.

## Implementation handoff

The [step 1.5 protocol](concurrency.md) binds these records to serialized mutation, a single journal commit point, repeatable redo/rollback, fixed-state progress at `LogFull`, and notification generations. The executable model checks bounded crash schedules; native implementation and qualification follow in milestones 2 through 8 and 10. The initial API operations are blocking. This layout makes no end-to-end lock-free claim.

[Milestone 1](milestones/milestone-1.md) · [Configuration](configuration.md) · [Public API](api.md)
