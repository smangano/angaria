# Milestone 1 Core architecture and policy contracts

Milestone 1 establishes the contracts needed to implement Angaria's shared Registry, mapped queues, Queue Sets, and policies. Completion requires a reviewed public C++ API example for a bounded multi-process application and a concrete mapped layout and concurrency plan.

The staging branch is `milestone-1`. Each step uses its own branch and a PR targeting the staging branch. The completed milestone reaches `main` through a milestone PR.

## Review steps

| Step | Branch | Scope | Completion criterion |
| --- | --- | --- | --- |
| 1.1 | `milestone-1-1` | Domain objects, ownership, membership, delivery and acceptance invariants, failure outcomes, and a representative topology. | Review the core contract in this document against the agreed architecture. |
| 1.2 | `milestone-1-2` | TOML configuration schema and compatibility, bounded configuration, registration lifecycle, policy binding and mutability, and acknowledgment scope. | Specify the remaining lifecycle and configuration contracts with explicit defaults. |
| 1.3 | `milestone-1-3` | Public C++ identities, descriptors, policy/result types, and API examples. | Compile-check an example that expresses the accepted contracts. |
| 1.4 | `milestone-1-4` | Mapped representations, offsets, ID widths, timestamps, schema identity, capacities, and compatibility. | Review a layout and versioning plan suitable for Registry implementation. |
| 1.5 | `milestone-1-5` | Shared allocation, publication commits, interrupted-write recovery, and progress guarantees. | Validate crash/concurrency protocols and reconcile them with the API and layout. |

Each step carries forward the contracts integrated by the preceding PRs. The following sections define step 1.1. The [configuration and lifecycle contract](../configuration.md) specifies step 1.2, with a [TOML example](../../examples/trading.toml) parsed by the pinned cpptoml revision. Steps 1.3 through 1.5 build on those contracts.

## Domain objects

| Object | Contract |
| --- | --- |
| Registry | Shared definitions, identities, topology, ownership, effective limits, and lifecycle metadata. Its persisted representation supplies the historical definitions needed to interpret captured sessions. |
| Task | A registered logical participant representing a process or thread. It can produce and consume, own multiple endpoints, and restart under a stable identity with a new incarnation. |
| Field definition | Registered type information and metadata describing a unit of message data. |
| Message schema | An immutable versioned definition identifying an ordered sequence of fields. |
| Message | An immutable committed payload with a header and an exact schema identity. |
| Queue | A named FIFO delivery stream with one producer Task and membership in one consumer-owned Source Queue Set. |
| Source Queue Set | A named collection of queues owned by a consumer Task. Its selection policy chooses the next member queue to read. |
| Publishing Queue Set | A named collection of destination queue references owned by a producer Task. Its acceptance policy governs one publication to those destinations. |
| Publication | One identified send to a queue or publishing set, including its committed destination snapshot and acceptance outcome. |
| Delivery | One destination queue's obligation to deliver an accepted message, with independent acknowledgment or configured loss progress. |

## Ownership and membership

A queue has one producer owner. The application coordinates sending threads that share that queue. Concurrent publication through overlapping publishing sets also requires coordinated access to their shared destination queues.

A queue available for sending or receiving belongs to exactly one Source Queue Set, whose consumer Task owns its read operations. A Task may own multiple Source Queue Sets. Selection preserves each member queue's delivery order subject to its delivery-loss policy.

A queue can be referenced by multiple Publishing Queue Sets belonging to its producer Task. Source membership and publishing references are separate Registry relationships. A publication's destination snapshot fixes the recipients of that publication even if a later topology revision changes the publishing set.

Registry registration records ownership and membership consistently. Queue creation, source assignment, publication references, and attachment use registered identities. Ownership changes and registration timing are specified in step 1.2.

## Bounded topology

Angaria targets a small, explicitly bounded network, typically ten or fewer cooperating processes. Configuration and build settings bound Tasks, Queues, Queue Sets, memberships, message definitions, Fields, symbols, and string-pool storage.

The effective limits are recorded and validated when participants attach. Within reserved capacity, the registration lifecycle determines when new entities and relationships can be added. Capacity exhaustion has an explicit result.

Each queue has its own sequence of deliveries. A Source Queue Set's selection policy determines interleaving across its member queues. Configuring separate queues for sending threads therefore also creates separate scheduling inputs.

## Configuration format

