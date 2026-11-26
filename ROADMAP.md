# Angaria roadmap

<p align="center">
  <a href="assets/logos/README.md"><img src="assets/logos/A1_original_web.png" alt="Angaria — C++ messaging library" width="200"></a>
</p>

Angaria is a C++ inter-process messaging framework built around a shared persistent Registry, Source Queue Sets, Publishing Queue Sets, and selectable policies. The target application is a small, bounded network of cooperating processes, typically ten or fewer, with Tasks representing processes or threads.

The ten milestones below guide implementation. Milestone 1 is complete through [PR #7](https://github.com/smangano/angaria/pull/7), [milestone 2](docs/milestones/milestone-2.md) through its eleven reviewed steps, and [milestone 3](docs/milestones/milestone-3.md) through its six reviewed steps and integration release `0.3.7`. [Milestone 4](docs/milestones/milestone-4.md) is in native transport contract review. Milestone 6 delivers the [five-process trading simulation](#demonstration-application) using the Registry, both Queue Set types, and recoverable mapped messages. Milestones 7 through 10 complete the synchronization, storage, field, and tooling coverage.

## Living programmer primer

The [Angaria primer](docs/primer.md) teaches the implemented public API through small runnable programs, then builds toward multi-process applications. Keep its language accessible to junior programmers. Every milestone includes updating the primer and its [compiled examples](examples/primer/) for new public features, supported policies, and lifecycle rules; milestone completion reviews include that update. CI runs the examples so changes cannot silently leave the learning path broken. Planned topics remain clearly marked until their APIs work.

## Versioning

The framework uses `major.minor.build`, recorded in the root [VERSION](VERSION) file and read by CMake. During roadmap development, major is `0` and minor is the milestone number. Starting with milestone 3, build starts at `1` for each milestone and increases once for every new PR within that milestone. This includes fixes, documentation changes, and milestone integration PRs. Additional commits, review fixes, and CI reruns within one PR retain its assigned version.

Milestone 2 keeps its issued versions through integration release `0.2.14`. The first milestone-3 step is `0.3.1`; its next PR uses `0.3.2`. The first milestone-4 PR starts again at `0.4.1`. Assign the next unused build number within the current milestone when opening a PR and include it in the description. Check previously assigned versions, including closed PRs, to avoid reusing a version.

Tracking begins with the versioning PR at `0.2.1`; earlier PRs remain unversioned. Completion of all ten milestones produces release **`1.0.0`**, resetting the development counter. Framework versions are independent of configuration, mapped-format, message-schema, and synchronization protocol versions; those retain their own compatibility rules.

## Demonstration application

The agreed milestone-6 demo is a trading simulation with five cooperating application processes:

| Process | Role |
| --- | --- |
| Market-data feed | Produce a repeatable synthetic quote workload. |
| Strategy | Consume quotes and execution reports, and generate simulated orders using simple rules. |
| Risk gateway | Apply simple configured checks and forward approved orders to the exchange simulator. |
| Exchange simulator | Process approved orders and produce simulated execution reports. |
| Audit consumer | Observe orders, risk decisions, and execution reports through its own destination queues. |

Each producer-consumer connection uses a separate single-producer queue in the consumer's Source Queue Set. Publishing Queue Sets fan out messages, including quotes and execution reports. Quote queues may allow stale-data loss with visible gap reporting; order, risk-decision, and execution-report paths recover from the log and acknowledge independently. Configuration uses TOML through the common loader.

The complete demo must exercise Source selection policies, all-destination acceptance, configurable partial acceptance/retry, ring wraparound, bounded log exhaustion, and producer/consumer termination and restart. Rejection scenarios require a destination that actually cannot accept: an offline consumer or overwritten ring alone does not cause rejection. Assertions cover message contents, per-queue order, acceptance results, gaps, and acknowledgment/redelivery progress.

Use stable application order and execution identifiers to recognize repeated processing after redelivery. Transport redelivery does not provide exactly-once application effects or a total order across producer queues. Detailed schemas, queue wiring, workload bounds, and simulated-order processing rules will be reviewed during planning.

| Milestone | Demo objective |
| --- | --- |
| 2–3 | Plan the topology, schemas, workloads, and failure scenarios against the Registry and message APIs. |
| 4 | Build the first runnable multi-process path using mapped messages and log recovery. |
| 5 | Exercise multiple inputs and Source Queue Set selection policies. |
| 6 | Deliver the complete five-process application and the acceptance scenarios above. |
| 7–10 | Extend the same application across waiting/storage policies, then add release walkthroughs and measurements. |

## Agreed architecture

### Registry and bounded topology

The Registry is the shared source of definitions, identities, ownership, topology, and lifecycle metadata. It is persisted with message logs so tools can interpret captured sessions using their historical definitions.

Each queue has one registered producer Task and belongs to one consumer-owned Source Queue Set. A producer arranges a destination queue in its consumer's Source Queue Set. A Publishing Queue Set references destination queues owned by its producer. A queue can be referenced by several publishing sets belonging to that same producer.

The application coordinates sending threads that share a producer endpoint. Queue order follows the producer's serialized sends. A Source Queue Set's selection policy determines how its member queues interleave.

Tasks, queues, queue sets, memberships, message definitions, fields, and pools have explicit capacity limits. Build settings and configuration provide these bounds. Milestone 1 specifies how settings bind and how attaching processes validate the effective limits. Registration within reserved capacity can support runtime topology changes if the lifecycle contract permits them.

### Configuration format

Configuration files use TOML, implemented with [cpptoml](https://github.com/skystrife/cpptoml). Converters provide YAML, JSON, and XML configuration support by emitting TOML against Angaria's configuration schema. Converted configurations use the same loader and validation path.

Milestone 1 specifies the schema and supported syntax for the selected cpptoml version. Milestone 2 integrates loading and validation, and milestone 10 supplies converter tooling.

### Delivery and acceptance policies

| Policy | Agreed default | Configurable alternative |
| --- | --- | --- |
| Per-queue delivery | Lagging consumers recover unacknowledged deliveries from the retained log in queue order. | Allow delivery loss by skipping overwritten deliveries and reporting the skipped sequence range. |
| Per-Publishing Queue Set acceptance | Every destination accepts the publication. Rejection changes neither the queues nor the log. | Allow partial acceptance and return accepted destinations, rejected destinations, and rejection reasons. |

Recovery requires recorded per-queue publication history and acknowledgment progress. Message payloads and schema versions remain immutable, and delivery progress is tracked independently for every destination.

Publishing records have a persistent identity and a snapshot of their destination membership. Recovery can determine which deliveries were committed, and retries can target rejected destinations without republishing to destinations that already accepted the publication.

Acceptance establishes delivery obligations. Consumers acknowledge independently. A destination configured to allow delivery loss can subsequently skip overwritten deliveries. Committed payloads remain available for inspection under the log's retention rules.

The default recovery policy retains records needed for unacknowledged deliveries. Exhausting the configured log capacity returns a log-full result. Process-crash recovery is part of the planned implementation. Power-loss durability requires a separate contract.

## 1 Core architecture and policy contracts

Completed through [PR #7](https://github.com/smangano/angaria/pull/7). The [milestone 1 review plan and core contracts](docs/milestones/milestone-1.md) divided this work into five reviewed step PRs targeting `milestone-1`.

- Define the public C++ model for Tasks, queues, both Queue Set types, registration, ownership, attachment, acknowledgment, and failure results.
- Specify policy interfaces for storage mapping, source selection, consumer synchronization, queue delivery, and publishing acceptance.
- Define mapped layouts, offset-based references, ID widths, timestamps, immutable schema identities, and file-format compatibility. Reconcile prototype representations with the wiki, including its 32-bit field type identifiers.
- Resolve startup versus runtime registration and membership changes, policy binding and mutability, effective capacity validation, and acknowledgment scope. Specify the TOML configuration schema, cpptoml syntax compatibility, validation outcomes, and converter contract.
- Design shared payload allocation, publication commits, abandoned-write recovery, and the scope of lock-free guarantees. Evaluate these protocols against single-producer queues and all-destination acceptance before fixing the storage layout.

Completion requires a reviewed public API example for a bounded multi-process application and a concrete mapped layout and concurrency plan. Raise tradeoffs if the agreed guarantees conflict during protocol validation.

## 2 Shared persistent Registry

Complete. The [milestone 2 review plan](docs/milestones/milestone-2.md) records eleven reviewed step PRs, from the format-2 foundation through validated TOML configuration, consistent historical snapshots, and the [independent-process Registry walkthrough](docs/registry-walkthrough.md). Framework version `0.2.14` integrates this completed milestone into `main`.

- Implement the live Registry in shared mapped memory and place its persisted representation at the start of message logs.
- Implement registration and lookup for Tasks, Queues, Source Queue Sets, Publishing Queue Sets, Messages, schema versions, and Fields.
- Load TOML configuration through cpptoml and validate capacities, topology, and policy settings before applying Registry updates.
- Record global metadata, application and Angaria versions, creator identities, names, descriptions, ownership, membership, priorities, capacities, and effective policies.
- Track Task registration, restart/incarnation information, and topology lifecycle history. Generalize the prototype's role ownership to registered Tasks and Queue Sets.
- Implement consistent IDs and name indexes, concurrent registration, capacity errors, and recovery from interrupted updates.
- Preserve the historical definitions needed to interpret captured messages after schema, Task, or topology changes.

Completion is demonstrated by independent processes registering and discovering every entity type, recovering interrupted registration and topology updates, and inspecting current and historical Registry state after application shutdown. The full qualification suite and walkthroughs pass on Linux and macOS for both x86_64 and ARM64.

## 3 General messages and basic inspection

Complete. Framework version `0.3.7` integrates the six reviewed steps recorded in the [milestone 3 review plan](docs/milestones/milestone-3.md) into `main`. Steps 3.1–3.3 implement scalar/logical payloads, TOML version 2, and format 2.1. Step 3.4 adds [canonical message records](docs/message-record.md), with exact Session/schema context and bounded portable headers. Step 3.5 adds [capture streams and generic inspection](docs/capture-inspection.md), retaining exact Registry definitions and independent record/catalog/text bounds. Step 3.6, version `0.3.6`, [qualifies process exchange](docs/process-exchange.md), producer replacement, retained schema versions, and inspection after application exit.

- Implement generic headers and ordered fields driven by registered message definitions.
- Cover sequence numbers, message/schema identity, payload type and size, producer identity, timestamps, and user flags. Define dequeue and acknowledgment metadata per destination in the receipt contract; native execution follows the Queue milestones.
- Implement explicit binary encoding for booleans, basic bit fields, signed and unsigned integers, floating-point values, fixed character strings, and enumerations.
- Keep immutable schema versions addressable by recorded messages.
- Extend the inspector to decode multiple application-defined types using the Registry.

Completion requires applications to exchange several registered message types and an offline inspector to decode them without application-specific code. Older records retain their original meaning after new versions are registered.

## 4 Two plane mapped storage

Step 4.1, framework version `0.4.1`, reviewed the [native transport contract](docs/native-transport.md). Step 4.2, version `0.4.2`, implements checked private Publication/destination/receipt/ring codecs and independent byte fixtures. Step 4.3, version `0.4.3`, adds [core delivery-history validation](docs/native-validation.md) and capability rejection before recovery. The [eleven-step plan](docs/milestones/milestone-4.md) orders codecs, history validation, mapped storage, and cleanup before public send/receive. Steps 4.6–4.7 implement public send/status, receive, acknowledgment/release, and Source close. Step 4.8, version `0.4.10`, enables configured allowed loss and protected in-flight delivery, with a sixteenth public-API primer lesson. New factories select format 2.2/`0xf`; readers retain 2.0/2.1 and core 2.2/`0x7` support without in-place upgrade. Fresh Source selection initially supports one Active Queue; multiple-Queue scheduling follows milestone 5 through the same APIs.

Step 4.9, version `0.4.11`, adds [combined failure and exhaustion qualification](docs/native-qualification.md): overlapping producer/consumer failures, repeated recovery, full-log mixed history, final counter values, unchanged rejection, and actual OS file-growth failure. Step 4.10, version `0.4.12`, adds the [native multi-process primer application](docs/native-pipeline.md), connecting Feed, Strategy, and Audit through public mapped Queues, exact schemas, retained backlog, acknowledgment/redelivery, and replacement. Performance measurements and the completion audit remain step 4.11.

- Implement the fixed-size rings of message offsets and the append-only memory-mapped payload plane.
- Support single-producer queue ownership and allocation of shared payload storage.
- Implement publication and commit recovery, ring wraparound, log-capacity checks, and rejection without state changes.
- Implement recovery from the log for lagging consumers by default, plus configured delivery loss with gap reporting.
- Make recorded publication and acknowledgment progress sufficient to reconstruct delivery after ring overwrite or endpoint replacement.

Completion requires contention, wraparound, resource-exhaustion, partial-write, and producer-termination tests, plus an audit of the [stated progress guarantees](docs/concurrency.md#supported-failure-and-progress-scope). The initial protocol serializes mutation; measure allocation/commit contention alongside latency and throughput for subsequent policy comparisons.

## 5 Source Queue Sets

- Implement receive operations through consumer-owned Source Queue Sets with per-queue priorities.
- Implement strict priority with round-robin selection among equal priorities.
- Implement proportional-budget fairness with specified budget units and reset rules.
- Implement weighted-random selection with defined weight semantics and reproducible tests.
- Integrate acknowledgment, configured delivery-loss behavior, Task ownership, and consumer replacement.

Completion requires multiple producer queues feeding a consumer, with verified per-queue ordering, selection behavior, delivery progress, and restart recovery under each selection policy.

## 6 Publishing Queue Sets

- Publish one immutable message to the destination queues referenced by a producer-owned set.
- Implement all-destination acceptance by default and configurable partial acceptance.
- Record publication identities, destination snapshots, per-destination acceptance results, and independent delivery progress.
- Implement recovery around publication commits and retry handling for rejected destinations.
- Compose publishing sets with the destinations' consumer-owned Source Queue Sets and configured delivery policies.

Completion requires tests for a destination that cannot accept, rejection without queue or log changes, partial acceptance and retry, independent acknowledgment, and publisher termination around commit and publication. The [five-process trading simulation](#demonstration-application) must demonstrate these behaviors using the Registry and both Queue Set types.

## 7 Consumer synchronization policies

- Implement spinning on Source Queue Sets.
- Implement Linux eventfd waiting, including notification-resource discovery and sharing between independently launched processes.
- Handle notification lifetime and generation changes during endpoint attachment and replacement.
- Define and implement the checks needed around sleeping, publication, notification coalescing, and recovery from missed notifications.
- Record the supported platform and policy combinations.

Completion requires the same Queue Set scenarios to work with both synchronization policies, including publication while a consumer transitions to sleep and endpoint replacement. Measure idle CPU usage and wakeup latency.

## 8 Segmented mapping

- Implement growing queue storage with configurable segment-allocation sizes.
- Implement segment discovery, mapping, stable references, and attachment to existing segments.
- Define allocation locking, capacity and filesystem-exhaustion results, and recovery from interrupted segment creation.
- Preserve the message, acknowledgment, Source Queue Set, and Publishing Queue Set contracts across storage mappings.
- Expose segment configuration and storage usage through the Registry and tools.

Completion requires the Queue Set acceptance scenarios to pass through growth, resource exhaustion, and restart with both mapping policies. Measure allocation pauses, memory consumption, and throughput.

## 9 Complete fields pools and serialization

- Complete bit fields through 256 bits, date/time/timestamp representations, duration units, sequence numbers, and GUIDs.
- Implement fixed homogeneous arrays, heterogeneous tuples, and repeating groups.
- Implement symbols with the wiki's 64-character limit and a configurable UTF-8 string pool, including reference lifetime, retention, and exhaustion behavior.
- Complete C++ field operations and optional minimum/maximum, enumeration, regex, and units metadata.
- Implement CSV, JSON, XML, and text representations, including defined representations for compound values.

Completion requires round-trip encoding, schema-driven inspection, pool-reference lifetime checks, and validation of supported constraints across every field family, including nested structures.

## 10 Tools and release qualification

- Complete Registry, topology, Task lifecycle, queue, delivery, and message inspection.
- Implement explicit replay with defined ordering, destination, and identity semantics.
- Provide YAML, JSON, and XML configuration converters that emit schema-valid TOML, including tests for equivalent configurations and explicit errors for unsupported or lossy mappings.
- Complete TOML configuration tooling and programmer, operator, best-practice, and performance documentation.
- Publish an application example with bounded cooperating processes, both Queue Set types, and selectable policies.
- Qualify supported policy combinations under concurrency, process termination, and resource exhaustion on x86-64 and ARM64.
- Publish latency, throughput, CPU, memory, and storage measurements with the tested platform and configuration.

Completion requires a reproducible application walkthrough, documented guarantees and configuration limits, and a release with the supported policy/platform matrix and performance results.

## Existing prototype

The completed v0 demo provides a CMake build, Linux CI, a bounded filesystem queue, one recorded integer schema, role locks, explicit acknowledgment, and process-restart tests. Its file format and runtime describe the current prototype.

The build infrastructure, encoding patterns, inspector fixtures, ownership mechanisms, and failure tests provide reusable foundations for the milestones above. The shared Registry, mapped queue storage, general message API, Queue Sets, and policy implementations are planned work.

The [README](README.md) documents the runnable prototype, and the [session format](docs/session-format.md) describes its existing files. The [restart walkthrough](test/restart_demo.py) exercises its process and acknowledgment behavior.

[Original design wiki](https://github.com/smangano/angaria/wiki/Angaria)
