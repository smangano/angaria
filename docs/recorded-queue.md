# Recorded point-to-point delivery

Framework **0.4.35**, milestone step **4.33**, implements workstream step 4 of the [independent messaging plan](recorded-unacknowledged-delivery-plan.md#implementation-steps-for-review). The private [RecordedProducer and RecordedConsumer](../src/recorded_queue.h) endpoints connect the [producer-owned log](producer-log.md) to a bounded descriptor ring. **Recorded/unacknowledged** means every accepted message remains in the log, while receiving it creates no application-processing acknowledgment obligation.

Step **4.34**, framework **0.4.36**, qualifies workstream step 5's process restart, log catch-up and connection independence. The [restart and independence section](#restart-and-connection-independence) describes the tested boundaries and the limits of this private profile.

Step **4.35**, framework **0.4.37**, implements private [optional acknowledged progress](recorded-acknowledged-progress.md). Acknowledged bindings use this same producer-owned message log and descriptor ring, plus a separate consumer-owned receipt file. `RecordedAcknowledgedConsumer` returns `Delivery` with an exact token; repeated pinned receives, acknowledgment, release and replacement use local committed progress. Unacknowledged bindings reserve no receipt file. Public admission remains later work.

This is a private, fixed-topology implementation. Public Session admission, TOML integration and the public runtime replacement follow separately. Acknowledgment remains the public default; this step does not change it. The runnable [primer](primer.md) continues to use the current public API.

## Checked setup and ownership

The first private profile has **one outgoing Queue per producer Task, one input per Source Queue Set, one fixed exact message schema per binding, and distinct producer/consumer Process Tasks**. It uses Binary payloads, recover-from-log delivery, strict single-input selection and nonblocking polling. These are qualification limits, not permanent framework limits. Thread Task parent claims, multiple inputs, Publishing fan-out, allowed loss and notification policies require their later qualification.

Before public admission, outbound Queue endpoints must share one Task-owned producer-log owner, and endpoints must cache all registered exact schemas and select the right codec from each `MessageHeader` schema identity. The present single-schema/single-Queue private binding must not narrow the intended public messaging interface.

`RecordedQueueOptions` supplies a **sealed catalog**: the Registry/schema/topology snapshot fixed before messaging, with its exact revision, active Queue/Source identities and actual Field/message definitions. Opening resolves and retains an exact Binary codec once. It checks selected ownership, membership, policy, schema shape, revision and stored bindings. The schema-definition reference is supplied by the checked catalog owner; a caller-created `RegistrySnapshot` has no independent Session identity or proof of origin. These checks do not authenticate forged snapshots with matching revisions or same-size replacement definitions. Supplying the correct immutable catalog is an explicit private-layer precondition; public admission must establish that authority.

`create_recorded_queue` is offline bootstrap with existing parent directories. It creates the producer data/owner files and physically reserves a connection file before publishing the resources. A failed unpublished bootstrap is discarded as a staging unit. The producer's **committed tail**, the atomic end of its immutable accepted records, remains the sole acceptance authority. See the [glossary](glossary.md#proposed-independent-messaging-protocol) for producer stream, committed tail, publication capacity and logical credit.

The producer retains a native lifetime claim on the actual log inode; the consumer claims the actual connection inode. An inode identifies the underlying filesystem object, independently of a path name. Separate fork-registered claim descriptors prevent inherited data mappings from retaining a parent's claim. A paused owner cannot be displaced by timeout. Duplicate owners return `OwnershipBusy`; foreign-thread and fork-inherited calls return `WrongOwner` before data operations. Integrated public Task attachment and lifecycle remain later work.

The connection header binds both files' device/inode identities. Copying the connection file does not produce a second usable consumer claim, and a mismatched log or setup generation rejects at open before an incarnation advances. Destructive external mutation/replacement of live resources is unsupported. Operations use cached checked metadata, mappings and ownership; they do not consult Session or recheck paths per message.

## Connection file

The native file has a 4,096-byte prefix followed by `capacity * 64` descriptor bytes. The immutable header occupies the first 256 bytes; its little-endian fields are written explicitly rather than copying a C++ struct.

| Offset | Bytes | Immutable field |
| ---: | ---: | --- |
| 0 | 8 | ASCII `ANGACONN`. |
| 8 / 10 | 2 each | Format major/minor, 3.0. |
| 12 | 4 | Prefix length, 4,096. |
| 16 / 24 | 8 each | Session identity high/low halves. |
| 32 / 34 / 36 / 38 | 2 each | Queue, producer Task, consumer Task and Source Queue Set IDs. |
| 40 | 8 | Sealed catalog revision. |
| 48 / 52 | 2 / 4 | Message type and exact schema version. Bytes 50–51 are zero. |
| 56 / 64 | 8 each | Schema-definition reference and Queue topology revision. |
| 72 / 80 | 8 each | Nonzero connection generation and ring capacity. |
| 88 / 96 | 8 each | Producer log's device/inode identity. |
| 104 / 112 | 8 each | Producer log byte reservation and publication capacity. |
| 120 | 4 | Qualified native ABI profile. |
| 124 / 126 | 2 each | Synchronization revision 2 and atomic width 8. |
| 128 | 8 | Exact Binary payload size. |
| 136 | 8 | Required feature mask: 1 for recorded/unacknowledged, 3 for recorded with acknowledged progress. |
| 144 | 8 | Complete connection file length. |
| 152 / 160 | 8 each | Connection file's own device/inode identity. |
| 168 / 176 | 8 each | Acknowledged progress file device/inode; zero for unacknowledged bindings. |
| 184 | 72 | Reserved zero. |

The **connection generation** distinguishes the fixed checked connection setup; it does not advance on ring wrap or owner replacement. The file is not reconfigured while endpoints are live. Unsupported format, native ABI, synchronization, atomic-width or feature requirements return `UnsupportedPolicy` before control-word binding or ownership-state writes. Malformed magic returns `IoError`; a supported-format header with different context or reserved bytes returns `ConfigurationMismatch` before ownership-state writes.

Mutable words start in distinct 256-byte regions: returned credit at offset **256**, consumer incarnation at **512**, and, for acknowledged bindings, a separately committed processing-watermark hint at **768**. The incarnation advances under the consumer's claim before its identity can appear in returned messages and never wraps. Other prefix bytes are initialized to zero. The processing hint may lag committed progress and is never inferred from returned credit. Descriptors start at **4096**, with a 64-byte stride:

| Offset within slot | Bytes | Live field |
| ---: | ---: | --- |
| 0 | 8 | Queue-sequence stamp; zero initially. |
| 8 / 16 | 8 each | Producer-record offset and exclusive end. |
| 24 | 8 | Connection generation. |
| 32 | 32 | Unused zero padding. |

All live descriptor fields and control words use the qualified native atomic adapter. Slot `q` is `(q - 1) % capacity`. A descriptor is an acceleration hint, never acceptance authority or payload ownership. There is no descriptor occupancy head that can disagree with log catch-up.

## Send, credit and uncertain results

Let `A` be the last accepted Queue sequence, `C` the consumer-returned **logical credit** watermark, and `K` the ring capacity. A new sequence `q = A + 1` may be accepted only when `q - C <= K`; counters do not wrap. `A` comes from producer-local state rebuilt from accepted log records. `C` is monotonic and must remain within accepted progress.

`next_publication_id()` issues the caller's ID before sending. `try_send(id, message)` resolves an accepted ID first: identical meaningful input returns the original outcome even when credit or the log is full, while changed input is `Conflict`. Before credit admission, an unaccepted ID must have been issued by the current producer claim and must not have been retired by acceptance of a later ID. Ineligible IDs return `InvalidArgument` even when the Queue is full; returning credit cannot make them eligible. New input must match the bound exact Binary schema and pass its payload validation. `QueueCreditFull` is a typed, known-unaccepted result: no message is recorded and the caller may retry its still-valid ID after credit returns. `LogFull` separately denotes exhausted record/byte storage.

Acceptance is the log's single release-store of its new committed tail. The producer then publishes the reference/generation and release-publishes the slot stamp. If a later result or hint operation fails, acceptance is not undone: the result is `OutcomeUnknown`, and actor-owned `lookup(id)` resolves the retained record. The endpoint reconciles cached `A` from the log even when append itself returns uncertainty, before performing another credit check. No processing receipt, Session gate, common journal, mapping creation or file growth joins this normal path.

## Receive, catch-up and replacement

`try_receive()` expects the next Queue sequence. For a sequence **greater than returned credit**, its slot cannot yet be recycled: the consumer may acquire the matching stamp, read its atomic fields, then validate the committed record and generation. It loads the stamp before the relevant acquired log boundary, avoiding a stale boundary that predates a newly published descriptor.

For a sequence **at or below returned credit**, replacement replay bypasses the ring entirely because the producer may be rewriting that slot. A second stamp read is not a replacement for this rule. The payload always comes from immutable committed log bytes.

The reader's `poll_next(cursor)` walks already validated entries directly and validates at most one new committed record when its cursor is exhausted. It extends that cursor's cut by at most one record; copies used with ordinary `next()` remain frozen. No newly committed bytes means a bounded empty poll. This first profile's sole Queue makes the next producer record its next Queue record; multi-output routing needs its separately bounded scanning policy.

A missing hint therefore cannot hide acceptance. A consumer can read a committed record while its live producer is paused before publishing the descriptor. Returning credit lets the producer continue; a late old hint cannot cause duplicate live delivery. The forward cursor advances for every delivered record, including ring-hint matches, so later catch-up does not rescan delivered history.

The consumer prepares the complete bound `MessageView` before returning credit. Its header preserves publication identity/time, schema, flags and payload size, then adds Queue sequence, enqueue time, consumer incarnation and dequeue time. Returning credit certifies no application processing. The public borrow ends at the next receive invocation, including empty/error, or endpoint destruction. `copy_message()` preserves the entire observed header, payload and retained decoding definitions beyond that lifetime.

After a consumer-side operation fails following cursor progress, or stored validation fails, the endpoint fails closed until reopened. It does not silently skip a delivery or revive after bytes are repaired. A replacement starts from the retained Queue beginning and may duplicate earlier application delivery. It never decreases `C` or returns credit twice for previously credited records; only newly consumed sequences advance it. These guarantees concern process interruption, not machine-power-loss recovery or synchronous disk flushing.

## Restart and connection independence

The [restart tests](../test/test_recorded_queue_restart.cpp) terminate independent processes during actual mapped operations. Before the committed tail advances, a provisional or complete record is still unaccepted: a consumer must not deliver it, and a replacement producer may reuse that suffix. After the tail advances, the record remains accepted even if the producer dies before publishing a descriptor or returning its result. The replacement rebuilds accepted progress from the committed prefix, resolves accepted IDs, and continues Queue sequences under a new producer incarnation.

A live producer paused between commitment and descriptor publication keeps its lifetime claim. The consumer catches up from the log without taking over that claim. Resuming the original producer may publish a late hint, which must not deliver the same Queue sequence again.

Consumer termination is tested on both sides of the credit store. Death after the replay cursor advances but before credit publication leaves credit unchanged. Death after credit publication has already freed transport capacity, even if no message reached application code. A replacement deliberately starts at the retained beginning and can deliver a message again. It preserves the original publication identity and publication time, adds its own consumer incarnation and dequeue time, and never returns the same transport credit twice. An owning `Message` copied before endpoint exit retains the exact header observed at that delivery, its payload and its decoding definitions.

Stale **setup generations** and stale **descriptor generations** have different rejection points. Options which name a different immutable connection generation reject at open before incarnation writes. A published descriptor with a different generation fails closed before returning credit. The generation does not change when a process restarts. Resources remain fixed while endpoints are open; online reconfiguration, revocation of an established mapping, and destructive external mutation or replacement are unsupported. Normal communication therefore needs no per-message path or Session-generation scan.

The [connection-independence tests](../test/test_recorded_queue_independence.cpp) hold the actual Session administrative gate and establish that an unrelated connection can still issue IDs, send, resolve accepted outcomes and receive. Other cases leave a peer Queue full, exhaust its recording capacity, pause its append, or pause its replacement's recovery while another producer/consumer pair progresses. The connections share a Session identity and sealed catalog but have distinct Task, Queue, Source and producer-stream resources. This qualifies the private messaging layer; public Task admission and lifecycle integration remain later work. Independence does not promise immunity from host scheduling, memory pressure or shared hardware limits.

The private [`ProducerLogCheckpoint::RecoveryBeforeScan`](../src/producer_log.h) observer pauses cold writer opening after acquisition of the actual log-inode claim and before rebuilding the accepted prefix. It lets a test prove that recovery holds its affected stream's ownership while unrelated messaging continues. Killing that paused recovery releases its claim; a subsequent replacement can resolve accepted IDs, replay and continue sending while the Session gate remains held. Ordinary opens have no observer, and this seam adds no work to established send or receive. Recovery is allowed to scan the affected producer's retained history; established operations continue from cached checked state and forward cursors.

## Qualification and measurement

[Functional and independent-process tests](../test/test_recorded_queue.cpp) cover Queue order, small-ring wrap, full credit, accepted retries, empty polls, exact schema admission, owning copies, claims and committed-before-hint delivery. [Independent byte fixtures](../test/test_recorded_queue_format.cpp) check the connection header and reject unsupported/mismatched stored metadata without changing any resource bytes. [Producer-log tests](../test/test_producer_log.cpp) qualify bounded live validation, frozen cuts and permanent replay failure after corruption. The [focused performance test](../test/test_recorded_queue_performance.cpp) checks ordinary operation costs and zero successful-path allocation/filesystem/mapping/Session-gate work; wall-time observations have no noisy CI failure threshold.

Step 4.33 retains the [matched nanomsg comparison and latency/throughput study](../benchmarks/results/milestone-4-recorded-connection/README.md) at its pinned source commits. Recording, application acknowledgment, waiting and completion boundaries remain explicit in that comparison. Step 4.34 adds a [focused replacement/replay study](../benchmarks/results/milestone-4-recorded-restart/README.md) using the same-process performance test. For initial histories H=0/128/4,096, each invocation takes five fresh samples that add 64 baseline round trips before closing both endpoints, then separately measure producer/consumer replacement opening, replay of the retained history, the first 64 ID/send/lookup/receive round trips after catch-up, and a following 4,096-message stream. The first block after catch-up is not assumed to represent steady-state cost. Minor and major page-fault counts surround the baseline and both post-catch-up intervals, with counter sampling outside the timing intervals. Replay and live calls retain allocation and native-work guards; zero guarded filesystem or mapping calls does not establish zero virtual-memory work. Cold recovery and established message delivery have separate timing intervals; a recovery scan is not a per-message cost. These local block measurements do not establish independent-process capacity or nanomsg speedup. Diagnostic counters explain results; they do not replace latency and verified throughput. Optional acknowledged progress is workstream step 6; public admission follows later.
