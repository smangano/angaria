# Recorded delivery with optional acknowledged progress

Framework **0.4.37**, milestone step **4.35**, implements workstream step 6 of the [independent messaging plan](recorded-unacknowledged-delivery-plan.md#implementation-steps-for-review). The private [RecordedAcknowledgedConsumer](../src/recorded_queue.h) returns a `Delivery`: a borrowed `MessageView` and an exact `DeliveryToken`. Its immutable accepted messages use the same [producer-owned log](producer-log.md) as recorded/unacknowledged delivery.

**Transport credit** frees a descriptor slot. **Acknowledgment** records that the application explicitly completed the delivery. Neither receiving a message nor returning credit certifies that completion. Accepting a message remains the producer log's committed-tail store; processing progress has its own consumer-owned commit point. Acknowledgment does not erase the message or reclaim log space, and all progress storage is reserved before messaging so release, acknowledgment and replacement can finish when the message log is full.

This implementation remains private, with one Process producer, one Process consumer, one outgoing Queue, one Source input and one exact Binary schema per binding. Shared Task log ownership, Thread parent claims, public Task lifecycle and public profile admission remain later work. The current public runtime and its acknowledged default remain as documented by the [public receive API](native-receive.md). Workstream step 8 integrates the replacement into that API and its primer programs.

## Binding and ownership

`RecordedQueueOptions::_acknowledged` selects the private mode at offline creation. An acknowledged connection reserves a separate **progress file**, selected by `_progressPath` or `<connectionPath>.progress`. Unacknowledged bindings create no progress file or processing receipts. Their connection feature mask remains 1; acknowledged bindings use mask 3 and record the progress file's device/inode identity at connection-header offsets 168/176. The producer binds to that same mode without a separate recording implementation.

An inode identifies the actual filesystem object independently of its pathname. The consumer holds the connection's native lifetime claim and a second claim on the actual progress inode. The progress header binds its own identity, the connection identity and the sealed logical context. Copying a file does not create another admissible owner; multiply linked native files are rejected. Foreign-thread and fork-inherited operations return `WrongOwner`. A paused owner is not displaced by timeout.

The [ProgressStore](../src/recorded_progress.h) opens and maps reserved storage once. It does not acquire the Session gate or use its journal. Selected context, format, native ABI and synchronization are checked before progress repair or state writes. Connection generations remain immutable; destructive external replacement or mutation of live resources and online reconfiguration are unsupported. No per-message pathname or Session-generation scan is introduced.

## Progress file 1.0

This private subformat is **1.0** within a format-3 producer/connection binding. Integers are little endian. UTC values are signed 64-bit nanoseconds represented by their two's-complement bits; explicit presence flags distinguish an absent event from epoch zero or a negative timestamp. Mutable atomic words use the qualified eight-byte native adapter, not ordinary byte helpers. Plain working rows and receipt rows have one consumer owner and are not concurrent peer-read interfaces.

The file reserves `8192 + publicationCapacity * 64` bytes. The immutable first 256 bytes are:

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 8 | `ANGAPROG` magic. |
| 8 / 10 | 2 each | Progress major/minor, 1/0. |
| 12 | 4 | Receipt-area start, 8,192. |
| 16 / 24 | 8 each | Session identity high/low. |
| 32 / 34 / 36 | 2 each | Consumer Task, Source Queue Set and Queue IDs. |
| 38 | 2 | Zero. |
| 40 | 8 | Immutable connection generation. |
| 48 / 56 | 8 each | Connection device/inode identity. |
| 64 | 8 | Reserved publication/receipt capacity. |
| 72 / 80 | 8 each | This progress file's device/inode identity. |
| 88 | 8 | Complete reserved file length. |
| 96 | 4 | Qualified native ABI profile. |
| 100 / 102 | 2 each | Synchronization revision 2 and atomic width 8. |
| 104 | 8 | Required feature mask 1: acknowledged progress. |
| 112 | 144 | Reserved zero. |

The remaining fixed layout is:

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 256 | 8 | Atomic selected operation generation; zero initially. |
| 512 | 8 | Atomic applied generation; zero initially. |
| 1,024 | 80 | Installed Source/Queue working state. |
| 4,096 / 4,352 | 256 each | Operation image slots 0 and 1. |
| 8,192 | 64 per row | Receipt for Queue sequence `q` is row `q - 1`. |

Other reserved bytes start at zero. The 80-byte Source state is:

| Offset within state | Bytes | Meaning |
| ---: | ---: | --- |
| 0 / 8 | 8 each | Consumer incarnation and issuance counter. |
| 16 / 24 | 8 each | Pinned Queue sequence and its issuance; zero when idle. |
| 32 / 40 | 8 each | Contiguous acknowledged sequence and logical credit. |
| 48 | 8 | Presence bits: bit 0 pinned dequeue time, bit 1 last acknowledgment time. |
| 56 / 64 | 8 each | Captured dequeue and last acknowledgment times. |
| 72 | 8 | Reserved zero. |

An idle Source has no pinned sequence, pinned issuance or active dequeue time; its issuance counter is retained. A pin identifies `acknowledged + 1`; only one delivery can be in flight. Credit is monotonic and may be one sequence ahead of acknowledgment. A new consumer incarnation preserves credit, terminal progress and the last acknowledgment time, releases a predecessor pin, and begins its own issuance context.

The 64-byte receipt row is:

| Offset within row | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 8 | State: 0 Pending, 1 Issued, 2 Acknowledged. |
| 8 / 16 | 8 each | Latest consumer incarnation and issuance. |
| 24 | 8 | Presence bits: bit 0 dequeue time, bit 1 acknowledgment time. |
| 32 / 40 | 8 each | Latest dequeue and acknowledgment times. |
| 48 | 16 | Reserved zero. |

A zero row means never-issued Pending for an accepted destination. Row storage alone does not prove acceptance: the endpoint checks the producer's committed history before exposing `receipt(q)`. Release retains the earlier issuance/dequeue fields while restoring Pending. Acknowledgment preserves that issuance and adds its explicit terminal time. These rows record the latest progress, not every receive/release event; an optional execution trace is separate work.

Each 256-byte operation image contains:

| Offset within image | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 8 | Nonzero operation generation. |
| 8 / 16 / 24 | 8 each | Session high/low and immutable connection generation. |
| 32 / 34 / 36 | 2 each | Queue, Source and consumer Task IDs. |
| 38 | 2 | Zero. |
| 40 | 8 | Changed receipt sequence, or zero if no receipt changes. |
| 48 | 80 | Complete absolute Source after-state. |
| 128 | 64 | Complete absolute receipt after-state. |
| 192 | 64 | Reserved zero. |

## Commit, installation and recovery

A **Source operation generation** is independent of connection generation and Task incarnation. The selector stores the full generation; its parity chooses the image slot. Generation zero means no operation has committed. No generation or issuance counter wraps.

The sole owner first prepares and validates one complete after-image in local fixed-size storage. It writes that image to the inactive slot and publishes its generation through `AtomicWord::publish_control`. **The selector publication commits processing progress.** The primitive's full ordering barrier orders both the prior image stores and dependent working-row stores, including later reuse of the previously authoritative slot. Release/acquire ordering alone is not substituted for that reuse barrier.

The owner installs the image's receipt and Source after-values, then publishes applied generation through the same control primitive. It finishes this installation before beginning another image. Peers read neither partially installed plain rows nor inactive images. This is a fixed Source/receipt operation, not a general Session redo-command engine or a copy of the retained message ledger.

Before selector publication, an interrupted inactive image has no authority; a replacement retains the previous selected operation. After publication, the selected complete image is authoritative even if receipt/state installation or the return was interrupted. Recovery under the actual owner claim validates that selected image and repeats its absolute stores when applied generation lags. Invalid context, generations or installed state fail closed. An inactive image is never promoted because it happens to look complete.

**Applied generation certifies row installation, not hint publication.** Attachment and uncertain-operation settlement reconcile the connection's credit and acknowledgment hints from committed Source state even when applied already equals selected. Reconciliation is monotonic and bounded by accepted progress. The acknowledged-watermark hint occupies the connection's atomic word at **768**. `RecordedProducer::acknowledged_sequence()` can observe this lagging committed hint; it never interprets transport credit as acknowledgment. Exact latest receipt inspection is owner-only through the consumer, not an unsynchronized read of working rows.

## Delivery, token and borrow behavior

| Operation | Result |
| --- | --- |
| Fresh receive | Read the oldest pending accepted record, prepare its bound header/view, reserve a terminal-resolution generation, and commit a new issuance and pin. Return credit after that commit, then return `Delivery`. |
| Receive while pinned | Return the same token, original dequeue time and view without another issuance, image commit or receipt change. |
| Acknowledge | Match every token field, mark that receipt Acknowledged, advance the contiguous watermark, capture acknowledgment time and clear the pin in one operation. |
| Release | Match the exact token, restore Pending, preserve historical issuance/dequeue fields, and clear the pin without advancing acknowledgment. |
| Close | Release an active pin before ending the borrow and owner lifetime; idle close needs no image write. Destruction attempts close and releases local ownership; replacement repairs any unsettled persisted pin. |
| Replacement | Repair selected progress, reconcile hints, preserve terminal progress and restore a predecessor's pin to Pending. Cold positioning walks this affected producer's acknowledged prefix once; ordinary receives then advance the saved cursor. |

The token contains Session, consumer Task/incarnation, Source, Queue, Queue sequence and issuance. Stale, duplicate or foreign tokens return `StaleDelivery` and cannot complete another issuance. A fresh issue requires room for its own generation **and one terminal resolution**; an existing pin remains repeatable and resolvable when fresh issuance is exhausted.

An unchanged failure before progress commitment preserves an existing pin and its advertised borrow. An uncertain committed operation returns `OutcomeUnknown`; stop using that earlier borrow until `try_receive()` or `close()` settles the Source. Settlement preserves captured token/timestamps and does not turn a committed acknowledgment into redelivery. Stored validation failures fail the affected endpoint closed; a failed handle is not revived by repairing bytes. Successful acknowledgment, release, close or owner destruction ends the borrow. `copy_message()` while valid retains the complete observed header, independent bytes and exact decoding context without prolonging the token obligation.

## Qualification and performance

[Storage tests](../test/test_recorded_progress.cpp) use independent header/image/row bytes, explicit epoch-zero/negative times, staged errors, exclusive-inode ownership and rejection without writes. A qualification checkpoint can stop Source installation after 40 of its 80 bytes, so ordinary issuance, release and acknowledgment exercise genuinely mixed old/new fields before selected-image repair. [Endpoint/process tests](../test/test_recorded_acknowledged_queue.cpp) cover exact tokens, pin repetition, release/redelivery, log-full progress, generation reservation, uncertainty and real process termination across image, selector, row, applied-generation and hint boundaries. Repeated recovery and old-slot reuse preserve the original acknowledgment time. Connection-independence cases exercise acknowledged traffic while administrative or unrelated connection state is held.

The [matched comparison adapter](../benchmarks/ipc_comparison.cpp) includes a `recorded-acknowledged-connection` profile alongside recorded/unacknowledged delivery and classic nanomsg IPC. Its verified completion interval includes ordinary producer acceptance lookup and consumer acknowledgment completion. Nanomsg provides unrecorded/unacknowledged delivery in that comparison, so different guarantees and waiting/buffering remain explicit. Focused performance guards check allocation and native filesystem/mapping/Session-gate work; ordinary wall-time runs report delivered-message latency and completed-publication throughput separately from diagnostics. Expected added acknowledgment cost is bounded local progress work per operation, rather than a Session transaction or a rescan of retained messages. Measurement results belong to their retained study and do not imply production tail or sustained-capacity guarantees.

The [retained 0.4.37 measurements](../benchmarks/results/milestone-4-recorded-acknowledged/README.md) include separate acknowledged and unacknowledged matrices on native macOS ARM64 and hosted Linux x86_64, with delivery and consumer acknowledgment-return latency, common-T0 and active completed rates, all attempt ranges and exact workload/build provenance. Different Linux CPU models keep the two modes' results separate; no isolated acknowledgment-overhead ratio is inferred.

These guarantees concern process interruption on a running coherent local system. Synchronous flushing, recovery from power loss and live log reclamation are separate concerns; acknowledgment does not introduce them.
