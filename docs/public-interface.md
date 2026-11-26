# Reviewing Angaria's complete public interface

This is a proposal for the application interface through the **1.0.0 roadmap**, including features whose implementation has not started. It is intended for review before more runtime machinery is built. Steps 4.6–4.7 now implement the approved direct-send path, shared message model, and basic Source delivery/close; the status table below separates that subset from later work.

The proposed declarations live in **`angaria::proposed`**, separately from the current `angaria` API:

- [Application setup, Registry, endpoints, and diagnostics](../include/angaria/proposed/core.h).
- [Complete Fields, values, pools, and serialization](../include/angaria/proposed/schema.h).
- [Log inspection, export, and replay](../include/angaria/proposed/log.h).
- [Application and debugging examples](../examples/api_review.cpp).

These are review declarations and compile-only examples. They are not linked applications, runtime substitutes, or new runnable primer lessons. The [programmer primer](primer.md) remains the guide to features users can run today. The [existing API contract](api.md), [roadmap](../ROADMAP.md), and [original Wiki](https://github.com/smangano/angaria/wiki/Angaria) provide the background for this proposal.

The implemented reference remains available alongside the proposal: [typed identities](../include/angaria/identities.h), [results](../include/angaria/results.h), [configuration/policy descriptors](../include/angaria/descriptors.h), [scalar/logical payload codec](../include/angaria/payload.h), [canonical record codec](../include/angaria/message_record.h), [ordinary capture streams](../include/angaria/capture.h), and [generic inspector](../include/angaria/inspection.h). The review header [api.h](../include/angaria/proposed/api.h) collects the proposed application/schema/log surface; it does not replace those implemented codecs or silently change their representations.

## The ordinary application path

Most applications should work with five things: a Session, a Task claim, message schemas, an output endpoint, and a Source Queue Set. Configuration and endpoint lookup happen once during setup. The application's processing loop should not need record envelopes, native offsets, journal commands, mapping leases, or recovery checkpoints.

A **Session** is the cooperating programs' persistent workspace. A **Task claim** is exclusive attachment to a named logical role by a process or thread. A **Source Queue Set** is a consumer's input collection; a **Publishing Queue Set** is a producer's destination list. A message schema describes how that message's data is represented. The [glossary](glossary.md) defines the remaining shared terms.

For the small trading system used in the examples, a practical workflow is:

1. An initialization program creates a Session from TOML, naming its creator Task. This registers schemas, Tasks, Queues, both Queue Set kinds, and policies together. Programmatic Registry setup remains available when an application needs it.
2. Each cooperating process attaches to that Session and claims its named Task. A Thread Task also requires its local parent Process claim.
3. The process opens named endpoints once and obtains the exact schemas/codecs it uses.
4. Producers encode messages, obtain publication identities, and send or publish them.
5. Consumers receive from their Source Queue Sets, process each message, and explicitly acknowledge or release its exact token.
6. Applications close Sources explicitly when they need to observe cleanup errors, then release their other handles.

Name overloads remove repeated `find_task`/`find_queue`/`find_source_set` boilerplate from setup. The typed-ID overloads remain useful when a program already has IDs or needs to keep names out of its processing loop. An endpoint retains its Task claim, so moving or closing the original Task variable does not silently end the endpoint's ownership.

The shorter producer path is:

```cpp
auto session = value(api::Session::attach("trading-session"));
auto feed = value(session.attach_task("Feed"));
auto writer = value(feed.open_writer("FeedToStrategy"));
auto id = value(feed.next_publication_id());
remember_request(id, message); // Application persists the ID and input as needed.
auto result = writer.send(id, message);
```

Here `api` means `angaria::proposed`, `message` is a prepared `MessageView`, and `value` is the example's error-checking helper. The [complete example](../examples/api_review.cpp) includes schema encoding, all receive alternatives, explicit close, partial retry, uncertainty handling, diagnostics, log inspection, replay, and isolated republishing. The angaria::proposed example remains compile-only. The direct-send sequence is runnable in the production angaria namespace; primer lessons 14–15 add complete native send, receive, copy, and acknowledgment examples.

TOML is the common configuration format. YAML, JSON, and XML support is through converters into that same validated TOML model. Creating a Session remains distinct from attaching to one: a misspelled attachment path must not create a new, empty communication system.

### Sending one message

A producer first obtains a `PublicationId`, then passes that identity and a `MessageView` to its output endpoint. The view's `_header` supplies its exact schema, payload format, payload byte count, and user flags; `_payload` borrows the application bytes. The send operation borrows those bytes only for the call; an accepted message retains its own stored bytes. Transport-owned publication and delivery metadata describes the accepted message, rather than being inferred from the caller's payload.

A newly prepared header has an invalid default publication identity. `_publishedAt` is meaningful only when publication metadata is present; epoch zero alone never means absence. Sending uses the explicit request ID and the runtime clock to establish the new authoritative publication header. It reads schema, format, payload byte count, flags, and payload from the input view, without changing that view's header. If a stored message view is reused as new input, its old publication/delivery fields remain inspection data; they do not override the explicit new request identity or create destination metadata for the new publication.

The ordinary direct-send overload now returns an **owning `PublicationOutcome`** through `Result<PublicationOutcome>`. Owning means that its destination results remain usable after the next operation or after the original output variables leave scope. The existing buffer-taking overload remains the path for callers that want to supply and reuse their own destination storage.

Its `AcceptanceStatus` contains only `Accepted`, `Partial`, or `Rejected`. Uncertainty uses `ErrorCode::OutcomeUnknown` in the outer `Result`, rather than a second unknown-status channel inside an owning result. The low-level report keeps its original representation.

The publication identity stays explicit in both forms. The application can retain it before attempting the operation and use it to resolve an interrupted result. A convenience method must not hide the only identity needed after an uncertain send.

For a direct Queue there is one destination. For a Publishing Queue Set the snapshot can contain several destinations, but the producer still sends one immutable message. Its acceptance policy controls whether every destination must accept together or whether partial acceptance is permitted. Neither endpoint turns a full overwriteable ring into a new pending-message limit.

### Receiving and processing

`SourceQueueSet::receive` returns a `Delivery`, an allowed-loss gap, a timeout, or an error. A gap and a timeout are distinct events; neither owns a delivery slot. Source selection policy chooses among its Queues while preserving each Queue's message order.

The names describe ownership and delivery separately:

| Type | Purpose |
| --- | --- |
| `Message` | Owns its header and payload bytes, together with the dependencies needed to decode them later. |
| `MessageView` | Holds the same header value and borrows read-only payload bytes. |
| `Delivery` | Contains a `MessageView` in `_message` and the current acknowledgment/release token in `_token`. |

Both message types expose a consistent `MessageHeader` as `_header`. Its common publication fields are schema, format, publication identity, publication time, payload byte count, and application flags. Its destination fields are Queue, Queue sequence, consumer claim, enqueue time, and dequeue time. The destination fields are optional when no particular destination has been selected, as with a publication-only log record; a message received in a `Delivery` has those fields populated. Optional timestamps distinguish absence from a valid epoch-zero time.

This is one logical C++ header. Its immutable publication information and destination-specific delivery information may come from different stored records; its public shape does not dictate the native or portable byte layout. In particular, each destination of a fan-out publication can have different enqueue/dequeue times while retaining the same original publication identity.

An application inspects either message type through the same member path:

```cpp
const auto originalHeader = delivery._message._header;
auto saved = value(delivery._message.copy_message(maxPayloadBytes));
value(inputs.acknowledge(delivery._token));

// saved._header preserves originalHeader, including its delivery metadata.
// saved._payload and its decoding dependencies remain usable here.
```

The ordinary receive path borrows the payload through the Source's in-flight ownership. Applications that need it after acknowledgment explicitly copy the `MessageView` into a `Message`. `copy_message` preserves the **entire header unchanged**, copies the payload, and retains source schema/pool context, so later reclamation cannot invalidate referenced values. `Message::view()` exposes that same header with a borrowed span over the owning payload. Copying a view alone does not extend the payload's lifetime.

`EncodedPayload::view()` also carries decoding context for prepared output. A raw header/span view constructed by an application is suitable for sending but has no inferred Session/catalog binding; its `copy_message` returns `InvalidState` if that binding is absent. In the proposed interface, library-provided delivery, log, and encoded-payload views carry the required decoding dependencies while their payload remains borrowed. `Message::view()` propagates its owner's existing binding; manually constructing a `Message` does not infer one. Step 4.7 now supplies bound incoming views through public receive; owning copies preserve their complete headers and current exact decoding context.

The private retention helper keeps decoding dependencies alive; it neither keeps the delivery in flight nor copies an active acknowledgment token into the owning `Message`. Consumer and delivery fields retained in its header are historical information. The ordinary path avoids copying every payload solely to make its lifetime invisible.

Acknowledgment is explicit and happens after processing succeeds. Release makes the message available for another issuance. A destructor must not acknowledge an unprocessed message. Repeated receive while a delivery is already in flight returns the same issuance and time; an old token cannot acknowledge a subsequent redelivery.

The payload view expires when acknowledgment, release, valid Source closure, or claim termination ends that delivery's lifetime. Moving a handle transfers its current ownership. A caller finishes reading before an operation that ends its view. `SourceQueueSet::close()` reports cleanup failure; the reviewed deferred-cleanup rules still apply to destruction and move replacement.

### Partial delivery and interrupted operations

The interface keeps three questions separate:

| Question | Application action |
| --- | --- |
| Did the operation return an ordinary error? | Inspect the error before interpreting any acceptance result. A malformed request or an insufficient output buffer is not a valid acceptance report. |
| Did only some destinations accept? | Inspect the recorded destination outcomes. Use `PublishingQueueSet::retry` for the still-rejected destinations of that retained publication. Already accepted destinations receive no additional obligation. |
| Is the result uncertain? | Retain the original `PublicationId` and query `Task::publication_status`. Do not assume that an error means rejection and send a second identity automatically. |

Low-level `PublicationReport` keeps its existing independent `_error` and `_status` fields and borrows caller-provided output storage. Callers check `_error` first. The implemented direct-send owning overload expresses the same facts without requiring a caller to manage a report's span lifetime.

Lookup and retry can address a retained publication from an earlier incarnation of the same logical producer. Reclamation can make history unavailable; `HistoryUnavailable` must not be interpreted as proof that a publication was rejected. The API does not hide this distinction behind an automatic retry loop.

## The complete interface inventory

The headers are the full declaration reference. This table explains the application purpose and separates existing behavior from the proposed additions. **Implemented** means available through the current public runtime; a declaration or a private qualification fixture alone does not establish public support.

| Area | Application-facing surface | Status and intended scope |
| --- | --- | --- |
| Identities and results | Distinct Task/Queue/Queue Set/Field IDs, exact schema versions, Session identities, Task claims, publication IDs, delivery tokens, ordinary errors. | Existing public value types. The proposal reuses their meanings. |
| Configuration | Session options and named capacities; TOML creation/attachment; configuration validation and diagnostics; format converters. | Options and TOML factories implemented. Standalone validation/conversion interfaces are proposed; alternate formats still converge on TOML. |
| Session lifecycle | Exclusive creation, attachment, metadata/effective options, Task claims, capability/platform information. | Core lifecycle and name conveniences implemented. A complete capability/diagnostic surface remains proposed. |
| Registry | Register/find/describe definitions, activate topology, adopt drafts, assign Sources, replace Publishing destinations, inspect current/historical snapshots. | Typed Registry behavior largely implemented. The review keeps metadata independent of native storage layout. |
| Task ownership | Process and Thread roles, parent retention, claim incarnation, publication-ID issuance, endpoint opening. | Claims, issuance, endpoint identities, and name overloads implemented. |
| Direct Queue output | Send an immutable message with an explicit publication ID; inspect acceptance. | Implemented in step 4.6: owned/buffered send and status, atomic acceptance, exact retries/conflicts, and retained earlier-incarnation lookup. |
| Publishing Queue Sets | Publish to an ordered snapshot, all-destination/partial acceptance, status lookup, retry rejected destinations. | Public declarations exist; full Publishing behavior follows its roadmap milestone. |
| Source Queue Sets | Receive, acknowledge/release exact tokens, report gaps/timeouts, close explicitly, preserve borrowed-view lifetime. | Implemented in step 4.7: one Active Queue for fresh selection, stable borrowed messages, exact acknowledgment/release, checked finite Spin waits, and explicit close with deferred cleanup. Allowed-loss gaps remain step 4.8; multiple-Queue selection remains milestone 5. |
| Policies | Two-plane/segmented storage; strict/fair/weighted-random selection; spin/eventfd waiting; recover-from-log/allowed-loss delivery; all/partial acceptance. | Policy value types exist. Runtime support depends on the implementation milestone and platform; choosing a value does not enable an unsupported policy. |
| Message construction | Schema-driven values and codecs; owning `Message` and borrowed `MessageView` with the same `MessageHeader`; `Delivery` carries a view and its token; owning/borrowed portable records. | Scalar/basic logical values, binary/record codecs, and the consistent Message/MessageView/Delivery header model implemented. Received views and owning copies retain current exact codecs and complete headers. Complete Field families, pools, and serializers remain proposed. |
| Complete Fields and pools | Wider bit fields, temporal values, durations, sequence/GUID values, arrays, tuples, repeating groups, constraints, symbols and UTF-8 strings. | Proposed full-roadmap schema/value surface. Pool lifetime, bounds, and portable representation remain explicit. |
| Generic inspection | Decode messages without application-specific C++ types, retain exact catalogs, render structured/text output. | Existing capture inspector implements bounded text rendering for supported Binary values. Broader structured inspection and formats are proposed. |
| Runtime diagnostics | Task liveness, Queue sequence/backlog/progress, Source pin/issuance/selection state, Publishing topology, storage use/extents. | Task diagnostics implemented. A coherent public runtime/storage inspection surface is proposed, separate from definition-only Registry snapshots. |
| Capture and export | Read/write portable message records; retain the schemas and provenance needed to inspect them later. | Ordinary bounded capture streams implemented. Complete portable bundles and native-log export are proposed. |
| Native message logs | Open an explicit read-only view, select a consistent cut, query/filter/seek records and delivery metadata, detect missing retained history. | Proposed tool-facing interface; native append/recovery internals do not become public record pointers. |
| Replay | Feed recorded messages to a processing handler; choose an explicit ordering, range, filters and pacing; optionally republish into a test Session with explicit mapping. | Proposed and not yet executable. Fidelity and identity rules below are part of the review. |

The proposal does not introduce a second policy engine or a separate transport API for each storage mapping. The same Queue and Queue Set operations should work across supported policy combinations. Capability queries explain what a particular build/platform/Session can actually execute.

Legacy v0 structs and Boost/shared-memory implementation types should remain outside the intended modern entry points. They are migration compatibility, not concepts a new application should need to learn.

The review headers reuse current typed identities, policy descriptors, and result types. Their `FieldDescriptor` and generalized `SchemaCodec` are candidate replacements/extensions for the fuller milestone-9 model; they have not replaced the implemented scalar/logical codec. Scalar values keep ordinary C++ arithmetic; wide bit values expose checked bit operations. Temporal values use explicit recorded offsets, calendar durations remain distinct from elapsed time, and compound definitions have finite bounds and reject cycles. UTF-8 symbol character counting and regex syntax in these declarations are proposed details to review before their wire/serializer contracts are fixed.

Writable pool handles come from an actor-bound `Task`; Session and archive access resolves retained values without authorizing interning. Encoded payload owners retain any local pool leases through the send call. Caller-buffer encoding returns a lease with a borrowed written prefix. Archives retain the source pool namespace, and cross-Session republishing decodes owning values and re-encodes them against target pools rather than copying native pool-reference bytes.

Storage maintenance is explicit too: `Session::plan_reclamation` returns an opaque plan and summary, and `reclaim` validates that plan again against current state. Requested time/byte targets cannot override unacknowledged deliveries, pins, retained schema/pool dependencies, or publication-identity retirement fences. These are proposed milestone-8 controls, not implemented log-space recovery today.

## Debugging with message logs

The debugging path should be as intentional as the application path:

1. Obtain an immutable log view or export a consistent capture from a live Session.
2. Retain its Session/schema context and the selected commit boundary.
3. Filter records by the relevant producer, Queue, schema, sequence range, or time range.
4. Inspect their headers, values, original destinations, and retained delivery progress.
5. Replay the chosen records into the same application processing handler, or explicitly republish them into an isolated test Session.

### Read-only inspection and portable captures

`Session::attach` is a live runtime operation and can perform recovery writes. It must not be presented as a read-only file inspector. The proposed log interface distinguishes opening an immutable archive or quiescent read-only log from taking a consistent view of a live Session. A live view needs a documented captured boundary; it must not accidentally follow a moving tail or repair production state merely because a user requested offline inspection.

The existing `MessageCaptureReader`/`MessageCaptureWriter` wrap ordinary streams of canonical records. They do not own native log files, follow a live writer, or embed a schema catalog. A portable debugging bundle needs the exact catalog and source Session identity as well as record bytes, including the value context needed for future pooled Fields. A same-numbered schema in another Session is not automatically the same schema.

Log cursors identify positions in the exposed log view. They should be opaque public values with documented validity, rather than native memory addresses or unchecked file offsets. A frozen archive can support stable seek/resume; a live retained-log cursor needs explicit behavior when retention removes its position.

Publication cursors yield each original message once. Delivery projections yield accepted destinations, including later retry acceptances, in acceptance order while preserving each Queue's sequence. A separate `acceptance_attempts()` cursor exposes retained initial/retry evidence and original versus cumulative destination outcomes; failed attempts that were never recorded are not invented. It is an inspection cursor, not a source of replayed deliveries. Receipt snapshots describe progress at the captured cut, not an event trace of every borrow.

The archive catalog exposes original Session metadata/options and retained Registry revisions. `LogArchive::observe()` reports captured Queue/Source/storage progress and last recorded Task diagnostics. Its stored attached flag is not a claim that an old PID is alive, and these diagnostics are not a complete history of every Task attachment. Live `Session::observe()` remains a distinct, recovered runtime observation.

### Replay for a processing handler

Handler replay supplies message data and recorded context directly to application processing code. It does not claim the original producer/consumer Tasks, fabricate a live `DeliveryToken`, or acknowledge the original receipts. Its cursor and progress belong to the replay operation.

One useful application shape is to keep message processing separate from receiving and from external effects. The live loop receives and acknowledges around that handler. A debugging replay feeds recorded messages to the same handler with a test clock, recorded context, and application-selected effect adapters. The library cannot infer an application's clocks, random choices, network calls, or database decisions from message bytes alone.

Ordering must be explicit:

| Replay order | What the retained log can establish |
| --- | --- |
| Publication commit order | The stored order of committed publications. This is not the order of producer-issued publication IDs or a reconstruction of application processing. |
| One Queue's sequence order | Its retained accepted deliveries in Queue order, including messages whose ring entries were overwritten. |
| Recorded consumer input order | An optional execution trace records the application-selected message-input sequence for one processing stream. Ordinary message logs cannot reconstruct it. |

UTC times are useful inspection and filtering data. They can be equal or move backwards and are not a universal causal order. Fair/random selection settings help reproduce policy choices under specified inputs, but the current receipts retain only their most recent issuance and terminal metadata, not every scheduling/release attempt. A seeded selector alone does not reconstruct the timing and availability seen by the original application.

The agreed direction is **message replay by default, with optional execution tracing when needed**. The proposed `InputTraceRecorder` records each selected input immediately before its handler is invoked, including deliberate repeat processing of a pin. It retains message/schema/pool context in a separate self-contained trace; `ExecutionTrace::inputs()` supplies that recorded order to the same replay engine. One recorder represents one serialized processing stream, not a global order across independent consumers. Ordinary logging has no trace cost by default.

Opening a trace can expose its validated complete prefix after process termination; status explicitly reports an incomplete tail or missing finalization. Corrupt completed entries are errors, and opening never repairs the original file. This is a proposed trace-file contract, not a claim that ordinary captures already provide it.

The recorder retains its exact Task claim, validates the current pin, and copies canonical data/context before a borrow can end. A successful call writes the complete framed input and dependencies beyond user-space buffering before processing proceeds; an I/O failure makes further recording unusable. This is process-termination scope, without a power-loss promise. Trace replay preserves the original handler context and historical issuance, and anchors virtual time at recorded trace-start UTC plus monotonic elapsed time. The example returns trace status alongside its replay object so the caller can decide whether to use an incomplete prefix.

This trace is evidence of inputs at the chosen instrumentation boundary. A crash after recording but before processing can leave an unprocessed final input, and trace failure must not be ignored while continuing to claim faithful tracing. The caller handles errors and releases/closes its existing delivery normally; the recorder never acknowledges it. Restoring a replay checkpoint restores input position and virtual time only. External effects, random values, timeouts, gaps, and other nondeterministic application inputs require their own recorded/test dependencies if the application needs them reproduced; message-input order alone is not deterministic whole-application execution.

### Republishing into a test Session

Republishing is a separate operation from handler replay. It needs an explicit destination Session, producer/Queue or Publishing endpoint mapping, and compatible exact schema mapping. It uses new publication identities issued by that destination Session while retaining the original source identity as provenance.

Copying an old `PublicationId` or `DeliveryToken` into a new Session would confuse historical identity with a new delivery obligation. The interface must also make repeated replay runs and their interruption behavior visible; it must not silently promise exactly-once external application effects.

Before reinjection, the example's application callback persists the source bookmark, new target publication ID, exact target schema, and owning logical input. After interruption it resolves that target ID before retrying or advancing its replay checkpoint. Raw pooled Binary bytes alone are not a durable substitute for that logical input/context.

## Choices to review before adopting this proposal

The message naming/header model and replay direction are agreed. The owning-outcome default and detailed signatures remain part of this review:

1. **`Message`, `MessageView`, and `Delivery` with one consistent header.** This naming and ownership model is agreed: `Message` owns, `MessageView` borrows payload bytes, and `Delivery` adds the acknowledgment token. Owning copies preserve the complete `MessageHeader` and decoding dependencies. Detailed signatures remain part of this review.
2. **Owning publication outcomes for the ordinary overload, reusable buffers for controlled allocation.** Recommended for the small bounded topology. The owning result is easier to store and inspect; the buffer overload remains available where allocation matters. Its exact error/uncertainty representation needs to remain consistent with the existing publication contract.
3. **Message replay by default, optional input tracing when needed.** This direction is agreed. The proposed recorder/replay signatures still need review. Whole-application determinism and a global cross-process execution trace are not implied.

Explicit publication identities, explicit acknowledgment, exact schema versions, and separation of replay from original receipt mutation remain fundamental to the proposed design. Any changes to already published signatures or behavior require an intentional compatibility review before moving declarations out of `angaria::proposed` into the production API.
