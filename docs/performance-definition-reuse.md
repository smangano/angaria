# Reusing definitions within one operation

Milestone 4 step **4.20**, framework **0.4.22**, continues [direct delivery lookup](performance-delivery-lookup.md). It removes repeated loading of Task definitions and retained Queue/Source definitions during one gate-held operation. Public APIs, stored formats, native protocols, headers and payload lifetimes are unchanged. Integration into `main` remains held for measured improvements and qualification.

## What changes

A **definition** records an entity's identity, configuration and registration metadata. Queue and Source definitions can have retained revisions; delivery validation must use the definition effective when each Publication was accepted, rather than substituting today's definition.

`runtime_topology` already decodes and validates complete Queue/Source definition histories. Previously `runtime_validate_delivery` decoded those records again into a second owning history. It now borrows the complete checked histories during that same call. The shared private `RetainedTopologyDefinition<T>` type in [topology_state.h](../src/topology_state.h) holds each owning definition and its record reference. Delivery's per-identity indexes hold pointers into those owners. Its historical lower-bound lookup, registration-revision checks and Publication/receipt validation remain.

Schema loading also uses Task definitions to check that Field and Schema creators existed at registration. Previously the Field and Schema paths each loaded Tasks. The generic schema loader now loads Tasks once and shares them through both paths. Delivery validation reuses the complete Task catalog already checked by topology. Private overloads accept checked Task spans; standalone generic Field/schema loaders still load and validate fresh Tasks as needed. The Fields-only schema overload retains a fresh Task load because its caller supplies no Task catalog.

No definitions or pointers are retained between operations. Repeated Field/Schema preparation in later message construction and payload-codec creation remains outside this focused change.

## Risk assessment

Under the [performance/correctness guidance](../AGENTS.md#performance-and-correctness), decoding the same immutable definitions twice under an unchanged mutation guard adds cost without new evidence. The reuse interval contains no Session mutation. Complete historical topology checks finish before delivery validation receives its spans; the Task catalog comes from `runtime_tasks(access)` under that same live access and continuously held guard.

Trusted Task inputs must be complete and unmodified, from that access, without intervening Session mutation. Arbitrary, stale or cross-access catalogs violate this private precondition. The history spans have the same requirement and are supplied only by `runtime_topology`. Their owners remain alive and unchanged until validation returns; no pointer escapes into a public object, payload borrow or later operation.

All original Field/Schema decoding, policy, index completeness, uniqueness, creator attribution, registration revision and version-chain checks remain. Historical Queue/Source selection keeps its exact record-order and revision rules. Delivery still freshly reads mutable Task, Source, Queue, receipt and ring state. No checks are cached across recovery, mutation, claim replacement, another process's commit or operation boundaries. Generic entry points remain available for deliberate corruption fixtures and validate freshly.

This removes repeated checks within a short unchanged context. Unsupported raw writes that bypass the gate and arbitrary memory corruption during that interval remain outside the coordination protocol. No detection frequency changes across public operations. Full-prefix and complete delivery-history validation remain history-dependent costs; this is not an incremental-validation design.

## Qualification and measurements

Private read-observer tests distinguish logical immutable-copy requests from physical reads. They exercise retained topology history with Publications, Task/Field/Schema attribution, generic versus trusted loader equivalence, and fresh corruption rejection without writes. Existing historical-transition, corrupt-index/creator/schema, ownership, receipt, allowed-loss, recovery and exhaustion tests remain relevant. The primer continues to use the same public API.

The unchanged diagnostic workload compares preserved 0.4.21 executables with normal and enabled 0.4.22 builds. Normal builds supply timing comparisons; enabled controls explain Task slots, allocations and native work. Fresh Sessions, history/capacity/payload/Field controls, serial/backlog/thread/process workloads, raw outcomes and source/executable hashes are retained. Adjacent alternating pairs complement sequential full matrices. Plots show observed ranges, not confidence intervals or production-tail guarantees. CI has no performance thresholds. Generic Task loading now happens in a wrapper outside the SchemaLookup diagnostic scope. That phase's inclusive boundary therefore differs from the preceding build; whole-operation timing and work counters supply the comparisons. No claim depends on treating that nested phase as an unchanged region.

The [result directory](../benchmarks/results/milestone-4-definition-reuse/README.md) records measurements and plots. Further definition reuse during message preparation, remaining history searches, allocation/copy costs, file checks/growth and idle polling remain candidates. The full scan and Session-wide mutation gate still limit scaling.
