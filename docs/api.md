# Public C++ API contract

The [complete public interface proposal](public-interface.md) reviews the full 1.0 roadmap, including application ergonomics, advanced Fields, diagnostics, offline logs, and replay. Its isolated `angaria::proposed` declarations have no runtime definitions; this document continues to describe the current API and its existing planned contracts.

Step 1.3 defines Angaria's process-local C++ types and the operations that subsequent milestones implement. The [trading API example](../examples/trading_api.cpp) expresses the configuration topology through typed Registry and endpoint calls. CMake compiles it as the `angaria_api_contract` object target.

```sh
cmake --build build --target angaria_api_contract
```

The target checks declarations, including future delivery operations. The [programmer primer](primer.md) contains runnable examples of Session and Registry use, scalar/logical payload encoding, canonical message records, capture, and generic inspection. Native mapped Queue payload operations follow in milestone 4, and Queue Set behavior in milestones 5 and 6. The prototype's executable and tests exercise its separate earlier implementation.

## Headers

| Header | Contract |
| --- | --- |
| `angaria/identities.h` | Distinct entity IDs, schema/session identities, Task claims, publication identities, and delivery tokens. |
| `angaria/descriptors.h` | Owning descriptors, session options and capacities, policy choices, and metadata views. |
| `angaria/results.h` | Typed ordinary failures and bounded publication outcome reports. |
| `angaria/message.h` | Immutable borrowed payload and delivery views, gap events, and timeouts. |
| `angaria/message_record.h` | Portable message headers/records, bounded framing, owning/borrowed decoding, and exact binary schema context. |
| `angaria/capture.h` | Bounded, move-only readers/writers of consecutive canonical records through borrowed streams. |
| `angaria/inspection.h` | Owning Field/schema catalogs and bounded generic text rendering with exact Session/schema context. |
| `angaria/payload.h` | Owning schema-driven binary payload codecs, exact scalar/logical values, and bounded construction. |
| `angaria/registry.h` | Registration, discovery, metadata inspection, activation, and draft ownership changes. |
| `angaria/runtime.h` | Session, Task, QueueWriter, SourceQueueSet, and PublishingQueueSet handles. |

Descriptors own strings and vectors for registration and inspection. The [mapped layout contract](mapped-format.md) defines their persisted representations, capacity ranges, and compatibility rules. The existing prototype registration overloads remain available during the field API migration.

## Identities and tokens

Task, Queue, Source Queue Set, Publishing Queue Set, and Message type IDs have distinct C++ types with 16-bit values. Source and Publishing IDs share the Registry's allocation namespace. Zero is invalid. A Field ID has 32 bits and registration validates the `0xEE` high byte.

A schema identity includes message type and version. A session has an opaque 128-bit identity. A Task claim includes its session, Task ID, and incarnation. A publication identity adds a producer-local sequence to that claim.

A delivery token contains the consumer claim, Source Queue Set, queue-local sequence, queue ID, and issuance generation. Issuance advances when a delivery is newly issued, including reissue after explicit release or Source handle reopening. It remains distinct within a live claim. Re-reading the current in-flight delivery returns its existing token. Acknowledgment with an older issuance fails even if the queue and sequence match.

## Session creation and Registry attribution

Programmatic creation takes session options and a creator process-Task descriptor. File-based creation takes a TOML path and the name of a process Task declared by that file. Creation returns `CreatedSession`, containing the Session and its attached creator Task.

The creator consumes one reserved Task entry. File-based creation uses its existing declaration, and identical programmatic re-registration returns that same identity. Creation rejects a thread creator, an absent creator declaration, or insufficient Task capacity. The creator's native process/user information and logical Task identity supply session creation metadata and initial registration attribution.

`Task::registry()` supplies an actor-bound Registry. Typed mutations record the acting Task and validate its live claim. Configuration/bootstrap registrations are attributed to the returned creator. `Session::registry()` supplies a view for lookup and inspection, and its typed mutations return `ReadOnlyRegistry`.

