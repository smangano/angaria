# Native format 3.0

Framework **0.4.39** admits native **format 3.0**, synchronization revision **2**, and required-feature mask **0x7** for public Sessions. The [public runtime guide](public-runtime-admission.md) describes ownership, lifecycle, guarantees, and supported policies. Formats 1 and 2 are retired; recreate development Sessions with matching programs/libraries. No migration or legacy runtime is supplied.

A **SuperBlock** is the fixed 4,096-byte header that identifies the Session, its format and native ABI, administrative storage limits, fixed sections, and recovery/publication control words. It is serialized field by field; a C++ `struct` is never persisted directly. See [mapped_format.h](../include/angaria/detail/mapped_format.h), [mapped_superblock.cpp](../src/mapped_superblock.cpp), and [the glossary](glossary.md).

## Representation and files

Integers are fixed-width little-endian values in eight-bit bytes. Native atomics require the qualified host ABI and aligned mapped synchronization words. Immutable records and their ownership/reference bounds are checked before exposure; an offset alone proves neither commitment nor message delivery.

| File | Owner and purpose |
| --- | --- |
| `session.ang` and coordination/Task claim files | Administrative Registry, Task diagnostics, topology, setup seal, and administrative redo/recovery. |
| `producer-N.messages.ang` and `producer-N.owner.ang` | One producer log/issuer per Task. All its outgoing Queues share this owner. |
| `queue-N.transport.ang` | Fixed Queue descriptor hints, returned credit, incarnation and acknowledgment hints. Accepted bytes remain in the producer log. |
| `queue-N.progress.ang` | Reserved consumer-owned selected progress/receipts, created only for acknowledged Sources. |

Ordinary messaging never appends to `session.ang`. Its thirteen fixed sections retain Task/topology/schema indexes and administrative reservations. Administrative ring metadata is not the established transport ring. Message acceptance, Source pins, and processing receipts are not old Session-journal records.

## Admission fields

Offsets in this table are absolute bytes in the SuperBlock:

| Offset | Width | Meaning |
| --- | --- | --- |
| 8 / 10 | u16 each | Major **3**, minor **0**. |
| 12 | u32 | SuperBlock bytes, **4096**. |
| 16 / 24 | u64 each | Session identity halves. |
| 32 | u64 | Administrative `max_log_bytes`. |
| 50 | u16 | Synchronization revision **2**. |
| 52 | u32 | Qualified native ABI profile. |
| 56 | u64 | Required features; public profile **0x7**. |
| 72 | u64 | Committed Session-definition reference. |
| 112 | u64 | Session-ready flag. |
| 120 | u64 | Applied administrative generation. |
| 184 | u64 | Catalog seal revision; zero means setup remains mutable. |

The metadata-only `0x3` framing profile exists for byte/administrative fixtures. `Session::attach` requires the recorded messaging capability before entering recovery or mutable runtime access. Unknown versions, profiles, synchronization requirements, and retired required-feature masks reject explicitly; byte-preservation tests verify rejection does not repair or reinterpret the input.

The Session-definition immutable body has a **144-byte fixed prefix**. Its effective acknowledgment default is u16 at body offset **12**; `producer_log_bytes` and `producer_log_publications` are u64 at body offsets **128** and **136**. Descriptor string slices follow that prefix. A Source-definition body's u16 at **70** stores acknowledgment: **1 = required**, **2 = none**. These are body-relative offsets, not file offsets. [Session metadata](../src/session_metadata.cpp) and [topology metadata](../src/topology_metadata.cpp) own the remaining canonical field layouts.

## Administrative commit and the seal

The global journal handles Registry/Task/topology administration. Its current transaction subtype is Registry and its redo operation is absolute `WriteRange`. Retired message-record kinds 9/10 and legacy compact claim/loss commands are rejected, including persisted intents, before applying writes; malformed committed evidence is retained for diagnosis.

Under the administrative gate, first operational admission validates the complete catalog and initializes all required message resources. A fixed-state administrative transaction then records its definition revision at byte 184. After that commit, resource paths and definitions remain fixed; attaching replacement Tasks changes lifecycle diagnostics, not the catalog revision. Reopening sealed resources validates their bindings and never deletes/recreates them.

## Messages and consumer progress

The [producer-log format](producer-log-format.md) defines immutable accepted Publications, per-Queue destination sequences and atomic committed-tail cuts. The [producer log](producer-log.md) and [Queue](recorded-queue.md) implementations supply append, recovery, bounded discovery, and replay. Connection headers with all-schema binding record the schema count at byte **184** and its canonical fingerprint at **192**; these offsets belong to the connection file, separately from the Session seal.

The [consumer progress format](recorded-acknowledged-progress.md) has its own reserved operation images and receipt rows. Its selected operation commits processing progress independently of the producer log and returned credit. Unacknowledged Sources reserve no such receipt file.

Producer byte/publication capacities are configured separately from administrative storage. They are physical reservations, not prefaulted RAM. Returning credit or acknowledgment does not reclaim accepted records. Online resource replacement, reclamation, foreign-ABI reading, and power-loss recovery are outside the admitted profile.

The retired layout and historical performance data remain available at [the pre-admission revision](https://github.com/smangano/angaria/blob/b4ec8ae/docs/mapped-format.md); those byte layouts are not readers maintained by the current runtime.