Angaria configuration files use TOML, with [cpptoml](https://github.com/skystrife/cpptoml) as the TOML implementation. Users of YAML, JSON, or XML receive configuration support through converters that emit TOML.

The converters map Angaria configuration data into its TOML schema and pass through the common TOML loader and validation path. Unsupported or lossy mappings produce explicit conversion errors. This configuration conversion contract is separate from the message serialization formats in milestone 9.

The [step 1.2 contract](../configuration.md) pins the cpptoml revision and specifies TOML v0.5.0 syntax, schema, defaults, and validation diagnostics. Milestone 2 integrates configuration loading, and milestone 10 delivers the converters.

## Policy model

| Policy family | Behavior to express | Agreed default or scope |
| --- | --- | --- |
| Storage mapping | Two-plane offset rings with an append-only payload plane, or segmented growth. | Selected storage policy must preserve the queue and publication contracts. Binding is specified in step 1.2. |
| Source selection | Strict priority with round-robin ties, proportional-budget fairness, or weighted-random selection. | Applies to a Source Queue Set. Detailed scheduling rules follow in the configuration contracts and milestone 5. |
| Consumer synchronization | Spinning or Linux eventfd waiting. | Waits on a Source Queue Set. Notification discovery and restart behavior follow in the concurrency plan and milestone 7. |
| Queue delivery | Recover from the retained log, or allow delivery loss on ring overrun. | Recovery from the log is the default for each queue. |
| Publishing acceptance | All destinations accept, or partial acceptance with per-destination results. | All-destination acceptance is the default for each Publishing Queue Set. |

The composition must preserve the accepted ownership, ordering, acknowledgment, and failure contracts. Step 1.2 specifies binding and mutability. Step 1.5 examines the coordination and progress guarantees required by the combinations.

## Delivery and acknowledgment

Under the default queue delivery policy, a lagging consumer resumes from its oldest unacknowledged delivery using the retained log. The stored state must identify intended destinations and their acknowledgment progress so recovery can reconstruct delivery after ring overwrite or consumer replacement.

Under the configured loss policy, a consumer skips overwritten deliveries and receives the skipped sequence range. Committed payloads remain available for inspection under the log's retention rules.

Acknowledgment advances progress for the selected destination independently of other destinations. A consumer that completes external work and dies before acknowledging can cause that work to be repeated. Application handlers must tolerate that possibility.

The [step 1.2 contract](../configuration.md) specifies one in-flight delivery per Source Queue Set, token-scoped acknowledgment or release, and payload pinning across ring overrun. The prototype's one-delivery API supplies a regression case for this design work.

## Publishing acceptance

All-destination acceptance commits delivery obligations for the complete destination snapshot. If any destination cannot accept under its configured policies, the publication rejects without changing the queues or log.

Partial acceptance commits obligations for the accepted subset and returns accepted destinations, rejected destinations, and rejection reasons. If no destination accepts, the operation rejects without changing the queues or log.

A persistent publication identity connects the message, destination snapshot, and acceptance outcome. Recovery must distinguish committed obligations from abandoned writes. Retrying rejected destinations must preserve the recorded acceptance of destinations that already accepted the publication.

Acceptance and consumer acknowledgment have separate progress. A destination configured for allowed loss can subsequently skip overwritten deliveries. All-destination acceptance therefore composes with independently configured queue delivery policies.

## Failure outcomes

| Situation | Required observable outcome |
| --- | --- |
| Invalid identity, schema, ownership, or operation | Return a specific failure for the violated contract. State transitions remain consistent. |
| Competing attachment to an occupied endpoint | Return an ownership-busy result. |
| Known queue or log capacity rejection | Preserve the queues and log and report the exhausted resource. |
| All-destination publication with an unavailable destination | Reject the complete publication and identify the rejection cause. Preserve the queues and log. |
| Partial acceptance | Return the accepted and rejected destinations with their outcomes. |
| Lagging consumer under recovery | Deliver the retained backlog in queue order. |
| Ring overrun under allowed loss | Report the skipped sequence range and resume eligible deliveries. |
| Process termination during a send or acknowledgment | Recover the recorded commit state. The interrupted caller can have an unknown outcome, resolved using recorded publication and delivery state. |

A failed I/O operation or process termination can interrupt a state transition after writes have begun. The allocation and commit protocols in step 1.5 specify recovery for those boundaries. The unchanged-state guarantee for a known rejection must remain distinguishable from an interrupted operation.

The default recovery policy retains records required for unacknowledged deliveries. Exhausting the configured log capacity returns a log-full result. Process-crash recovery is part of these contracts. Power-loss durability has a separate contract.

## Representative topology

The following illustrative configuration uses three process Tasks and three queues. Priority values and the loss setting demonstrate legal choices.

| Queue | Producer Task | Source Queue Set | Consumer Task | Priority | Delivery policy |
| --- | --- | --- | --- | --- | --- |
| MarketDataToStrategy | MarketData | StrategyInputs | Strategy | 100 | Allow delivery loss |
| MarketDataToRisk | MarketData | RiskInputs | Risk | 100 | Recover from log |
| StrategyToRisk | Strategy | RiskInputs | Risk | 200 | Recover from log |

MarketData owns a Publishing Queue Set named MarketDataUpdates referencing MarketDataToStrategy and MarketDataToRisk. Its default acceptance policy requires both destinations to accept one publication.

Strategy consumes MarketDataToStrategy through StrategyInputs and sends its own messages through StrategyToRisk. Risk selects between two producers' queues through RiskInputs. Each queue has one producer, and its Source membership coexists with any publishing reference.

This example exercises both Queue Set types, a Task that produces and consumes, multiple producers feeding one Source Queue Set, and different delivery policies on destinations of the same publication. Step 1.3 expresses this topology through the public C++ API.

## Decisions assigned to later steps

- Step 1.2 specifies the TOML schema, cpptoml revision and syntax compatibility, converter mappings, configuration bounds and defaults, registration and membership lifecycle, policy binding and mutability, adoption/ownership changes, acknowledgment scope, and scheduling parameters.
- Step 1.4 specifies binary representations and compatibility, including the wiki's 32-bit Field type identity, historical schema references, and mapped-memory capacity validation.
- Step 1.5 specifies shared payload allocation, publication visibility and commit recovery, notification resource coordination, and the scope of lock-free guarantees. Single-producer rings still share payload allocation and fan-out coordination requirements.

[Project roadmap](../../ROADMAP.md)