`Session::options()` exposes the effective settings and limits. Session metadata includes creator, creation time, framework/application versions, and creator user. Registry metadata exposes entity creator, registration time, and revision. Task runtime inspection supplies PID/thread diagnostics, incarnation, restart count, last attachment, and attachment state.

## File-based Sessions

Step 2.10 implements `Session::create_from_file(configuration, creator)` and `Session::attach_from_file(configuration)` through pinned cpptoml. The [configuration contract](configuration.md) specifies accepted keys, exact TOML types, reference resolution, defaults, capacity checks, and the supported native profile.

Creation requires a declared Process creator, resolves forward references, validates the full configuration, and attributes its initial registrations to that creator. It uses the same descriptor, policy, registration, and activation rules as programmatic setup. Complete nonempty topology becomes Active in Source/Queue/Publishing order; empty sets remain Draft. Setup occurs in an exclusively created staging directory, and publication of the completed Session refuses to replace an existing target. Ordinary failure removes only that invocation's unpublished staging resources.

A relative `session.directory` resolves from the TOML file's parent. File-based attachment checks the requested Session name, application version, log byte limit, storage settings, capacities, and policy defaults against persisted effective values, returning `ConfigurationMismatch` for disagreements. The actual canonical attachment directory can differ from the recorded creation location. Supplied declarations and references are validated, with already registered names available for reference resolution; the declaration list does not undo supported runtime topology changes. Attachment applies no configuration updates and claims no Task. Its normal validation and recovery can still write to the Session file.

Malformed syntax and semantic configuration errors return `InvalidConfiguration` with filename/key-path diagnostics. Known unavailable native policies return `UnsupportedPolicy`. Capacity or complete log-allocation failures retain their specific error categories. The [seventh primer example](../examples/primer/07_configuration.cpp) demonstrates creation, inspection, closing the creator, and compatible reattachment.

## Capacity defaults and named setters

