# Public C++ API contract

Framework **0.4.39**, milestone step **4.37**, admits producer-owned recorded messaging through the public API. The current native format is **3.0**, synchronization revision **2**. Require a fresh development Session for this breaking pre-1.0 change; old formats are rejected rather than migrated or interpreted by another engine.

The [programmer primer](primer.md) contains eighteen runnable lessons using these public headers and explicit Result checks. The [complete public interface review](public-interface.md) also contains proposed full-roadmap APIs. Its isolated `angaria::proposed` declarations are compile-only; a matching declaration does not imply a runtime implementation. The earlier v0 API and Session-gated messaging engine are retired.

## Headers

| Header | Contract |
| --- | --- |
| `angaria/identities.h` | Typed entity IDs, schemas, Session/Task claims, publication IDs and exact delivery tokens. |
| `angaria/descriptors.h` | Owning registration descriptors, Session limits and policy settings. |
| `angaria/results.h` | Typed failures, owned publication outcomes and caller-buffer reports. |
| `angaria/message.h` | Consistent MessageHeader in owning Message and borrowed MessageView; Delivery wraps a view and token. |
| `angaria/runtime.h` | Session, Task, QueueWriter and SourceQueueSet operations; Publishing messaging remains unsupported. |
| `angaria/registry.h` | Definition registration, discovery, activation and current/historical snapshots. |
| `angaria/payload.h` | Exact schema-driven Binary payload codecs and supported scalar/logical values. |
| `angaria/message_record.h` | Bounded portable publication framing, independently versioned from native storage. |
| `angaria/capture.h` | Bounded readers/writers of flat portable records through caller-owned streams. |
| `angaria/inspection.h` | Owning exact schema catalogs and generic bounded text rendering. |
| `angaria/log.h` | Frozen accepted-producer cuts, owning LogRecords, filtered cursors, rewind, inspection and flat export. |

## Identities and tokens

Task, Queue, Source Queue Set, Publishing Queue Set and Message type IDs are distinct 16-bit C++ types; zero is invalid. Source and Publishing IDs share the Queue Set namespace. Field IDs have 32 bits and require the `0xEE` high byte. A schema is an exact message type/version pair. A Session has an opaque 128-bit identity.

A TaskClaim identifies its Session, logical Task and incarnation. PublicationId adds that producer incarnation's sequence. Queue sequence is separate: it records acceptance order on one connection and continues across producer replacement. Several outgoing Queues share one Task-owned log and ID issuer, so their Queue sequences must not be confused with the producer's publication sequence.

For acknowledged delivery, DeliveryToken contains the exact consumer claim, Source, Queue, Queue sequence and issuance. Repeated receive retains the current token. Release, close/reopen or consumer replacement can issue the same message under a new token; an old token cannot finish that new issuance. Explicitly unacknowledged delivery returns MessageView and creates no token obligation.

## Session creation and Registry attribution

`Session::create(options, creator)` creates a new exclusive directory and returns CreatedSession with its Session and creator Task. The creator is a Process Task, consumes Task ID 1 and supplies initial Registry attribution. `Session::attach(directory)` validates the current native format and supported ABI before access. The supported profiles are Linux x86_64/ARM64 and macOS ARM64; macOS x86_64 is request-only qualification.

SessionOptions `_maxLogBytes` bounds administrative definitions/journal bytes. `_producerLogBytes` and `_producerLogPublications` bound each producing Task's accepted-message reservation separately, defaulting to 64 MiB and 65,536. Bounds bind at creation. Returning Queue credit or acknowledging processing does not reclaim immutable log space.

`Session::registry()` supplies an inspection view without mutation authority. `Task::registry()` retains that Task's claim and supplies its actor attribution for permitted setup operations. Exact registration retries preserve definitions; changed descriptors under the same identity conflict. Current Task liveness and claims are administrative diagnostics, not the authority to infer message acceptance or processing completion.

## File-based Sessions

