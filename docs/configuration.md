# Angaria configuration and lifecycle contract

This document defines Angaria's configuration schema and lifecycle rules. Step 2.10 implements TOML loading and the file-based Session factories against the same descriptor and policy validation used by programmatic registration.

Configuration files use TOML through cpptoml revision `fededad7169e538ca47e11a9ee9251bc361a9a65`. The syntax contract targets TOML v0.5.0, which the upstream README documents. The [trading configuration](../examples/trading.toml) illustrates the three-process topology from the [milestone 1 plan](milestones/milestone-1.md). [cpptoml source](https://github.com/skystrife/cpptoml/tree/fededad7169e538ca47e11a9ee9251bc361a9a65)

The current native profile supports two-plane storage, scalar and basic logical Fields, strict selection, spin waiting, recovery-from-log and allowed-loss delivery, and all-destination acceptance. Other recognized policy choices below describe the planned contract and return `UnsupportedPolicy` until implemented. The [binary payload codec](message-payload.md) encodes these registered definitions. [Direct Queue send and acceptance lookup](direct-publication.md) are implemented in step 4.6; [public receive](native-receive.md) in step 4.7 gains allowed loss in step 4.8. YAML, JSON, and XML converters arrive in milestone 10 and produce TOML for this same loader.

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
| `session.max_log_bytes` | Integer | Required positive byte limit, including the persisted Registry and message history. |
| `session.storage` | String | Defaults to `two_plane`. The alternative is `segmented`. |
| `session.segment_bytes` | Integer | Allocation quantum for `segmented`, defaulting to 1,048,576 bytes. Format 2 requires at least 4,160 bytes and a multiple of 64, within the session byte cap. An extent may use multiple quanta for a large record. Specifying this key with two-plane storage is an error. |

Session creation identifies a process Task as its creator, as defined by the [public API contract](api.md). That Task consumes a reserved entry and supplies initial registration attribution.

The storage mapping and allocation settings bind when a session is created. `Session::create_from_file(path, creator)` requires the creator to name a declared Process Task and consumes that declaration's entry, without adding an extra Task. It validates the complete configuration before initial registration, then registers definitions and activates complete nonempty topology through the common runtime path. The completed directory is published exclusively only after setup succeeds. A failure does not publish a partial Session or replace an existing target.

`Session::attach_from_file(path)` locates the existing Session through the resolved directory, reads its recorded effective configuration, and validates compatibility. The Session name, application version, log byte limit, storage/segment settings, capacity limits, and policy defaults must match or return `ConfigurationMismatch`. The actual canonical directory can differ from the recorded creation location after relocation. Attachment checks supplied declarations and references without applying registrations, changing topology, or claiming Tasks. It does not require supported runtime topology changes to match the original declaration list. Normal Session validation and interrupted recovery may still write to the underlying file. Configuration changes take effect through a new Session or explicit supported Registry APIs.

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

Adding a queue consumes one queue entry and one membership in its Source Queue Set. Referencing that queue from a Publishing Queue Set consumes a membership in that publishing set. Multiple publishing references count independently. Runtime registration can use remaining capacity. Retained historical identities and schema versions continue to consume their entries.

The ring's configured slot count bounds its live offset index. Recover-from-log delivery continues through retained history after ring wraparound. This contract introduces no separate pending-delivery count limit. Exhausting the log byte limit returns a log-full result.

## Optional defaults

Per-entity settings override their corresponding configuration defaults.

| Key | Default | Allowed values |
| --- | --- | --- |
| `defaults.queue.ring_capacity` | `1024` | Positive integer for `two_plane` storage. |
| `defaults.queue.delivery` | `recover_from_log` | `recover_from_log`, `allow_loss`. |
| `defaults.source_set.selection` | `strict` | `strict`, `fair`, `weighted_random`. |
| `defaults.source_set.wait` | `spin` | `spin`, `eventfd`. |
| `defaults.publishing_set.acceptance` | `all_destinations` | `all_destinations`, `partial`. |

These are the agreed defaults. A selected policy must be supported by the build, platform, and Session's recorded capabilities. The current native implementation accepts `two_plane`, either delivery policy, `strict`, `spin`, and `all_destinations`; other known choices return `UnsupportedPolicy`. The planned eventfd policy is Linux-specific. Allowed loss requires the `0x8` capability included in new Sessions; existing 2.2/`0x7` Sessions retain recovery delivery and reject requests for allowed-loss defaults or Queues before registration writes.

## Tasks

Each `[[tasks]]` entry has a required `name`. Its `kind` defaults to `process` and accepts `process` or `thread`.

A thread Task also specifies `process`, naming its parent process Task. A process Task omits that key. Runtime process and thread identifiers are recorded when a Task attaches. Configuration names logical participants.

Programmatic Task registration is implemented in step 2.6. Its descriptor uses the parent's typed Task ID, and the parent must already be a registered Process Task; it need not be attached yet. Exact re-registration compares name, kind, parent, and documentation, preserving the original registration metadata. A different descriptor under that name returns `Conflict`. File-based creation resolves parent names and registers Process Tasks before their Threads, independently of declaration order.

A Task may produce and consume. Applications coordinate sending threads that share a producer endpoint, including overlapping publishing destinations.

## Source Queue Sets

Each `[[source_sets]]` entry contains the following keys.

| Key | Contract |
| --- | --- |
| `name` | Required Queue Set name. |
| `owner` | Required consumer Task reference. |
| `selection` | Optional override of the source selection default. |
| `wait` | Optional override of the wait default. |
| `random_seed` | Optional nonnegative integer for `weighted_random`. Omission generates an effective seed when the set is created and records it with policy settings. Reattachment reads the recorded seed. This key is an error for other selection policies. |

Queue declarations provide membership and priority. The loader checks those relationships together. A set owns at most one in-flight delivery under this initial contract, and its owner must acknowledge or release it before receiving another delivery. Separate Source Queue Sets have independent in-flight state.

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

New Sessions use mapped format 2.2/`0xf`, including logical-Field support, native direct publication, and allowed-loss capability, even when their TOML uses version 1 and their default delivery is recovery. TOML versions 1 and 2 both accept `allow_loss` for `defaults.queue.delivery` or an individual Queue's `delivery` override. Existing format-2.0 Sessions retain scalar registration and reject new logical/int8 registrations without mutation. Existing 2.2/`0x7` Sessions remain usable without an in-place capability upgrade. Attachment remains validation-only: supplied declarations do not register, replace, or reinterpret recorded definitions. Read the attached Session's Registry to obtain its authoritative Field meanings.

## Registration and activation

A new session validates the complete configuration and resolves defaults before applying its initial Registry declarations. Registration and relationship updates commit atomically so an interrupted update exposes a consistent Registry revision.

Exact-match re-registration is idempotent. A conflicting registration returns a specific error. Committed identities remain stable and are not reused for another entity during the session. Schema versions remain immutable.

Runtime additions are allowed within reserved capacity through atomic Registry updates. A draft queue can be created and assigned to a Source Queue Set before activation. Sending and receiving require active entities with complete ownership and membership. Active Source and Publishing Queue Sets have at least one member. Activation depends on complete definitions and allocated resources. A complete configured topology can be activated during session creation while its producer and consumer processes are offline. Empty sets remain draft until complete. Draft entities support subsequent programmatic setup.

Ownership adoption is allowed while an entity is draft. Once active, queue producer/source assignments and Source/Publishing Queue Set owners remain fixed under this initial contract. A replacement Task attaches to the same logical ownership with a new incarnation after the former claim is released. Competing live claims return ownership-busy.

Programmatic adoption requires a nonzero registered target Task, which may be offline. Source assignment can explicitly unassign a draft Queue by passing a zero Source ID. Activation order is Source, then its Queues, then Publishing sets. A Queue's activation budget includes its ring, definition, manifest, redo, and padding; insufficient space leaves it draft. Active Publishing destination replacement requires its owning actor, a nonempty list, and Active Queues owned by that producer. Exact unchanged operations remain no-ops at exhausted capacity/log/counters. Direct Queue sending and single-Queue Source receiving are implemented; multi-Queue selection follows in milestone 5 and Publishing acceptance in milestone 6.

Source membership may gain a newly registered queue through an atomic topology update. Publishing membership changes occur with no publication call in progress for that set and create a new topology generation. The update validates all destination ownership and capacity constraints before commit. Previously committed destination snapshots retain their meaning.

## Policy binding and scheduling

The [milestone-4 transport review](native-transport.md#native-compatibility-and-required-capabilities) specifies native delivery and allowed-loss capability admission. Step 4.8 enables `allow_loss` through the common programmatic/TOML validation path and the required capability recorded at Session creation. This requires no TOML schema or mapped-format minor change. Unknown required capabilities still reject attachment before interrupted journal recovery.

Session storage and capacity settings bind at session creation. Queue delivery, Source selection/wait, and Publishing acceptance choices resolve from explicit values and defaults before entity activation. Active policy choices are fixed under this initial contract. A changed policy uses a new entity with the desired configuration.

Strict selection prefers the largest priority and selects equal-priority available queues round robin. Priority zero is the lowest strict priority.

At the start of a fair-selection round, every member queue receives a message budget equal to its positive priority. Each newly selected delivery consumes one credit, whether its payload comes from the ring or retained log. Re-reading the current in-flight delivery consumes no additional credit. Reconstructing state during recovery does not itself consume credit.

Eligible queues with remaining credit receive service in descending priority, with round-robin ties. Empty queues do not delay a new round. Once all currently available queues exhaust their credits, budgets reset. An existing queue that becomes available during a round uses its remaining budget. A newly registered member starts with its full priority budget for the current round.

Weighted-random selection chooses among available queues in proportion to their positive priorities. Receiving the same in-flight delivery again does not perform a new random selection. Detailed algorithm and distribution checks are delivered in milestone 5.

## Acknowledgment and overrun

Delivery ordering and skipped ranges use queue-local sequence numbers. Receive timeouts use a monotonic clock.

A receive issues a token identifying the selected delivery and its consumer claim. Acknowledgment requires that exact current token. A successful acknowledgment advances that destination's progress and releases the in-flight slot. An invalid or stale token fails without advancing progress.

Re-reading an in-flight delivery returns the same delivery. Explicit release, endpoint closure, or Task termination before acknowledgment leaves the delivery pending. A replacement can receive it again under the queue's delivery policy.

With `allow_loss`, an issued delivery's payload remains pinned until acknowledgment or release. Ring overwrite does not invalidate an already issued payload. After release or consumer replacement, apply the current overrun/loss rules before selecting the next delivery. A fresh receive can mark the contiguous overwritten pending prefix Lost and return a `DeliveryGap` with the Queue and skipped sequence range. It preserves the Queue's last actual acknowledgment time and any historical consumer/dequeue tags. The next receive starts at the first retained nonterminal sequence. Loss uses reserved progress state at the log byte cap, without appending records or reclaiming message history and payloads; it does not free capacity for new sends.

With `recover_from_log`, retained publication and acknowledgment state reconstructs pending deliveries in queue order. Required unacknowledged records remain retained. The configured log capacity provides the resource bound. The [mapped layout](mapped-format.md) and [journal protocol](concurrency.md) reserve progress storage so acknowledgment, release, issuance, and replacement attachment remain possible when publication storage is full. Initial runtime mutations use a blocking session gate; there is no end-to-end lock-free guarantee.

## Validation and converters

Syntax failures include the input filename and the parser's line diagnostic where available. Semantic failures identify the configuration key path, offending value or reference, and violated rule. Unknown keys, unsupported policies, out-of-range values, unresolved references, conflicting identities, and exceeded capacities are explicit errors.

YAML, JSON, and XML conversion is defined against this configuration schema. Equivalent supported values produce equivalent effective TOML configuration. Unsupported or lossy mappings produce conversion errors. Relative-path meaning must be preserved when converted TOML is written to another directory.

The [loader tests](../test/test_file_configuration.cpp) exercise public file factories, semantic failures, capacity boundaries, effective attachment settings, and preservation of existing Sessions. The parser-compatibility check also keeps the documented example within the pinned parser's syntax. Converter implementations and equivalence tests follow in milestone 10.