`CapacityLimits{}` starts with the bounded [C++ default profile](configuration.md#capacity-limits). `SessionOptions::_limits` uses that profile. Override individual capacities through named setters:

```cpp
auto limits = CapacityLimits{}.tasks(16).queues(128).source_sets(16);
```

Unspecified capacities retain their defaults. Setters modify an existing object and support chaining; chaining on a temporary returns a value that can be stored safely. Positional capacity lists are not supported. Setter names follow the project's `snake_case` convention.

`CapacityLimits::empty()` explicitly sets every capacity to zero. Individual facilities can also be disabled with a zero-valued setter. All settings still pass through normal validation; an empty profile cannot create a session until it has room for its creator Task and required resources.

These defaults apply to new programmatic sessions. TOML capacity keys remain required. Existing-session attachment uses the recorded effective limits and validates any supplied configuration against them.

## Descriptors and policy inheritance

Optional policy overrides in entity descriptors inherit the recorded session defaults. Session defaults initially select recovery from log, all-destination acceptance, strict selection, and spinning. Queue ring capacity inherits the session default of 1024 for two-plane storage.

Registration resolves omitted policy choices and generated random seeds before activation and records the effective settings. Descriptor inspection returns those resolved values. Session storage and capacities bind at creation. Active queue and Queue Set policy choices retain the configuration contract's fixed semantics.

The optional application version is supplied through `SessionOptions` or the TOML `session.application_version` key and persisted with the Session definition.

## Registration and activation

The example registers Tasks, both Queue Set types, queues, Fields, and an immutable Quote schema. Typed lookups preserve each entity kind, and registration returns either its typed identity or an `Error`.

Step 2.6 implements `Registry::register_task(TaskDescriptor)` on a live actor-bound Registry. Task names are case-sensitive, nonempty UTF-8 without NUL characters; documentation is length-delimited UTF-8 and may be empty or contain NUL. Name, kind, parent, and documentation together define exact re-registration: an identical descriptor returns its existing Task ID without changing metadata, revision, generation, log bytes, or claim files. Changing any descriptor field under an existing name returns `Conflict`. There is no implicit Task edit or Task deletion API.

A Process Task omits its parent. A Thread Task names a registered Process Task, which can be offline and need not be the registrar. Registration describes a logical participant; it does not attach that Task or install native process/thread IDs. Attachment separately enforces the live local parent and thread-affinity rules. A new registration consumes one reserved entry, an unused Task ID, and log space for its immutable definition and transaction manifest. Its creator/time/revision record the acting claim's logical Task, current UTC time, and new Registry revision. See the [native registration contract](native-storage.md#public-task-registration) for coordination resources and interrupted outcomes.

Step 2.7 adds `register_field` and `register_message`, plus Field/schema lookup, description, and registration metadata. A Field's full ID, name, base type, declared byte size, documentation, and logical metadata are immutable; exact re-registration returns its existing ID, while a conflicting ID or name returns `Conflict`. Valid Field IDs range from `0xEE000000` through `0xEEFFFFFF`, independently of the number of reserved Field entries. The current implementation accepts char/byte, signed int8 and signed/unsigned 16-, 32-, and 64-bit integers, 32-/64-bit floating point, and the basic logical representations below. Other known representations return `UnsupportedPolicy`; malformed IDs, unknown base types, and incorrect sizes return `InvalidArgument`.

Step 3.3 implements [logical Field metadata](logical-fields.md) in `FieldDescriptor::_logical`. Its `FieldLogicalKind` is `None`, `Boolean`, `Bits`, `Enumeration`, or `FixedAscii`. Only an Enumeration has entries: each `EnumeratorDescriptor` owns a name and `EnumerationNumber` (`int64_t` for signed storage, `uint64_t` for unsigned storage, checked against the base width). Entry order canonicalizes numerically; reordered exact retries preserve attribution and revision. The complete mapping is immutable. int8 uses appended base code 26. New Sessions use format 2.1; existing 2.0 Sessions reject new logical/int8 registrations with `UnsupportedPolicy` and retain scalar behavior.

A message type retains one name across a [schema family](glossary.md#ownership-and-recovery). Each nonzero 32-bit version identifies a separate immutable schema. Versions can be sparse or registered out of numeric order. Changing the type/name pairing or an existing schema's ordered Field list/documentation returns `SchemaConflict`; exact retry preserves its original attribution and revision. Every referenced Field must be registered. Order and repeated Field IDs are preserved, and an empty Field list is valid for a control-message definition. Schema capacity counts versions, while Field-list length is bounded by available log bytes rather than a hidden 16-bit limit. `find_message(name, version)` and `describe_message(SchemaId)` return the exact retained version, not a latest-version substitution. Registration establishes definitions, not payload encoding or publication behavior; see the [native schema contract](native-storage.md#fields-and-message-schemas).

Step 2.8 adds draft Queue/Source/Publishing registration and typed lookup, description, metadata, state, and Source membership inspection. Queue Sets share IDs and names across their two kinds, while per-kind capacities remain separate. Registration resolves effective policies, returns exact retries without growth, and rejects changed descriptors with Conflict. Creating a Queue in a Source atomically adds its inverse membership and advances that Source's definition revision. Publishing destinations are unique, ordered Queues belonging to its nonzero producer owner. Incomplete drafts may omit ownership or a Queue's Source; every nonzero reference must resolve. See the [native draft-topology contract](native-storage.md#draft-queues-and-queue-sets) for supported policies and persisted validation.

Step 2.9 implements activation: Source activation checks complete ownership and nonempty membership, including fully configured draft queues. Queue activation requires its Source Queue Set to be active and atomically allocates its zeroed ring. Publishing activation requires active destination queues owned by the publishing Task. Any live actor can activate configured entities for offline owners. This ordering supports an initially offline consumer.

Step 2.9 also implements draft ownership adoption, source assignment, and Publishing destination replacement as explicit Registry operations. A zero Source ID in `assign_source` explicitly unassigns a draft Queue; moving it updates both inverse Source lists atomically. Source membership can gain fully configured draft Queues after Source activation, but an Active Source cannot become empty. Active Queue ownership, Source, and priority remain fixed. Active Publishing destination replacement requires its owning actor and nonempty Active destinations, commits a new topology revision, and uses the shared mutation gate. Existing publication snapshots preserve their original destinations when publication is implemented. See the [native update contract](native-storage.md#topology-changes-and-activation).

## Current and historical Registry snapshots

Step 2.11 adds `Registry::snapshot()` for listing all registered entity kinds in one consistent view. The result is an owning `RegistrySnapshot`, containing Tasks, Queues, Sources, Publishing sets, Fields, and exact message-schema versions. Each entry includes its descriptor and registration metadata; topology entries also include their state, Source membership, or Publishing topology revision as appropriate. Values remain usable after the originating handles close and retain no Task claim.

The operation holds one mutation gate while recovering and validating the Session and copying the catalog. This avoids combining the Task list from one revision with membership from another during concurrent updates. Entries sort by typed ID, with message schemas sorted by type and version; member and destination order retain their recorded meaning.

Pass a committed Registry revision, such as `registry.snapshot(12)`, to inspect the definitions at that revision. `_revision` identifies the requested cut, and `_currentRevision` records the latest committed revision observed by the same operation. An omitted argument selects that current revision. Revisions start at one; zero returns `InvalidArgument`, and a future revision returns `NotFound`, both at `registry.revision`. A historical cut contains the final state of each entity within an atomic batch and omits later registrations. It preserves older Source assignments, activation states, Publishing destinations, and schema versions. All Registry history is retained in this implementation.

Task attachments, incarnations, and publication-ID issuance use separate fixed state. A Registry revision does not reconstruct their earlier values. Read `task_status()` separately for current diagnostics. A snapshot from a Session Registry can inspect without a Task claim, but cannot perform configuration mutations. Normal validation/recovery can still write to the backing file; this is not an offline foreign-format or physically read-only file reader. General log tooling follows milestone 10.

The [Registry walkthrough](registry-walkthrough.md) and [primer](primer.md) demonstrate inspection, concurrent registration, retained history, and owner restart through independent executables.

## Endpoint ownership and lifetime

Session and Task handles are movable. Writer and Queue Set handles are movable and retain their backing session and Task claim. The claim remains held while its Task, actor-bound Registry views, or endpoints retain it. When receive is implemented, Source closure will release an unacknowledged delivery, and process termination will follow the configured recovery policy.

Steps 2.5–2.11 implement programmatic and file-based Session creation/attachment, Session identity/options/metadata, Task registration and attachment/claim/Registry access, publication-ID issuance, Task runtime status, registration/inspection of every Registry entity kind, current/historical snapshots, topology changes, activation, and endpoint identity/ownership handles. The creator consumes Task ID 1 and its initial Registry revision is 1. Delivery methods follow their implementation milestones. See the [native lifecycle contract](native-storage.md#public-session-and-task-lifetimes).

An operation returning `Result<T>` reports an empty/moved-from handle as InvalidState and inherited-process or wrong-thread Task use as WrongOwner. The plain-value `Session::identity()`, `Session::registry()`, `Task::claim()`, and `Task::registry()` methods throw `std::logic_error` for invalid ownership or an empty/moved-from handle. Applications must exec before using inherited native library state after fork, as specified by the native contract.

A Task may open writers for its queues, Source Queue Sets it owns, and Publishing Queue Sets it owns. A second open Source handle for the same set and live claim returns `OwnershipBusy`. The single in-flight slot applies to the set. The issuance generation survives closing and reopening its Source handle within the claim.

Step 2.9 implements those three opening operations and each handle's `id()`. Opening requires an Active entity and the matching logical Task owner; other owners get WrongOwner and draft entities get NotActive. Handles retain their Task claim across moves and closure of the original Task variable. Source close/reopen currently changes only claim-local opening state and writes no persisted data; receive/acknowledgment/release and delivery pin cleanup arrive with delivery. A moved-from, inherited-process, or wrong-thread handle's plain `id()` throws `std::logic_error`, matching Task's plain-value ownership checks.

The application serializes sends touching a common queue and operations on a common Source Queue Set. A thread Task attaches on its current thread while its parent process claim is held by that process. The thread Task and its endpoints retain the parent claim until they close. A process Task supports the application's coordinated sending threads.

The [concurrency contract](concurrency.md) uses per-Task lifetime claims and a crash-released session mutation gate. Initial send, receive, Registry, and delivery-progress operations are blocking. Claim replacement, issuance, acknowledgment, and release use reserved state and require no new log allocation. Immutable payload reads after receive retain their mapping/pin without holding the operation gate.

## Binary payload codecs

Step 3.1 implements `BinaryPayloadCodec`, compiled from an exact `SchemaId` and an explicit payload byte cap. `from_registry` takes one consistent snapshot; `from_snapshot` also accepts a previously captured catalog. The codec owns resolved Field descriptors and their order, so it retains no Session, Task claim, or Registry view. `schema()` and `payload_size()` identify its layout; `fields()` borrows its ordered descriptors for the codec's lifetime.

`PayloadValue` distinguishes char, byte, signed int8 and signed/unsigned 16-/32-/64-bit integers, float/double, `bool`, `BitFieldValue`, `EnumValue`, and `FixedAsciiValue`. `ScalarValue` remains an alias for `PayloadValue`; earlier scalar examples still compile. Exhaustive variant visitors need cases for the new alternatives. Enum values wrap an `IntegerValue` of the exact underlying width/sign; fixed ASCII owns its text bytes. `encode` accepts exactly one matching alternative for each Field occurrence and returns owning bytes. `decode` requires exactly the compiled payload length and returns owning values. Fields are packed in schema order with explicit little-endian integer and IEEE floating-point representations. Repeated Fields and empty schemas remain meaningful, and newer versions do not change older codecs.

`encode({values...})` provides initializer-list construction under the codec's byte cap. `encode_into(values, output)` requires a byte span of exactly `payload_size()` bytes and returns `OperationResult`; all validation/allocation completes before copying, so errors leave caller output unchanged. These APIs construct application-memory payloads. Step 3.4 wraps them in portable message records below; Queue transport follows later milestones.

Malformed or ambiguous selected definitions, invalid widths, mismatched values, and incorrect byte lengths return `InvalidArgument`. Missing exact schemas/referenced Fields return `NotFound`; recognized unavailable Field representations return `UnsupportedPolicy`; byte/allocation limits return `CapacityExceeded`. A zero cap is valid for an empty schema. Full layout, lifetime, and error rules are in [the payload contract](message-payload.md), demonstrated by the [ninth primer program](../examples/primer/09_payload.cpp).

## Canonical message records

Step 3.4 implements `MessageHeader`, `MessageRecord`, `MessageRecordView`, and `MessageRecordCodec`. The header owns an exact schema, payload format/byte count, full producer `PublicationId`, initial publication UTC time, and user flags. Encoding version 1 uses an explicit 80-byte little-endian header followed immediately by the payload. This portable record has no native offsets or destination receipts and leaves native Session formats 2.0/2.1 unchanged.

Every header/record encode or decode requires `maxRecordBytes`, including header and payload. `encode_header` and `decode_header` operate on exactly 80 header bytes and validate the announced total before payload allocation. `encode` returns owning record bytes; `decode_view` borrows only the input payload; `decode` owns an independent header/vector. Whole-record decoding requires exactly one complete record. `encode_into` accepts the exact output span and preserves it on errors, including overlapping payload/output spans.

All five stable payload-format codes are preserved as opaque bytes; JSON/XML/CSV/text serializers are not implied. `decode_binary(record, codec, schemaSession)` additionally requires Binary, the exact codec schema, and a matching independently retained source Session identity. A same-numbered schema from another Session or a different same-sized version returns `SchemaConflict`. The supplied definitions remain caller-selected, with no authentication of their origin or proof of publication admission.

`MessageRecord::view()` is allowed on lvalues and borrows their payload storage. `MessageRecordView::message()` adapts to the existing borrowed `MessageView` while retaining the same backing-storage lifetime. Initial publication time is distinct from `DeliveryView`'s per-destination enqueue/dequeue metadata. Complete bytes, errors, lifetime, and receipt boundaries are specified in [the message-record contract](message-record.md) and demonstrated by [primer lesson 11](primer.md#11-wrap-a-payload-in-a-message-record).

These codecs construct application-memory bytes. `MessageView` remains the borrowed view that later publication steps will consume; codec encoding does not issue publication identities or perform delivery.

## Capture and generic inspection

Step 3.5 implements `MessageCaptureWriter`, `MessageCaptureReader`, and `MessageInspector`. A [capture](capture-inspection.md) contains consecutive canonical records, with no additional file header or embedded schema catalog. Stream wrappers borrow a caller-owned stream and require record/aggregate byte caps. Writers validate before output; a partial I/O failure is terminal. Readers return an owning record, clean EOF, or a terminal contextual error. Counters count only complete successful records from the stream's initial position.

`MessageInspector::from_session(session, limits)` obtains one consistent Registry snapshot. `from_snapshot(schemaSession, snapshot, limits)` accepts an independently retained Session identity and definitions. Inspectors own only Field/schema definitions and retain no Session or Task claim. Copies remain independent; moved-from inspectors return `InvalidState`. Later definitions require a new inspector. Native snapshot acquisition uses the ordinary Session API before inspector catalog preflight.

`inspect(recordBytes)` or `inspect(recordView)` validates framing, source Session, and exact schema version, then returns owning ASCII text. It decodes existing binary scalar/logical Fields without application-specific types. Names, labels, and values are escaped; Field order, enum numbers/labels, bit widths, ASCII padding, and IEEE special values remain visible. Missing schemas return `NotFound`, a different Session returns `SchemaConflict`, and known nonbinary formats return `UnsupportedPolicy`.

`InspectionLimits` bounds whole-record bytes, retained/expanded catalog definitions, and per-record text. Catalog preflight precedes inspector copies; repeated Field definitions are charged before compiling a selected layout. These size budgets do not measure exact process memory. The `angaria_inspect` CLI adds aggregate capture/output limits and closes native handles before capture I/O. Native catalog attachment can recover/write the Session and requires a compatible profile. See the [complete contract](capture-inspection.md) and [primer lesson 12](primer.md#12-capture-and-inspect-multiple-message-types).

Step 3.6's [process qualification](process-exchange.md) uses these same APIs in independent executables. A retained inspector keeps its original schema catalog while other processes add a version and replace a producer claim. Cold inspectors read every retained version after applications exit. This step adds an example and qualification coverage without changing public API or persisted formats.

## Messages and receive events

The [native transport contract](native-transport.md) specifies future admission, compatibility, receipt, and lifetime behavior. Step 4.3 implements [core Session/history validation](native-validation.md) after the private local-byte codecs; public native delivery remains unimplemented. The planned additions `DeliveryView::_publishedAt` and `SourceQueueSet::close()` arrive with step 4.7; current headers are unchanged. Fresh selection initially supports at most one Active Queue per Source while existing in-flight acknowledgment/release remains available during membership growth. See the [milestone implementation sequence](milestones/milestone-4.md).

`MessageView` carries an exact schema identity, payload format, immutable byte span, and user flags. Publication borrows input bytes for the duration of the call. A committed message retains its encoded data independently of the caller's input buffer.

Receive returns a `DeliveryView`, `DeliveryGap`, `ReceiveTimeout`, or `Error`. The delivery identifies the original publication, current token, and enqueue/dequeue times expressed as UTC chrono time points. The mapped timestamp encoding follows step 1.4.

A received payload remains pinned while its delivery is in flight. Its borrowed views expire on acknowledgment, release, Source handle closure, or claim termination. Moving a handle transfers ownership. Replacing a handle releases its previous endpoint state. Applications finish reading a payload before the operation that ends its lifetime.

Acknowledgment and release require the exact current token. Release leaves a pending delivery for reselection under the queue's delivery policy. Gap ranges use queue-local sequences. Gap events and timeouts occupy no delivery slot. Receive waits use a monotonic timeout.

## Publication outcomes and output buffers

The caller supplies storage for destination outcomes to send, publish, retry, and recovery lookup. Retry and lookup capacity cover the complete original destination snapshot, including destinations accepted before the retry. `PublicationReport` borrows the filled portion of that buffer. The report's destination view expires when the caller changes or destroys the buffer.

Before committing a new publication, an undersized output buffer returns `OutputBufferTooSmall` and reports the required capacity. A status lookup buffer error describes the lookup and does not establish that the original publication was rejected. Callers check `_error` before acting on `_status`.

| Publication status | Meaning |
| --- | --- |
| `Accepted` | Every destination in this publication's snapshot has accepted. |
| `Partial` | A committed publication has accepted and rejected destinations. Results identify each destination and reason. |
| `Rejected` | The current attempt is known to have established no delivery obligations. |
| `OutcomeUnknown` | Available information does not establish the final acceptance outcome. Recovery lookup is required. |

A Task obtains an identity before attempting publication so it can query that identity after an interrupted call. Identity issuance and payload publication are separate operations. Step 1.5 defines their allocation and commit protocol.

Identity issuance persists through the reserved fixed-state journal. Fresh sends require a current-claim issued ID. Repeating an identical already committed current-claim send returns its original cumulative outcome; conflicting input returns Conflict without altering that publication. The recorded endpoint and original snapshot remain authoritative after topology changes. Recovery settles the journal before lookup; provable absence without reclaimed history reports Rejected with an empty outcome view. Missing retired history returns `HistoryUnavailable` and cannot establish rejection.

Recovery lookup and retry permit an earlier incarnation's publication identity for the same logical producer Task and session. Ownership checks prevent using another producer's publication identity.

`PublishingQueueSet::retry()` resumes a recorded partially accepted publication. It reads the retained original schema, payload format, flags, payload, and destination snapshot using the publication identity. Requested destinations must be unique members of the original snapshot. Already accepted destinations keep their original outcome and receive no additional publication. Repeating a subset after the publication becomes fully accepted returns that recorded outcome without adding obligations.

If required history has been reclaimed under the retention rules, retry or lookup returns `HistoryUnavailable` and establishes no additional delivery. Required unacknowledged delivery history retains its recovery guarantee.

A retry report describes cumulative acceptance for the original snapshot. If a retry accepts no additional destination, the recorded publication stays partial. When every original destination has accepted, its status becomes accepted.

A known rejected publication has no persisted acceptance snapshot. A fresh publish operation can retry its input under the current topology. The compile-checked example checks both error and partial status before using the recovery retry operation.

## Validation

Identity tests check type separation, widths, schema versions, producer incarnation, and every delivery-token identity component. API value tests check inheritance defaults, per-destination reasons, and receive-event distinctions. The complete trading example compiles against the declared methods without runtime substitutes.

[Configuration and lifecycle contract](configuration.md)