`Session::create_from_file(configuration, creatorName)` parses TOML through cpptoml, validates the complete effective setup and publishes no partial Session on failure. `Session::attach_from_file(configuration)` validates immutable effective settings and supplied definitions without registering or changing them. Relative Session paths resolve from the configuration file's parent. See [the complete configuration schema](configuration.md), including acknowledgment overrides and producer-log reservations.

## Capacity defaults and named setters

`CapacityLimits{}` starts with the bounded [C++ default profile](configuration.md#capacity-limits). `SessionOptions::_limits` uses that profile. Override individual capacities through named setters:

```cpp
auto limits = CapacityLimits{}.tasks(16).queues(128).source_sets(16);
```

Unspecified capacities retain their defaults. Setters modify an existing object and support chaining; chaining on a temporary returns a value that can be stored safely. Positional capacity lists are not supported. Setter names follow the project's `snake_case` convention.

`CapacityLimits::empty()` explicitly sets every capacity to zero. Individual facilities can also be disabled with a zero-valued setter. All settings still pass through normal validation; an empty profile cannot create a session until it has room for its creator Task and required resources.

These defaults apply to new programmatic sessions. TOML capacity keys remain required. Existing-session attachment uses the recorded effective limits and validates any supplied configuration against them.

## Registration and activation

Register the complete catalog of Tasks, Fields, exact message schemas, Queues and Queue Sets before starting messaging. Source and Publishing metadata may describe later roadmap topologies; current receiving selects one Active Queue, and Publishing delivery is unsupported. Activate complete connections in dependency order. Before messaging starts, supported Registry APIs can finish draft setup and edit permitted metadata.

The first successful endpoint opening or publication-ID issuance seals an authoritative, checked catalog. New registrations and topology edits then fail rather than changing established bindings or schema caches. Exact unchanged registration retries remain idempotent. All supported registered exact Binary schemas are cached; MessageHeader selects the required decoder. A Task's outgoing QueueWriter handles share one producer-log owner and issuance/recovery state.

## Current and historical Registry snapshots

`Registry::snapshot(revision)` returns an owning coherent definition snapshot: Tasks, Fields, exact schemas, topology states and membership. Omission selects current Registry revision; zero is InvalidArgument and a future revision is NotFound. Historical cuts preserve definitions and topology at that Registry revision. They do not reconstruct earlier Task liveness, message-log tails or consumer receive order.

Snapshots retain no Task claim and remain usable after native handles close. Session attachment and snapshot acquisition may validate/recover administrative state. General foreign-ABI or physically read-only Session attachment is not supplied by these APIs.

## Endpoint ownership and lifetime

Session, Task and endpoint handles are movable. Task, actor-bound Registry views and endpoints retain their applicable claims. A Process Task serializes individual shared producer operations locally; applications preserve their intended ID-issuance/send order and synchronize their own mutable buffers and use of a common Source. A Thread Task belongs to its attaching thread and retains its registered parent Process claim. Claim replacement advances incarnation after the old lifetime owner is gone.

A fork child **must exec before using any Angaria API**, including a fresh Session create/attach. Existing inherited state is not a supported shortcut. Empty/moved-from or ownership-invalid handles return the documented typed errors on Result operations; plain identity/accessor operations can throw std::logic_error.

Opening a Draft endpoint returns NotActive; a mismatched owner returns WrongOwner. An already open Source for the same live claim returns OwnershipBusy. Acknowledged close releases its current pin without acknowledging it; unacknowledged close ends its borrow. Copies made while a MessageView is valid keep their complete observed header, owned payload and exact decoding context after endpoint closure.

Established ID issuance, send/status, receive, acknowledgment/release/close and committed watermark reads do not acquire the Session administrative gate. Administrative setup, admission and Task claim work still use their own coordination. This is independent messaging ownership, not a promise that every operation is lock-free or has bounded wall-clock latency.

## Binary payload codecs

Step 3.1 implements `BinaryPayloadCodec`, compiled from an exact `SchemaId` and an explicit payload byte cap. `from_registry` takes one consistent snapshot; `from_snapshot` also accepts a previously captured catalog. The codec owns resolved Field descriptors and their order, so it retains no Session, Task claim, or Registry view. `schema()` and `payload_size()` identify its layout; `fields()` borrows its ordered descriptors for the codec's lifetime.

`PayloadValue` distinguishes char, byte, signed int8 and signed/unsigned 16-/32-/64-bit integers, float/double, `bool`, `BitFieldValue`, `EnumValue`, and `FixedAsciiValue`. `ScalarValue` remains an alias for `PayloadValue`; earlier scalar examples still compile. Exhaustive variant visitors need cases for the new alternatives. Enum values wrap an `IntegerValue` of the exact underlying width/sign; fixed ASCII owns its text bytes. `encode` accepts exactly one matching alternative for each Field occurrence and returns owning bytes. `decode` requires exactly the compiled payload length and returns owning values. Fields are packed in schema order with explicit little-endian integer and IEEE floating-point representations. Repeated Fields and empty schemas remain meaningful, and newer versions do not change older codecs.

`encode({values...})` provides initializer-list construction under the codec's byte cap. `encode_into(values, output)` requires a byte span of exactly `payload_size()` bytes and returns `OperationResult`; all validation/allocation completes before copying, so errors leave caller output unchanged. These APIs construct application-memory payloads. Step 3.4 wraps them in portable message records below; step 4.6 accepts these payload bytes through direct Queue sends. `validate(payload)` checks the exact packed representation without constructing decoded values and is used by Binary send admission.

Malformed or ambiguous selected definitions, invalid widths, mismatched values, and incorrect byte lengths return `InvalidArgument`. Missing exact schemas/referenced Fields return `NotFound`; recognized unavailable Field representations return `UnsupportedPolicy`; byte/allocation limits return `CapacityExceeded`. A zero cap is valid for an empty schema. Full layout, lifetime, and error rules are in [the payload contract](message-payload.md), demonstrated by the [ninth primer program](../examples/primer/09_payload.cpp).

## Canonical message records

Step 3.4 implements `MessageHeader`, `MessageRecord`, `MessageRecordView`, and `MessageRecordCodec`. The header owns an exact schema, payload format/byte count, full producer `PublicationId`, initial publication UTC time, and user flags. Encoding version 1 uses an explicit 80-byte little-endian header followed immediately by the payload. This portable record has no native offsets or destination receipts and has an encoding version independent of native Session format 3.0.

Every header/record encode or decode requires `maxRecordBytes`, including header and payload. `encode_header` and `decode_header` operate on exactly 80 header bytes and validate the announced total before payload allocation. `encode` returns owning record bytes; `decode_view` borrows only the input payload; `decode` owns an independent header/vector. Whole-record decoding requires exactly one complete record. `encode_into` accepts the exact output span and preserves it on errors, including overlapping payload/output spans.

All five stable payload-format codes are preserved as opaque bytes; JSON/XML/CSV/text serializers are not implied. `decode_binary(record, codec, schemaSession)` additionally requires Binary, the exact codec schema, and a matching independently retained source Session identity. A same-numbered schema from another Session or a different same-sized version returns `SchemaConflict`. The supplied definitions remain caller-selected, with no authentication of their origin or proof of publication admission.

`MessageRecord::view()` is allowed on lvalues and borrows their payload storage. `MessageRecordView::message()` preserves the complete header in a borrowed `MessageView` with the same backing-storage lifetime. Encoding version 1 rejects any optional destination metadata it cannot store. Initial publication time is distinct from the optional per-destination enqueue/dequeue metadata in MessageHeader. Complete bytes, errors, lifetime, and receipt boundaries are specified in [the message-record contract](message-record.md) and demonstrated by [primer lesson 11](primer.md#11-wrap-a-payload-in-a-message-record).

These codecs construct application-memory bytes. MessageView is the borrowed input consumed by direct send; codec encoding does not issue publication identities or perform delivery.

## Capture and generic inspection

Step 3.5 implements `MessageCaptureWriter`, `MessageCaptureReader`, and `MessageInspector`. A [capture](capture-inspection.md) contains consecutive canonical records, with no additional file header or embedded schema catalog. Stream wrappers borrow a caller-owned stream and require record/aggregate byte caps. Writers validate before output; a partial I/O failure is terminal. Readers return an owning record, clean EOF, or a terminal contextual error. Counters count only complete successful records from the stream's initial position.

`MessageInspector::from_session(session, limits)` obtains one consistent Registry snapshot. `from_snapshot(schemaSession, snapshot, limits)` accepts an independently retained Session identity and definitions. Inspectors own only Field/schema definitions and retain no Session or Task claim. Copies remain independent; moved-from inspectors return `InvalidState`. Later definitions require a new inspector. Native snapshot acquisition uses the ordinary Session API before inspector catalog preflight.

`inspect(recordBytes)` or `inspect(recordView)` validates framing, source Session, and exact schema version, then returns owning ASCII text. It decodes existing binary scalar/logical Fields without application-specific types. Names, labels, and values are escaped; Field order, enum numbers/labels, bit widths, ASCII padding, and IEEE special values remain visible. Missing schemas return `NotFound`, a different Session returns `SchemaConflict`, and known nonbinary formats return `UnsupportedPolicy`.

`InspectionLimits` bounds whole-record bytes, retained/expanded catalog definitions, and per-record text. Catalog preflight precedes inspector copies; repeated Field definitions are charged before compiling a selected layout. These size budgets do not measure exact process memory. The `angaria_inspect` CLI adds aggregate capture/output limits and closes native handles before capture I/O. Native catalog attachment can recover/write the Session and requires a compatible profile. See the [complete contract](capture-inspection.md) and [primer lesson 12](primer.md#12-capture-and-inspect-multiple-message-types).

Step 3.6's [process qualification](process-exchange.md) uses these same APIs in independent executables. A retained inspector keeps its original schema catalog while other processes add a version and replace a producer claim. Cold inspectors read every retained version after applications exit. This step adds an example and qualification coverage without changing public API or persisted formats.

## Messages and receive events

Direct sending accepts MessageView with a registered exact Binary schema, payload byte count and flags. The explicit PublicationId and runtime clocks establish stored provenance; sending leaves the caller's input header unchanged. A valid received view has the same MessageHeader model, adding its observed Queue/consumer/time context. `copy_message(maxBytes)` preserves that observed header and owns independent bytes; it does not manufacture a token or advance processing.

`ReceiveResult` has `Delivery`, `MessageView`, `DeliveryGap`, `ReceiveTimeout` and `Error` alternatives. Required acknowledgment, the default, returns Delivery and retains one pin until acknowledge/release/close. None returns MessageView without processing certification; its next receive attempt ends the preceding borrow. Neither returned credit nor acknowledgment erases accepted messages. A replacement unacknowledged consumer can replay them again. DeliveryGap remains vocabulary for future stale-data loss; `allow_loss` is currently UnsupportedPolicy.

Zero-time receive makes one nonblocking attempt; positive Spin receive polls until its monotonic deadline. Negative or overflowing durations reject. A timeout means this call found no delivery before its deadline; a producer can accept a message afterward. Current Source receiving requires one Active input Queue. Publishing send/retry and multiple-input selection remain later work.

## Publication outcomes and output buffers

`QueueWriter::send(id, message)` and `Task::publication_status(id)` return Result<PublicationOutcome> with an owned destination vector. Buffer-taking overloads return PublicationReport borrowing caller-supplied destination storage. Check `_error` before interpreting `_status`; an undersized send buffer rejects before a new acceptance, while an undersized lookup buffer does not prove rejection of the original publication.

| Outcome | Meaning |
| --- | --- |
| Accepted | The selected destination accepted and the immutable record committed. This does not certify application processing. |
| Rejected | No destination accepted this attempt. An issued but unused ID lookup has no destinations. |
| Partial | Vocabulary for later Publishing acceptance; current direct sending has one destination. |
| OutcomeUnknown | Final acceptance could not be established. Retain the original ID and resolve it with status lookup. |

A full logical Queue returns a known rejected destination with QueueFull, without recording a new publication or retiring an otherwise eligible ID. Let its consumer return credit and retry that same eligible ID. Producer-byte or publication-count exhaustion returns LogFull. Identical repeats of an accepted current-claim ID return its original outcome; changed content conflicts. A replacement can query accepted IDs from the same Task's earlier incarnations, but fresh sending requires its current claim. No history reclamation occurs in this runtime.

Accepted DestinationOutcome values include optional `_queueSequence`. `QueueWriter::acknowledged_sequence()` returns a committed processing lower bound for that same Queue; it may lag after interruption. Comparing that watermark with an accepted destination's Queue sequence lets a producer observe certified progress. Compare neither another Queue nor PublicationId's producer sequence. Unacknowledged mode returns UnsupportedPolicy for this query. It is separate from send acceptance and transport credit, and a caller must not turn a lagging watermark into a rejection claim.

## Frozen accepted logs and replay

`LogArchive::capture_live(session, limits)` captures a vector of immutable accepted prefixes from an already admitted Session. It does not attach a Task, seal setup, recover a producer log, change consumer receipts or create delivery obligations. Administrative Session attachment/capture setup still requires the supported native profile. Each producer has its own committed-tail cut; there is no global simultaneous cut or original consumer execution order.

`publications(filter)` creates a LogCursor, selecting by producer Task, exact SchemaId and/or accepted Queue. Between producers it iterates Task-ID order; within a producer and each Queue it preserves accepted record order. `read_next()` returns an owning LogRecord, end-of-cut as an empty optional, or Error. The record owns its Message with publication header and bytes, plus accepted destination context including Queue sequence and enqueue time. It contains no fabricated consumer/dequeue observations, processing receipts or live DeliveryToken. `rewind()` restarts the same cut, not a new live capture. Owning records survive cursor/archive/Session closure; backing files must remain in place while archives read them.

ArchiveLimits bound native encoded record bytes, the sum of configured producer publication capacities before read-index allocation, and the sum of fixed producer-file reservations before mapping. These conservative representation caps are not exact resident-memory measurements. `catalog()` supplies the matching owning RegistrySnapshot. `inspect(stream)` uses MessageInspector for exact schema names, Field names/types and decoded values alongside native accepted-destination context. ArchiveLimits `_maxTextBytes` defaults to 16 MiB for the aggregate rendered output, including the summary and each complete record unit; a CapacityExceeded error leaves only complete units. The inspector also retains its separate default 16 MiB catalog budget; an I/O failure may leave a partial final output unit. `export_capture(stream)` writes ordinary flat portable publication records, with no receipt/catalog bundle. Keep the exact source catalog for decoding; I/O failure can leave a complete prefix and partial final record in the caller-owned output stream.

[Primer lesson 18](primer.md#18-replay-accepted-messages-and-export-a-capture) runs this API over the three real accepted messages from lesson 15, then inspects the export with the existing public CLI. It replays messages into application code without mutating live progress. Advanced replay pacing/checkpoints, execution tracing, receipt snapshots, republishing and self-contained catalog bundles remain proposed; ordinary message logs cannot reconstruct the exact order in which a consumer executed handlers.

## Validation

Public lifecycle and process tests cover shared producer ownership, several outgoing Queues, exact schemas, Process/Thread parent claims, known backpressure, interruption/replacement, committed progress and independence from the Session gate. Log tests cover frozen cuts, filters, rewind, limits, owning records, export and inspection. Performance checks measure the real public API against nanomsg with recording and acknowledgment boundaries labeled. See [public runtime admission](public-runtime-admission.md), [performance qualification](public-runtime-performance.md) and the [configuration contract](configuration.md).
