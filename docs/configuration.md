# Angaria configuration and lifecycle contract

This document defines the current configuration and lifecycle contract for framework **0.4.39**, milestone step **4.37**. TOML loading and the public file-based Session factories use the same descriptor/policy validation as programmatic setup. Public messaging uses mapped format **3.0**, synchronization revision **2**; older Sessions are rejected and development Sessions must be recreated.

Configuration files use TOML through cpptoml revision `fededad7169e538ca47e11a9ee9251bc361a9a65`. The syntax contract targets TOML v0.5.0, which the upstream README documents. The [trading configuration](../examples/trading.toml) illustrates the three-process topology from the [milestone 1 plan](milestones/milestone-1.md). [cpptoml source](https://github.com/skystrife/cpptoml/tree/fededad7169e538ca47e11a9ee9251bc361a9a65)

The current native profile supports two-plane storage, scalar/basic logical Binary Fields, strict single-input Sources, spin waiting and recorded delivery with required or no processing acknowledgments. Acknowledgment remains required by default. Several outgoing Queues of one Task share its producer log, and exact schemas are selected by MessageHeader. Multiple-input selection, Publishing delivery, stale-data loss, segmented storage and other recognized future choices return `UnsupportedPolicy` when requested as runtime behavior. YAML/JSON/XML converters remain milestone 10 and will produce TOML for this same cpptoml loader.

The example registers three process Tasks, MarketData, StrategyHost, and Risk, plus the Strategy thread Task hosted by StrategyHost. Strategy owns StrategyInputs and produces StrategyToRisk. MarketDataUpdates names the Strategy and Risk destination queues. Its capacity values illustrate a bounded configuration. The [primer's TOML lesson](primer.md#7-move-setup-into-a-toml-file) provides a smaller runnable starting point.

## File structure and values

The root key `config_version` is required and must be integer `1` or `2`. Version 1 retains its scalar Field syntax. Version 2 adds the [logical Field forms](logical-fields.md#toml-version-2) and int8. Both contain `session` and `limits` tables, optional `defaults` tables, and arrays of tables declaring Tasks, Source Queue Sets, Queues, Publishing Queue Sets, Fields, and Messages. Declaration arrays may be omitted or empty. Each declared category must fit its reserved capacity.

Names and references are case-sensitive strings. During creation a reference names a declared entity of the required kind; declaration order does not constrain forward references. During file-based attachment a reference may also name an entity already registered in the Session. Entity names identify one entity in their Registry namespace. Source and Publishing Queue Sets share the Queue Set namespace. Duplicate declarations and conflicting identities are configuration errors, even when duplicate descriptors are identical. The optional `doc` key supplies documentation for each declared Task, Queue, Queue Set, Field, or Message.

TOML integers are signed 64-bit values. The loader validates sign and the destination range before converting to an unsigned capacity, identifier, version, seed, or priority. Positive means greater than zero. Values must have their specified TOML type: a quoted number or a floating-point value does not satisfy an integer setting. Unknown tables and keys are rejected so misspelled settings cannot silently select defaults.

## Session

| Key | Type | Contract |
| --- | --- | --- |
| `session.name` | String | Required session name. |
| `session.application_version` | String | Optional application version, defaulting to an empty string. |
| `session.directory` | String | Required directory for session files. A relative path resolves from the TOML file's parent directory. |
| `session.max_log_bytes` | Integer | Required positive limit for the administrative Session file: Registry definitions, topology and administrative journal. Message recording uses the independent per-producer limits below. |
| `session.producer_log_bytes` | Integer | Fixed byte reservation for each producing Task's accepted-message file. Optional, default **67,108,864** (64 MiB), at least 4,288, a multiple of 64, and within the build's supported geometry. |
| `session.producer_log_publications` | Integer | Fixed publication-count bound for each producing Task's message log and dependent reserved indexes/progress. Optional positive integer, default **65,536**, no larger than `(producer_log_bytes - 4096) / 192`. |
| `session.storage` | String | Defaults to `two_plane`. The alternative is `segmented`. |
| `session.segment_bytes` | Integer | Allocation quantum for `segmented`, defaulting to 1,048,576 bytes. Segmented storage is a recognized future option and is unsupported by this runtime. Specifying this key with two-plane storage is an error. |

Session creation identifies a process Task as its creator, as defined by the [public API contract](api.md). That Task consumes a reserved entry and supplies initial registration attribution.

The storage mapping and allocation settings bind when a session is created. `Session::create_from_file(path, creator)` requires the creator to name a declared Process Task and consumes that declaration's entry, without adding an extra Task. It validates the complete configuration before initial registration, then registers definitions and activates complete nonempty topology through the common runtime path. The completed directory is published exclusively only after setup succeeds. A failure does not publish a partial Session or replace an existing target.

`Session::attach_from_file(path)` locates the existing Session through the resolved directory, reads its recorded effective configuration, and validates compatibility. The Session name, application version, administrative log byte limit, both producer-log limits, storage/segment settings, capacity limits and effective policy defaults (including acknowledgment mode) must match or return `ConfigurationMismatch`. The actual canonical directory can differ from the recorded creation location after relocation. Attachment checks supplied declarations and references without applying registrations, changing topology, or claiming Tasks. It does not require supported runtime topology changes to match the original declaration list. Normal Session validation and interrupted recovery may still write to the underlying file. Configuration changes take effect through a new Session or explicit supported Registry APIs.

## Capacity limits

The following TOML `limits` keys are required nonnegative integers. A zero limit reserves no entries or bytes for that facility. The C++ defaults apply to `CapacityLimits{}` and `SessionOptions::_limits`; TOML files still specify every key explicitly.

| Key | C++ default | Reserved capacity |
| --- | ---: | --- |
| `tasks` | 16 | Logical Tasks, including both process and thread Tasks. |
| `queues` | 128 | Named Queues. |
| `source_sets` | 16 | Source Queue Sets. |
| `publishing_sets` | 16 | Publishing Queue Sets. |
| `memberships_per_set` | 32 | Member queues in each Source or Publishing Queue Set. |
| `fields` | 256 | Registered Field definitions. |
| `message_schemas` | 128 | Registered message-schema versions. Each version consumes one entry. |
| `symbols` | 1,024 | Symbol entries. |
| `string_pool_bytes` | 1,048,576 | Bytes reserved for the string pool. |

The [C++ API](api.md#capacity-defaults-and-named-setters) provides named fluent setters for overrides and `CapacityLimits::empty()` for an explicit all-zero profile. Defaults apply at new-session construction; recorded effective limits remain authoritative when attaching to an existing session.

Capacity limits bind at session creation and are recorded in the Registry. Configuration values must fit the build's supported ceilings and the [mapped layout's ranges and byte accounting](mapped-format.md). Source and Publishing Queue Set capacities together must fit 65,535 IDs. The initial declarations and activation resources must fit those limits and the session byte cap before creation can succeed. Registry names, documentation, and schema Field lists consume log arena bytes separately from the reserved application string pool.

Adding a queue consumes one queue entry and one membership in its Source Queue Set. Referencing that queue from a Publishing Queue Set consumes a membership in that publishing set. Multiple publishing references count independently. New registration can use remaining capacity before messaging seals the catalog. Retained historical identities and schema versions continue to consume their entries.

The Queue's configured slot count is a logical transport-credit bound, not permission to overwrite an outstanding obligation. A full Queue returns known-unaccepted `QueueFull` backpressure: no message is recorded and the caller can retry the same eligible issued ID after credit returns. Credit is separate from certified processing progress. Accepted immutable message bytes remain in the producer log after credit or acknowledgment; exhausting its byte or publication-count bound returns `LogFull`. Several outgoing Queues share that producer resource bound.

## Optional defaults

Per-entity settings override their corresponding configuration defaults.

| Key | Default | Allowed values |
| --- | --- | --- |
| `defaults.queue.ring_capacity` | `1024` | Positive integer for `two_plane` storage. |
| `defaults.queue.delivery` | `recover_from_log` | `recover_from_log`, `allow_loss`. |
| `defaults.source_set.selection` | `strict` | `strict`, `fair`, `weighted_random`. |
| `defaults.source_set.acknowledgment` | `required` | `required`, `none`. Both retain message recording/replay. |
| `defaults.source_set.wait` | `spin` | `spin`, `eventfd`. |
| `defaults.publishing_set.acceptance` | `all_destinations` | `all_destinations`, `partial`. |

These are the agreed defaults. The current implementation executes `two_plane`, `recover_from_log`, `strict`, `spin` and either acknowledgment mode. `allow_loss`, alternative selection/waits, segmented storage and Publishing acceptance/retry remain unsupported runtime behavior. Publishing topology may still be declared and inspected; that does not implement publishing delivery. Unsupported choices produce explicit errors rather than selecting a legacy engine. The planned eventfd wait is Linux-specific.

## Tasks

Each `[[tasks]]` entry has a required `name`. Its `kind` defaults to `process` and accepts `process` or `thread`.

A thread Task also specifies `process`, naming its parent process Task. A process Task omits that key. Runtime process and thread identifiers are recorded when a Task attaches. Configuration names logical participants.

Programmatic Task registration is implemented in step 2.6. Its descriptor uses the parent's typed Task ID, and the parent must already be a registered Process Task; it need not be attached yet. Exact re-registration compares name, kind, parent, and documentation, preserving the original registration metadata. A different descriptor under that name returns `Conflict`. File-based creation resolves parent names and registers Process Tasks before their Threads, independently of declaration order.

A Task may produce and consume. Individual Process-Task producer operations are serialized through their shared local owner. Applications preserve their intended ID-issuance/send order and coordinate mutable input/output buffers and access to a common Source. A Thread Task attaches on its owning thread while its parent Process claim is held; its handles retain that parent claim. After fork, a child must exec before any Angaria API use, including a fresh Session attachment.

## Source Queue Sets

Each `[[source_sets]]` entry contains the following keys.

| Key | Contract |
| --- | --- |
| `name` | Required Queue Set name. |
| `owner` | Required consumer Task reference. |
| `selection` | Optional override of the source selection default. |
| `wait` | Optional override of the wait default. |
| `acknowledgment` | Optional `required` or `none`, overriding `defaults.source_set.acknowledgment`. |
| `random_seed` | Optional nonnegative integer for `weighted_random`. Omission generates an effective seed when the set is created and records it with policy settings. Reattachment reads the recorded seed. This key is an error for other selection policies. |

Queue declarations provide membership and priority. The loader checks those relationships together. With `required`, a set owns at most one in-flight Delivery. Its owner acknowledges, releases or closes that pin before proceeding; repeated receive keeps the same token. With `none`, receive returns MessageView and no token obligation, and the next receive attempt ends the previous borrow. Both modes can copy a valid view to an owning Message with its complete observed header. Separate Sources have independent progress. Current public receiving requires one Active input Queue per Source; multiple-input scheduling is milestone 5.

Steps 2.8–2.9 implement programmatic Queue/Queue Set registration, effective policy inspection, membership discovery, draft adoption/assignment, and activation. Source and Publishing names/IDs share one namespace. Creating or moving a Queue commits its definition and inverse Source memberships together. Exact retries compare resolved values, and a Source registration retry remains idempotent after Queues join it. Drafts may be incomplete for later programmatic setup; TOML declarations require the complete references shown here. The step 2.10 loader resolves and validates them before application.

## Queues

Each `[[queues]]` entry contains the following keys.

| Key | Contract |
| --- | --- |
| `name` | Required queue name. |
| `producer` | Required producer Task reference. |
| `source_set` | Required Source Queue Set reference. |
| `priority` | Integer, default `1`. Strict selection permits 0 through 255. Fair and weighted-random selection require 1 through 255. |
| `ring_capacity` | Optional positive override of the queue ring-capacity default for `two_plane` storage. |
| `delivery` | Optional override of the queue delivery default. |

Explicit `ring_capacity` settings apply to two-plane storage and are rejected for segmented storage. Segmented allocation uses `session.segment_bytes`.

An active queue has one producer owner and one Source Queue Set membership. The Source Queue Set identifies the consumer owner. The same queue can be referenced by several Publishing Queue Sets belonging to its producer.

## Publishing Queue Sets

Each `[[publishing_sets]]` entry has required `name`, `owner`, and `destinations` keys. The owner names its producer Task. Destinations are an array of queue names owned by that Task. Repeated destination names are configuration errors. The optional `acceptance` key overrides the publishing acceptance default.

Each publication records its destination snapshot. Later membership changes preserve the obligations and acceptance outcomes already recorded for earlier publications.

## Initial Fields and Messages

Optional `[[fields]]` declarations have required `name`, `type`, and `id` keys, plus an optional `doc` string. The type identifies a supported scalar or logical representation, such as `int64` or version-2 `bool`. The identifier is a full 32-bit value whose high byte is `0xEE`, matching the wiki's 24-bit effective Field identity. Unknown types and conflicting Field names or IDs are errors.

Optional `[[messages]]` declarations have required `name`, `type`, `version`, and `fields` keys, plus an optional `doc` string. The type is an integer from 1 through 65,535. The version is an integer from 1 through 4,294,967,295. Fields are an ordered array of Field names.

A message schema is identified by its type and version. Multiple immutable versions may share a message type and name. Conflicting names for a type, conflicting types for a name, and repeated type/version declarations are errors. Every referenced Field must resolve.

Step 2.7 implements the equivalent programmatic Field/schema registrations and inspections. Field IDs use the complete `0xEE000000`–`0xEEFFFFFF` range; table capacity counts entries rather than the numeric ID. Programmatic exact re-registration is idempotent, while conflicting Field IDs/names return Conflict and conflicting schema identities/type-name pairings return SchemaConflict. Message Field order and repetition are meaningful, and an empty array is valid. Both TOML versions accept `char`, `byte`, `int16`, `uint16`, `int32`, `uint32`, `int64`, `uint64`, `float32`, and `float64`. Their sizes follow the fixed scalar wire width rather than `sizeof` a native C++ object.

Applications may register definitions programmatically within the same reserved capacities. TOML declarations supply initial registrations.

Step 3.3 adds version-2 `int8`, `bool`, `bits` with width 8/16/32/64, `enum` with an explicit integer base and closed value/label mapping, and `chars` with an exact positive ASCII byte size. The [logical Field contract](logical-fields.md#toml-version-2) specifies all keys, strict validation, and decimal-string enum values covering the full uint64 range. The [complete configuration](../examples/primer/logical-fields.toml) is demonstrated in [primer lesson 10](primer.md#10-encode-logical-fields-and-preserve-an-output-buffer).

Current Sessions use mapped format 3.0 and synchronization revision 2. TOML configuration versions 1 and 2 remain independent of native storage version. Old native Sessions are rejected rather than upgraded. Attachment remains validation-only for supplied declarations; it does not replace definitions or reinterpret the exact Field meanings recorded by the Session.

## Registration and activation

A new session validates the complete configuration and resolves defaults before applying its initial Registry declarations. Registration and relationship updates commit atomically so an interrupted update exposes a consistent Registry revision.

Exact-match re-registration is idempotent. A conflicting registration returns a specific error. Committed identities remain stable and are not reused for another entity during the session. Schema versions remain immutable.

Before messaging is sealed, additions within reserved capacity use atomic Registry updates. A draft queue can be created and assigned to a Source Queue Set before activation. Sending and receiving require active entities with complete ownership and membership. Active Source and Publishing Queue Sets have at least one member. Activation depends on complete definitions and allocated resources. A complete configured topology can be activated during session creation while its producer and consumer processes are offline. Empty sets remain draft until complete. Draft entities support subsequent programmatic setup.

Ownership adoption is allowed while an entity is draft. Once active, queue producer/source assignments and Source/Publishing Queue Set owners remain fixed under this initial contract. A replacement Task attaches to the same logical ownership with a new incarnation after the former claim is released. Competing live claims return ownership-busy.

Programmatic adoption requires a nonzero registered target Task, which may be offline. Source assignment can explicitly unassign a draft Queue by passing a zero Source ID. Activation order is Source, then its Queues, then Publishing sets. A Queue's activation budget includes its ring, definition, manifest, redo, and padding; insufficient space leaves it draft. Active Publishing destination replacement requires its owning actor, a nonempty list, and Active Queues owned by that producer. Exact unchanged operations remain no-ops at exhausted capacity/log/counters. Direct Queue sending and single-Queue Source receiving are implemented; multi-Queue selection follows in milestone 5 and Publishing acceptance in milestone 6.

Before messaging admission, Source membership and Publishing destination edits validate and commit their definitions together. After the first successful endpoint opening or publication-ID issuance seals the catalog, new registrations and topology edits are rejected. Exact unchanged registration retries remain idempotent. Register every Task, Field, exact schema and connection before starting messaging; late schema registration cannot silently invalidate an established endpoint's decoder cache.

## Messaging admission and policy binding

Session storage, entity capacities, producer-log bounds and policy defaults bind at Session creation. Entity overrides resolve before activation. The first successful endpoint opening or publication-ID issuance admits messaging and seals a checked catalog. All exact schemas and outgoing Queue bindings are then cached under that authority. New schema registration or topology edits require another fresh development Session; the current runtime provides no live reconfiguration engine.

Established ID issuance, send/status, receive, acknowledge/release/close and committed-acknowledgment watermark reads operate on producer/connection resources without the Session administrative gate. Cold setup, Task claims, catalog operations and admission still use administrative coordination. A full or paused independent connection must not become a global Session messaging gate.

Strict single-input selection is implemented now. The priority and fair/weighted-random names retain their planned multi-input meanings for milestone 5; defining them in a header or TOML vocabulary does not enable those runtimes. Publishing all/partial acceptance and retry follow milestone 6. Their metadata can be registered during unsealed setup, while public publishing messaging remains unsupported.

## Acknowledgment, credit and replay

`required` receives a Delivery token for the exact consumer claim, Source, Queue, Queue sequence and issuance. Acknowledgment commits processing progress; release or close leaves unfinished work available for another issuance. Repeated receive retains the current pin. A stale token cannot finish a new issuance. Consumer replacement can redeliver work not known to be acknowledged.

`none` receives MessageView without a token. Returning Queue credit permits further sending but does not certify processing. A restarted or reopened unacknowledged consumer can read accepted messages again from the retained log. This is distinct from configurable stale-message loss, which is unsupported here.

Accepted public DestinationOutcome values expose their optional `_queueSequence`. For acknowledged Queues, `QueueWriter::acknowledged_sequence()` supplies a committed processing lower bound that may lag after interruption. Compare the Queue sequence with the watermark from that same Queue, not the producer's PublicationId sequence, which can interleave several outgoing Queues. Unacknowledged Queues reject this watermark query with `UnsupportedPolicy`.

Neither returned credit nor processing acknowledgment erases accepted messages or reclaims producer-log space. LogArchive captures accepted prefixes for inspection/replay without manufacturing delivery tokens, changing receipts or reconstructing original consumer execution order. Flat export contains ordinary portable message records; retain the matching catalog for decoding. See [primer lessons 16 and 18](primer.md#16-receive-recorded-messages-without-acknowledgments).

## Validation and converters

Syntax failures include the input filename and the parser's line diagnostic where available. Semantic failures identify the configuration key path, offending value or reference, and violated rule. Unknown keys, unsupported policies, out-of-range values, unresolved references, conflicting identities, and exceeded capacities are explicit errors.

YAML, JSON, and XML conversion is defined against this configuration schema. Equivalent supported values produce equivalent effective TOML configuration. Unsupported or lossy mappings produce conversion errors. Relative-path meaning must be preserved when converted TOML is written to another directory.

The [loader tests](../test/test_file_configuration.cpp) exercise public file factories, semantic failures, capacity boundaries, effective attachment settings, and preservation of existing Sessions. The parser-compatibility check also keeps the documented example within the pinned parser's syntax. Converter implementations and equivalence tests follow in milestone 10.
