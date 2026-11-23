# Public C++ API contract

Step 1.3 defines Angaria's process-local C++ types and the operations that subsequent milestones implement. The [trading API example](../examples/trading_api.cpp) expresses the configuration topology through typed Registry and endpoint calls. CMake compiles it as the `angaria_api_contract` object target.

```sh
cmake --build build --target angaria_api_contract
```

The target checks the future runtime operation declarations. Registry and attachment implementation follows in milestone 2, general message implementation in milestone 3, mapped queues in milestone 4, and Queue Set behavior in milestones 5 and 6. The prototype's executable and tests exercise its existing implementation.

## Headers

| Header | Contract |
| --- | --- |
| `angaria/identities.h` | Distinct entity IDs, schema/session identities, Task claims, publication identities, and delivery tokens. |
| `angaria/descriptors.h` | Owning descriptors, session options and capacities, policy choices, and metadata views. |
| `angaria/results.h` | Typed ordinary failures and bounded publication outcome reports. |
| `angaria/message.h` | Immutable borrowed payload and delivery views, gap events, and timeouts. |
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

The optional application version is a process-local string in `SessionOptions` and the TOML `session.application_version` key. Its persisted representation follows step 1.4.

## Registration and activation

The example registers Tasks, both Queue Set types, queues, Fields, and an immutable Quote schema. Typed lookups preserve each entity kind, and registration returns either its typed identity or an `Error`.

Step 2.6 implements `Registry::register_task(TaskDescriptor)` on a live actor-bound Registry. Task names are case-sensitive, nonempty UTF-8 without NUL characters; documentation is length-delimited UTF-8 and may be empty or contain NUL. Name, kind, parent, and documentation together define exact re-registration: an identical descriptor returns its existing Task ID without changing metadata, revision, generation, log bytes, or claim files. Changing any descriptor field under an existing name returns `Conflict`. There is no implicit Task edit or Task deletion API.

A Process Task omits its parent. A Thread Task names a registered Process Task, which can be offline and need not be the registrar. Registration describes a logical participant; it does not attach that Task or install native process/thread IDs. Attachment separately enforces the live local parent and thread-affinity rules. A new registration consumes one reserved entry, an unused Task ID, and log space for its immutable definition and transaction manifest. Its creator/time/revision record the acting claim's logical Task, current UTC time, and new Registry revision. See the [native registration contract](native-storage.md#public-task-registration) for coordination resources and interrupted outcomes.

Step 2.7 adds `register_field` and `register_message`, plus Field/schema lookup, description, and registration metadata. A Field's full ID, name, base type, declared byte size, and documentation are immutable; exact re-registration returns its existing ID, while a conflicting ID or name returns `Conflict`. Valid Field IDs range from `0xEE000000` through `0xEEFFFFFF`, independently of the number of reserved Field entries. The current native implementation accepts char/byte, signed/unsigned 16-, 32-, and 64-bit integers, and 32-/64-bit floating-point Fields with their fixed encoded widths. Other known representations return `UnsupportedPolicy`; malformed IDs, unknown base types, and incorrect scalar sizes return `InvalidArgument`.

A message type retains one name across a [schema family](glossary.md#ownership-and-recovery). Each nonzero 32-bit version identifies a separate immutable schema. Versions can be sparse or registered out of numeric order. Changing the type/name pairing or an existing schema's ordered Field list/documentation returns `SchemaConflict`; exact retry preserves its original attribution and revision. Every referenced Field must be registered. Order and repeated Field IDs are preserved, and an empty Field list is valid for a control-message definition. Schema capacity counts versions, while Field-list length is bounded by available log bytes rather than a hidden 16-bit limit. `find_message(name, version)` and `describe_message(SchemaId)` return the exact retained version, not a latest-version substitution. Registration establishes definitions, not payload encoding or publication behavior; see the [native schema contract](native-storage.md#fields-and-message-schemas).

Step 2.8 adds draft Queue/Source/Publishing registration and typed lookup, description, metadata, state, and Source membership inspection. Queue Sets share IDs and names across their two kinds, while per-kind capacities remain separate. Registration resolves effective policies, returns exact retries without growth, and rejects changed descriptors with Conflict. Creating a Queue in a Source atomically adds its inverse membership and advances that Source's definition revision. Publishing destinations are unique, ordered Queues belonging to its nonzero producer owner. Incomplete drafts may omit ownership or a Queue's Source; every nonzero reference must resolve. See the [native draft-topology contract](native-storage.md#draft-queues-and-queue-sets) for supported policies and persisted validation.

Step 2.9 will implement activation: Source activation checks complete ownership and nonempty membership, including fully configured draft queues. Queue activation requires its Source Queue Set to be active. Publishing activation requires active destination queues owned by the publishing Task. This ordering supports an initially offline consumer.

Step 2.9 also implements draft ownership adoption, source assignment, and Publishing destination replacement as explicit Registry operations. Destination replacement is serialized against publication and commits a new topology revision. Existing publication snapshots preserve their original destinations.

## Endpoint ownership and lifetime

Session and Task handles are movable. Writer and Queue Set handles are movable and retain their backing session and Task claim. The claim remains held while its Task, actor-bound Registry views, or endpoints retain it. Source handle closure releases an unacknowledged delivery, and process termination follows the configured recovery policy.

Steps 2.5–2.8 implement programmatic Session creation/attachment, Session identity/options/metadata, Task registration and attachment/claim/Registry access, publication-ID issuance, Task runtime status, and registration/inspection of every Registry entity kind. Queues and Queue Sets remain draft until activation is implemented. The creator consumes Task ID 1 and its initial Registry revision is 1. Endpoints follow their implementation steps; TOML factories follow step 2.10. See the [native lifecycle contract](native-storage.md#public-session-and-task-lifetimes).

An operation returning `Result<T>` reports an empty/moved-from handle as InvalidState and inherited-process or wrong-thread Task use as WrongOwner. The plain-value `Session::identity()`, `Session::registry()`, `Task::claim()`, and `Task::registry()` methods throw `std::logic_error` for invalid ownership or an empty/moved-from handle. Applications must exec before using inherited native library state after fork, as specified by the native contract.

A Task may open writers for its queues, Source Queue Sets it owns, and Publishing Queue Sets it owns. A second open Source handle for the same set and live claim returns `OwnershipBusy`. The single in-flight slot applies to the set. The issuance generation survives closing and reopening its Source handle within the claim.

The application serializes sends touching a common queue and operations on a common Source Queue Set. A thread Task attaches on its current thread while its parent process claim is held by that process. The thread Task and its endpoints retain the parent claim until they close. A process Task supports the application's coordinated sending threads.

The [concurrency contract](concurrency.md) uses per-Task lifetime claims and a crash-released session mutation gate. Initial send, receive, Registry, and delivery-progress operations are blocking. Claim replacement, issuance, acknowledgment, and release use reserved state and require no new log allocation. Immutable payload reads after receive retain their mapping/pin without holding the operation gate.

## Messages and receive events

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
