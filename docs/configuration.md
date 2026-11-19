# Angaria configuration and lifecycle contract

This document defines the proposed step 1.2 defaults for review. It carries forward the agreed single-producer topology, recovery-from-log delivery default, and all-destination publication acceptance default.

Configuration files use TOML through cpptoml revision `fededad7169e538ca47e11a9ee9251bc361a9a65`. The syntax contract targets TOML v0.5.0, which the upstream README documents. The [trading configuration](../examples/trading.toml) illustrates the three-process topology from the [milestone 1 plan](milestones/milestone-1.md). [cpptoml source](https://github.com/skystrife/cpptoml/tree/fededad7169e538ca47e11a9ee9251bc361a9a65)

The step 1.2 compatibility check parses the example through the pinned parser. Milestone 2 delivers the Registry configuration loader and semantic validation. YAML, JSON, and XML converters arrive in milestone 10 and produce TOML for this same loader.

The example registers three process Tasks, MarketData, StrategyHost, and Risk, plus the Strategy thread Task hosted by StrategyHost. Strategy owns StrategyInputs and produces StrategyToRisk. MarketDataUpdates publishes to the Strategy and Risk inputs. Its capacity values illustrate a bounded configuration for review.

## File structure and values

The root key `config_version` is required and must be the integer `1`. The configuration contains `session` and `limits` tables, optional `defaults` tables, and arrays of tables declaring Tasks, Source Queue Sets, Queues, Publishing Queue Sets, Fields, and Messages. Declaration arrays may be omitted or empty. Each declared category must fit its reserved capacity.

Names and references are case-sensitive strings. A reference names a declared or registered entity of the required kind. Entity names identify one entity in their Registry namespace. Source and Publishing Queue Sets share the Queue Set namespace. Duplicate declarations and conflicting identities are configuration errors.

TOML integers are signed 64-bit values. Validate sign and the destination range before converting to an unsigned capacity, identifier, version, seed, or priority. Positive means greater than zero. Unknown tables and keys are rejected so misspelled settings cannot silently select defaults.

## Session

| Key | Type | Contract |
| --- | --- | --- |
| `session.name` | String | Required session name. |
| `session.application_version` | String | Optional application version, defaulting to an empty string. |
| `session.directory` | String | Required directory for session files. A relative path resolves from the TOML file's parent directory. |
| `session.max_log_bytes` | Integer | Required positive byte limit, including the persisted Registry and message history. |
| `session.storage` | String | Defaults to `two_plane`. The alternative is `segmented`. |
| `session.segment_bytes` | Integer | Positive allocation size for `segmented`, defaulting to 1,048,576 bytes. Specifying this key with two-plane storage is an error. |

Session creation identifies a process Task as its creator, as defined by the [public API contract](api.md). That Task consumes a reserved entry and supplies initial registration attribution.

The storage mapping and allocation settings bind when a session is created. Existing-session attachment reads its recorded effective configuration and validates compatibility. When an attachment supplies configuration, its resolved session name, storage settings, and capacity limits must match the recorded values or return a configuration-mismatch result. Configuration changes take effect through a new session or the explicit registration APIs for supported topology additions.

## Capacity limits

The following `limits` keys are required nonnegative integers. A zero limit reserves no entries or bytes for that facility.

| Key | Reserved capacity |
| --- | --- |
| `tasks` | Logical Tasks, including both process and thread Tasks. |
| `queues` | Named Queues. |
| `source_sets` | Source Queue Sets. |
| `publishing_sets` | Publishing Queue Sets. |
| `memberships_per_set` | Member queues in each Source or Publishing Queue Set. |
| `fields` | Registered Field definitions. |
| `message_schemas` | Registered message-schema versions. Each version consumes one entry. |
| `symbols` | Symbol entries. |
| `string_pool_bytes` | Bytes reserved for the string pool. |

Capacity limits bind at session creation and are recorded in the Registry. Configuration values must fit the build's supported ceilings and the file-format ranges defined in step 1.4. The initial declarations must fit those limits before session creation can succeed.

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

The two delivery and acceptance defaults are agreed architecture. Ring capacity, strict selection, and spinning are proposed step 1.2 defaults. A selected policy must be supported by the build and platform. The eventfd policy is Linux-specific.

## Tasks

Each `[[tasks]]` entry has a required `name`. Its `kind` defaults to `process` and accepts `process` or `thread`.

A thread Task also specifies `process`, naming its parent process Task. A process Task omits that key. Runtime process and thread identifiers are recorded when a Task attaches. Configuration names logical participants.

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

Optional `[[fields]]` declarations have required `name`, `type`, and `id` keys, plus an optional `doc` string. The type identifies a supported Field base type, such as `int64`. The identifier is a full 32-bit value whose high byte is `0xEE`, matching the wiki's 24-bit effective Field identity. Unknown base types and conflicting Field names or IDs are errors.

Optional `[[messages]]` declarations have required `name`, `type`, `version`, and `fields` keys, plus an optional `doc` string. The type is an integer from 1 through 65,535. The version is an integer from 1 through 4,294,967,295. Fields are an ordered array of Field names.

A message schema is identified by its type and version. Multiple immutable versions may share a message type and name. Conflicting names for a type, conflicting types for a name, and repeated type/version declarations are errors. Every referenced Field must resolve.

Applications may register definitions programmatically within the same reserved capacities. TOML declarations supply initial registrations.

## Registration and activation

A new session validates the complete configuration and resolves defaults before applying its initial Registry declarations. Registration and relationship updates commit atomically so an interrupted update exposes a consistent Registry revision.

Exact-match re-registration is idempotent. A conflicting registration returns a specific error. Committed identities remain stable and are not reused for another entity during the session. Schema versions remain immutable.

Runtime additions are allowed within reserved capacity through atomic Registry updates. A draft queue can be created and assigned to a Source Queue Set before activation. Sending and receiving require active entities with complete ownership and membership. Active Source and Publishing Queue Sets have at least one member. Activation depends on complete definitions and allocated resources. A complete configured topology can be activated during session creation while its producer and consumer processes are offline. Empty sets remain draft until complete. Draft entities support subsequent programmatic setup.

Ownership adoption is allowed while an entity is draft. Once active, queue producer/source assignments and Source/Publishing Queue Set owners remain fixed under this initial contract. A replacement Task attaches to the same logical ownership with a new incarnation after the former claim is released. Competing live claims return ownership-busy.

Source membership may gain a newly registered queue through an atomic topology update. Publishing membership changes occur with no publication call in progress for that set and create a new topology generation. The update validates all destination ownership and capacity constraints before commit. Previously committed destination snapshots retain their meaning.

## Policy binding and scheduling

Session storage and capacity settings bind at session creation. Queue delivery, Source selection/wait, and Publishing acceptance choices resolve from explicit values and defaults before entity activation. Active policy choices are fixed under this initial contract. A changed policy uses a new entity with the desired configuration.

Strict selection prefers the largest priority and selects equal-priority available queues round robin. Priority zero is the lowest strict priority.

At the start of a fair-selection round, every member queue receives a message budget equal to its positive priority. Each newly selected delivery consumes one credit, whether its payload comes from the ring or retained log. Re-reading the current in-flight delivery consumes no additional credit. Reconstructing state during recovery does not itself consume credit.

Eligible queues with remaining credit receive service in descending priority, with round-robin ties. Empty queues do not delay a new round. Once all currently available queues exhaust their credits, budgets reset. An existing queue that becomes available during a round uses its remaining budget. A newly registered member starts with its full priority budget for the current round.

Weighted-random selection chooses among available queues in proportion to their positive priorities. Receiving the same in-flight delivery again does not perform a new random selection. Detailed algorithm and distribution checks are delivered in milestone 5.

## Acknowledgment and overrun

Delivery ordering and skipped ranges use queue-local sequence numbers. Receive timeouts use a monotonic clock.

A receive issues a token identifying the selected delivery and its consumer claim. Acknowledgment requires that exact current token. A successful acknowledgment advances that destination's progress and releases the in-flight slot. An invalid or stale token fails without advancing progress.

Re-reading an in-flight delivery returns the same delivery. Explicit release, endpoint closure, or Task termination before acknowledgment leaves the delivery pending. A replacement can receive it again under the queue's delivery policy.

With `allow_loss`, an issued delivery's payload remains pinned until acknowledgment or release. Ring overwrite does not invalidate an already issued payload. After release or consumer replacement, apply the current overrun/loss rules before selecting the next delivery. An overwritten pending delivery can then be skipped with its sequence range reported.

With `recover_from_log`, retained publication and acknowledgment state reconstructs pending deliveries in queue order. Required unacknowledged records remain retained. The configured log capacity provides the resource bound. Delivery-progress storage must be reserved so acknowledgment and release remain possible when the publication log is full. Steps 1.4 and 1.5 specify that layout and commit protocol.

## Validation and converters

Syntax failures include the input filename and the parser's line diagnostic where available. Semantic failures identify the configuration key path, offending value or reference, and violated rule. Unknown keys, unsupported policies, out-of-range values, unresolved references, conflicting identities, and exceeded capacities are explicit errors.

YAML, JSON, and XML conversion is defined against this configuration schema. Equivalent supported values produce equivalent effective TOML configuration. Unsupported or lossy mappings produce conversion errors. Relative-path meaning must be preserved when converted TOML is written to another directory.

The example's parser-compatibility check establishes that the selected cpptoml revision accepts the chosen TOML syntax. Full loader behavior, semantic-error cases, and Registry-application tests follow in milestone 2. Converter implementations and equivalence tests follow in milestone 10.
