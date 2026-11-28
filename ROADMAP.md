# Angaria roadmap

<p align="center">
  <a href="assets/logos/README.md"><img src="assets/logos/A1_original_web.png" alt="Angaria — C++ messaging library" width="200"></a>
</p>

Angaria is a C++ inter-process messaging framework built around a shared persistent Registry, Source Queue Sets, Publishing Queue Sets, and selectable policies. The target application is a small, bounded network of cooperating processes, typically ten or fewer, with Tasks representing processes or threads.

The ten milestones below guide implementation. Milestone 1 is complete through [PR #7](https://github.com/smangano/angaria/pull/7), [milestone 2](docs/milestones/milestone-2.md) through its eleven reviewed steps, and [milestone 3](docs/milestones/milestone-3.md) through its six reviewed steps and integration release `0.3.7`. [Milestone 4](docs/milestones/milestone-4.md) now supplies producer-owned recording, independent point-to-point messaging, optional acknowledgments and replay through step 4.38, version `0.4.40`. Step 4.39, version `0.4.41`, records its [current completion review](docs/milestones/milestone-4-current-completion.md) and the [milestone-5 review plan](docs/milestones/milestone-5.md). Final qualification and integration into `main` remain pending. Milestone 6 delivers the [five-process trading simulation](#demonstration-application) using the Registry, both Queue Set types, and recoverable mapped messages. Milestones 7 through 10 complete the synchronization, storage, field, and tooling coverage.

## Living programmer primer

The [Angaria primer](docs/primer.md) teaches the implemented public API through small runnable programs, then builds toward multi-process applications. Keep its language accessible to junior programmers. Every milestone includes updating the primer and its [compiled examples](examples/primer/) for new public features, supported policies, and lifecycle rules; milestone completion reviews include that update. CI runs the examples so changes cannot silently leave the learning path broken. Planned topics remain clearly marked until their APIs work.

## Versioning

The framework uses `major.minor.build`, recorded in the root [VERSION](VERSION) file and read by CMake. During roadmap development, major is `0` and minor is the milestone number. Starting with milestone 3, build starts at `1` for each milestone and increases once for every new PR within that milestone. This includes fixes, documentation changes, and milestone integration PRs. Additional commits, review fixes, and CI reruns within one PR retain its assigned version.

Milestone 2 keeps its issued versions through integration release `0.2.14`. The first milestone-3 step is `0.3.1`; its next PR uses `0.3.2`. The first milestone-4 PR starts again at `0.4.1`. Assign the next unused build number within the current milestone when opening a PR and include it in the description. Check previously assigned versions, including closed PRs, to avoid reusing a version.

Tracking begins with the versioning PR at `0.2.1`; earlier PRs remain unversioned. Completion of all ten milestones produces release **`1.0.0`**, resetting the development counter. Framework versions are independent of configuration, mapped-format, message-schema, and synchronization protocol versions; those retain their own compatibility rules.

## Milestone 4 closeout and milestone 5 entry

The current implementation uses native format 3.0 and synchronization revision 2. The [public runtime contract](docs/public-runtime-admission.md) defines its executable profile. Earlier milestone-4 entries describe development history, including an engine that has been removed; their old allowed-loss and compatibility claims do not describe today's runtime.

| Review unit | Status and exit |
| --- | --- |
| 4.39: current completion review and roadmap | This documentation review, framework `0.4.41`, identifies implemented behavior, remaining qualification and the seven milestone-5 steps. It makes no new performance claim. |
| 4.40: final current-runtime qualification | Planned separate PR from staging. Measure the exact candidate's public API on Linux against nanomsg, with matched same-host baseline controls, both acknowledgment modes, payload/load/history/credit cases, longer bounded streams and slow/full consumers. Preserve the existing ID/send/status/receive workload and label an additional ordinary successful-send workload separately. |
| Focused fixes, if qualification requires them | Give each significant repeatable regression or unexplained implementation stall its own reviewed step and appropriate performance evidence. Record known policy tradeoffs and remaining limits. |
| Milestone-4 integration | After the [completion gate](docs/milestones/milestone-4-current-completion.md#integration-gate) is satisfied, open the separate `milestone-4` → `main` PR and qualify its exact final commit. Create `milestone-5` from that integrated `main`, then `milestone-5-1` for its contract review. |

Linux latency and verified completed-message throughput drive optimization acceptance. macOS remains supported with full functional qualification and measured regression coverage; a significant repeatable slowdown needs review, while modest documented tradeoffs can be acceptable. Results remain workload-specific: no universal nanomsg win or noisy hosted timing threshold is required. The current final-inline implementation has paired 64-byte confirmation; the broader earlier full matrices do not substitute for the planned final-candidate qualification.

The performance backlog follows its owning feature: input selection and shared-stream scanning in milestone 5, waiting/notification CPU cost in milestone 7, and rotation/retention/reclamation in milestone 8. Recheck larger-payload cost during final qualification before selecting a focused optimization. Keep reports, plots, summaries and provenance in Git, and retain raw attempts through the [external archive policy](benchmarks/results/README.md).

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
| 5 | Exercise multiple inputs, Source selection policies and configured loss/gap behavior under the current recording protocol. |
| 6 | Deliver the complete five-process application and the acceptance scenarios above. |
| 7–10 | Extend the same application across waiting/storage policies, then add release walkthroughs and measurements. |

## Agreed architecture

### Registry and bounded topology

The Registry is the shared source of definitions, identities, ownership, topology, and lifecycle metadata. It is persisted with message logs so tools can interpret captured sessions using their historical definitions.

Each queue has one registered producer Task and belongs to one consumer-owned Source Queue Set. A producer arranges a destination queue in its consumer's Source Queue Set. A Publishing Queue Set references destination queues owned by its producer. A queue can be referenced by several publishing sets belonging to that same producer.

The application coordinates sending threads that share a producer endpoint. Queue order follows the producer's serialized sends. A Source Queue Set's selection policy determines how its member queues interleave.

Tasks, queues, queue sets, memberships, message definitions, fields, and pools have explicit capacity limits. Build settings and configuration provide these bounds. Milestone 1 specifies how settings bind and how attaching processes validate the effective limits. The current admitted profile seals setup at the first valid endpoint opening or publication-ID issuance. Milestone 5 keeps that fixed topology. Runtime registration or membership changes require a separately reviewed lifecycle protocol; reserved capacity alone does not enable them.

### Configuration format

Configuration files use TOML, implemented with [cpptoml](https://github.com/skystrife/cpptoml). Converters provide YAML, JSON, and XML configuration support by emitting TOML against Angaria's configuration schema. Converted configurations use the same loader and validation path.

Milestone 1 specifies the schema and supported syntax for the selected cpptoml version. Milestone 2 integrates loading and validation, and milestone 10 supplies converter tooling.

### Delivery and acceptance policies

This table describes the agreed full-roadmap contract. Today's profile implements direct Queue delivery with `recover_from_log`, required acknowledgment by default and optional `none`. Configured loss returns in milestone 5; Publishing execution, all-destination acceptance and partial acceptance follow in milestone 6.

| Policy | Agreed default | Configurable alternative |
| --- | --- | --- |
| Per-queue delivery | Lagging consumers recover available deliveries from the retained log in Queue order. | Allow delivery loss by skipping eligible stale deliveries and reporting the skipped sequence range. |
| Per-Publishing Queue Set acceptance | Every destination accepts the publication. Rejection changes neither the queues nor the log. | Allow partial acceptance and return accepted destinations, rejected destinations, and rejection reasons. |

Recovery requires recorded per-Queue publication history and mode-specific consumer progress. Acknowledged mode additionally retains certified processing progress. Message payloads and schema versions remain immutable, and delivery progress is tracked independently for every destination.

Publishing records have a persistent identity and a snapshot of their destination membership. Recovery can determine which deliveries were committed, and retries can target rejected destinations without republishing to destinations that already accepted the publication.

Acceptance establishes delivery obligations. In acknowledged mode, consumers acknowledge independently; transport credit in either mode does not certify processing. A destination configured to allow delivery loss can subsequently skip eligible deliveries under its reviewed policy. Committed payloads remain available for inspection under the log's retention rules.

The current fixed-log profile retains all accepted records, including already acknowledged messages. Exhausting the configured log capacity returns `LogFull`; neither returned credit, processing acknowledgment nor future delivery loss alone authorizes erasing replay history. Native direct publication and delivery implement process-crash recovery; broader Publishing support follows milestone 6. Power-loss durability requires a separate contract.

### Recorded messaging performance workstream

The [recorded messaging performance plan](docs/recorded-unacknowledged-delivery-plan.md) has removed the Session gate and common Session transaction state from established point-to-point communication. Every accepted message remains in the replay log; processing acknowledgments become a separate choice on the same producer-owned recording architecture. The Session establishes ownership, schemas, topology and capacity during administration. Ordinary send, receive, acknowledgment/release, ID issuance and outcome lookup must operate independently of its administrative gate. Fixed fan-out uses a Publishing Queue Set and one producer-consumer Queue per destination, with incoming Queues collected by each consumer's Source Queue Set. Default all-destination acceptance and explicitly configured partial acceptance remain distinct contracts.

Step **4.28**, version **0.4.30**, implements the [optional reproducible 1:1 IPC comparison](docs/nanomsg-comparison.md). Step **4.29**, version **0.4.31**, supplies [stall diagnostics and the current-log SPSC experiment](https://github.com/smangano/angaria/blob/aade0f6a0da98f2439cacd44396b67efe1f31e75/docs/stall-study.md), including [retained measurements](https://github.com/smangano/angaria/blob/aade0f6a0da98f2439cacd44396b67efe1f31e75/benchmarks/results/milestone-4-stall-study/README.md). Those diagnostic and protocol steps led to public runtime replacement in step 4.37. The remaining priority is the bounded closeout qualification above. Continue the [nanomsg comparison](docs/nanomsg-comparison-plan.md), leading with end-to-end latency and verified delivered publications per second, then fan-out latency to the slowest required consumer when supported. Compare logging, acknowledgment, admission and failure guarantees explicitly.

The current-log experiment is evidence, not the final architecture: its producer still uses the Session gate and shared journal. The [step-1 protocol design](docs/independent-messaging-protocol.md) specifies producer-owned recording/replay, independent SPSC delivery and connection-scoped acknowledged progress. Producer/Queue order replaces the earlier plan's mandatory Session-wide commit order; stronger replay ordering uses a separately specified optional trace. Preserve accepted destination snapshots, bounded replay retention and borrowed-message lifetimes. Source fan-in, Publishing fan-out, notifications and reclamation remain their respective milestone 5/6/7/8 work. Before 1.0, backward compatibility is not required: replace the superseded runtime, require fresh Sessions after breaking changes, and omit old-format readers, parallel engines and migration work. Reject unsupported formats before access. Acknowledgment remains the default mode. The protocol review itself changed no runtime behavior; steps 4.31–4.37 implemented it.

Step **4.31**, framework **0.4.33**, implements workstream step 2's private [producer-log headers, references and Publication codecs](docs/producer-log-format.md), with independent byte fixtures and a focused allocation/codec-latency/throughput check. Step **4.32**, framework **0.4.34**, implements workstream step 3's [producer-owned mapped append, recovery and replay](docs/producer-log.md), including actual process-crash boundaries, exclusive ownership, accepted-ID retry and focused append/replay latency and throughput checks. It enabled no public format-3 Session at that step. Step 4.33 subsequently bound the log to recorded/unacknowledged SPSC delivery with logical credit.

Step **4.33**, framework **0.4.35**, implements workstream step 4's private [recorded/unacknowledged point-to-point endpoints](docs/recorded-queue.md), with logical credit, atomic descriptor hints, bounded incremental discovery and retained Message copies. Functional/process tests, deterministic hot-path guards and a full matched comparison against nanomsg qualify this layer.

Step **4.34**, framework **0.4.36**, qualifies workstream step 5's [restart, catch-up and connection independence](docs/recorded-queue.md#restart-and-connection-independence). Independent processes are terminated before and after acceptance and returned credit. Accepted-ID lookup, replay and owning copies survive endpoint exit; replacement can duplicate unacknowledged delivery without returning credit twice. A held Session administrative gate, full peer connection, paused append or paused affected-stream recovery must leave another established connection usable. Focused performance checks separate cold replacement scans from replay and subsequent live messaging. Stale setup and descriptor generations reject within the fixed-resource contract; online resource replacement remains unsupported. Optional acknowledged progress followed in step 4.35, before public admission in step 4.37.

Step **4.35**, framework **0.4.37**, implements workstream step 6's private [optional acknowledged progress](docs/recorded-acknowledged-progress.md). Consumer-owned selected operation images recover issuance, pins, acknowledgment, release and close without a Session transaction. Accepted message recording remains producer-owned, and transport credit is distinct from certified processing progress. Reserved receipts retain exact latest timestamps and tokens through replacement and full-log operation. Both processing modes undergo independent-connection qualification and matched nanomsg measurements with their different delivery guarantees labeled. End-to-end qualification and public admission followed in workstream steps 7 and 8.

Step **4.36**, framework **0.4.38**, adds [end-to-end qualification](docs/end-to-end-qualification.md) for both private delivery modes against nanomsg on each measured host. Live warmup, longer finite streams, balanced execution order, offered load, retained history, credit pressure, consumer delays and legacy controls expose latency, completed throughput and CPU cost. Binary samples and control transcripts independently reconstruct every published run; ordinary CI checks correctness without timing thresholds. Public runtime admission followed in workstream step 8.

Step **4.37**, framework **0.4.39**, implements workstream step 8's [public runtime admission](docs/public-runtime-admission.md). One producer-owned log is shared across a Task's writers and exact schemas; Process/Thread claims, frozen setup, TOML acknowledgment selection and the public Message/MessageView/Delivery interface are integrated. Established messaging acquires no Session administrative gate. Accepted Queue sequences and an acknowledged processing lower bound distinguish acceptance, credit and processing. Public `LogArchive` captures per-producer replay cuts and exports flat message captures. Native format 3.0/synchronization 2 replaces the old message engine and requires fresh Sessions. The [public performance qualification](docs/public-runtime-performance.md) includes deterministic hot-path guards and matched end-to-end latency/throughput comparisons with nanomsg. The primer now has eighteen runnable lessons. Earlier step reports below retain their historical behavior; this admission contract describes the current runtime.

Step **4.38**, framework **0.4.40**, [removes repeated messaging PID queries](docs/performance-fork-aware-ownership.md) through retained fork-aware process tokens and consolidated public checks. Native claims, Thread/parent validation, early rejection before inherited locks, replay and optional acknowledgments retain their contracts. Focused Linux symbol-interception tests require zero hot `getpid()` calls; same-host baseline/head latency and throughput comparisons plus full nanomsg qualification measure the result without timing thresholds.

Step **4.39**, framework **0.4.41**, updates the [current completion review](docs/milestones/milestone-4-current-completion.md), defines the remaining integration gate and supplies the [seven-step Source Queue Set plan](docs/milestones/milestone-5.md). Final qualification is planned as step 4.40; neither this planning review nor the historical full matrices claim it has passed.

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

The current runtime uses producer-owned immutable recording, per-connection logical credit and consumer-owned optional processing progress. Direct public send/receive, exact MessageHeader copies, process replacement and log replay are implemented. Established messaging takes no Session administrative gate. The [current completion review](docs/milestones/milestone-4-current-completion.md) is the integration authority; final current-candidate qualification and the separate PR into `main` are still pending.

- Keep the ring as a bounded delivery hint over the accepted producer log, with one registered producer per Queue.
- Reserve recording and progress resources at setup; reject exhausted capacity without a new accepted publication.
- Qualify publication, recovery, replay, wraparound, interrupted operations and producer/consumer replacement.
- Recover retained messages after ring reuse. Configured stale-message loss and visible gaps are explicitly scheduled in milestone 5.6 under the new protocol.
- Keep transport credit, processing acknowledgment, retained history and borrowed-message lifetime distinct.

Completion requires the supported-profile and [integration-gate evidence](docs/milestones/milestone-4-current-completion.md#integration-gate), including the current primer, failure/progress guarantees and final Linux-first performance review. Native format 3.0 rejects earlier development Sessions before recovery or mutation; there is no old-engine or migration path.

### Historical implementation sequence

Steps 4.1–4.29 built, qualified and optimized the earlier Session-gated engine. Step 4.8's allowed-loss support belonged to that removed engine. The [historical 0.4.13 audit](docs/milestones/milestone-4-completion.md) preserves its original findings; its format-2 compatibility table and completion claims are not current admission rules. Steps 4.30–4.38 subsequently established the independent producer-owned runtime described above. The [milestone review record](docs/milestones/milestone-4.md) retains each step's scope and evidence.

The step-4.11 baseline exposed substantial preparation cost. Step **4.12**, version **0.4.14**, adds [detailed public-operation diagnostics](docs/performance-diagnostics.md), history/capacity/schema controls, and concurrent/idle-polling workloads. That baseline held integration into `main` while diagnostics, focused fixes and runtime replacement were developed; today's remaining gate is the current completion review above. The [findings](docs/performance-diagnostics-findings.md) identify repeated history validation, native metadata/mapping work, and poor idle-polling progress. No runtime optimization is bundled into the diagnostic step. Step **4.13**, version **0.4.15**, begins [measured optimization](docs/performance-prefix-reuse.md) by reusing record information already validated within an operation. Step **4.14**, version **0.4.16**, reduces native calls in those scans through [bounded mapped windows](docs/performance-prefix-windows.md). Step **4.15**, version **0.4.17**, addresses loaded polling through [local gate handoff](docs/performance-gate-handoff.md). Step **4.16**, version **0.4.18**, extends [bounded mapping reuse to runtime metadata](docs/performance-runtime-windows.md). Step **4.17**, version **0.4.19**, [reuses history validated at operation entry](docs/performance-operation-validation.md) for the first gate-held execution attempt. Step **4.18**, version **0.4.20**, [removes duplicate receive delivery validation](docs/performance-receive-validation.md) while retaining live issuance checks. Step **4.19**, version **0.4.21**, [uses the validated oldest-pending link](docs/performance-delivery-lookup.md) to remove issuance's repeated metadata search. Step **4.20**, version **0.4.22**, [removes repeated definition loading](docs/performance-definition-reuse.md) inside the same unchanged gate-held operation. Step **4.21**, version **0.4.23**, [avoids reloading Fields for payload codec construction](docs/performance-payload-preparation.md). Step **4.22**, version **0.4.24**, [reduces temporary allocations in prefix validation](docs/performance-prefix-buffers.md). Step **4.23**, version **0.4.25**, [removes duplicate pre-lock resource checks](docs/performance-gate-resources.md), retaining rejection after acquisition and explicitly qualifying delayed resource errors under contention. Step **4.24**, version **0.4.26**, adds [on-demand instruction/cache/branch profiling](docs/performance-cachegrind.md) of the serial cycle to locate CPU work before selecting another measured optimization. Step **4.25**, version **0.4.27**, [specializes one-destination uniqueness validation](docs/performance-single-destination.md) while preserving complete row checks and qualifying the change with matched profiling and ordinary measurements. Step **4.26**, version **0.4.28**, [validates journal manifests and commands without owning copies](docs/performance-manifest-validation.md) during history scans.

## 5 Source Queue Sets

The [milestone-5 review plan](docs/milestones/milestone-5.md) divides this work into seven small PRs after milestone 4 reaches `main`:

1. Review multi-input ownership, scheduling, Source-wide pin and recovery contracts, including the precise budget and random-state rules.
2. Implement strict priority with round robin among equal priorities, bounded per-input state and the specialized one-input path.
3. Qualify multi-input acknowledgment, release, transport credit and replacement/recovery across independent inputs.
4. Implement proportional-budget fairness with the reviewed budget units and reset rules.
5. Implement weighted-random selection with defined weights, reproducible fixtures and explicit restart-state behavior.
6. Implement configured allowed loss, visible skipped ranges and protected in-flight messages on the current recording protocol. Loss does not erase accepted log records.
7. Add a public-API multi-input application, update the primer and complete Linux-first correctness/performance qualification.

Completion requires multiple producer Queues feeding one Source, per-Queue order, one Source-wide acknowledged pin, exact progress and replay, selection/fairness and replacement under each policy. Measure 1/2/4/8 inputs in both acknowledgment modes, with all-ready, mostly-idle and skewed inputs and shared versus distinct producer logs. Retain a matched 1:1 control and report per-input latency as well as aggregate completed throughput, CPU and recovery time. Each implementation PR runs its scope-appropriate performance check. Seeded random selection alone does not reproduce consumer execution order.

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

## 8 Segmented mapping and log retention

- Implement growing queue storage with configurable segment-allocation sizes.
- Implement segment discovery, mapping, stable references, and attachment to existing segments.
- Define owner-scoped allocation locking, capacity and filesystem-exhaustion results, and recovery from interrupted segment creation or rotation.
- Define log rotation, explicit retention and safe reclamation, including the replay availability promised after a retained range expires. Returning transport credit or acknowledging a message alone never authorizes deleting its history.
- Protect borrowed messages, captured replay cuts, outstanding processing obligations and every required destination while segments rotate or are reclaimed.
- Preserve the message, acknowledgment, Source Queue Set, and Publishing Queue Set contracts across storage mappings without introducing a Session gate into normal messaging.
- Expose segment configuration and storage usage through the Registry and tools.

Completion requires the Queue Set acceptance scenarios to pass through growth, rotation, retention boundaries, safe reclamation, resource exhaustion and restart with both mapping policies. Measure transition pauses, memory/storage use, latency and completed throughput. Until this work is implemented, current fixed producer logs return `LogFull`; finite-stream measurements do not establish indefinite operation.

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

## Earlier prototype

The v0 demo and Session-gated native message engine were superseded by public runtime admission in step 4.37. Their source, tests and measurements remain reproducible from their pinned commits. The current build contains one message engine and rejects older native formats before recovery or mutation. See the [README](README.md) for current runnable programs and [mapped format](docs/mapped-format.md) for current files.

[Original design wiki](https://github.com/smangano/angaria/wiki/Angaria)
