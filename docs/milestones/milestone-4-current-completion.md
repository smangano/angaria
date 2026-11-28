# Milestone 4 current-runtime completion review

Step **4.39**, framework **0.4.41**, reviews the current runtime and defines the remaining milestone-4 exit work. This is a documentation-only step: it changes no messaging code, stored format or measured performance result. The implemented runtime is the producer-owned protocol admitted in **0.4.39** and optimized in **0.4.40**, through merged [PR #71](https://github.com/smangano/angaria/pull/71).

**Status: final performance qualification and milestone integration are pending.** The next qualification step must measure the final candidate described below before a separate PR integrates `milestone-4` into `main`. Existing functional tests and measurements are evidence for that review; they do not mark the remaining work complete.

This review governs current milestone-4 exit decisions. The [0.4.13 completion audit](milestone-4-completion.md) preserves the superseded Session-gated engine's history. Its format compatibility, allowed-loss implementation and Session-wide mutation costs do not describe the current runtime. The [public runtime contract](../public-runtime-admission.md) remains the authority for implemented application behavior.

## Current scope and evidence

| Facility | Implemented behavior | Evidence |
| --- | --- | --- |
| Setup and ownership | The Registry records bounded Tasks, exact schemas and topology. Endpoint opening or ID issuance seals setup. Process/Thread Tasks and retained parent claims establish operational ownership. | [Public admission](../public-runtime-admission.md#setup-and-admission), [admission tests](../../test/test_public_runtime_admission.cpp), [storage tests](../../test/test_public_runtime_storage.cpp). |
| Recording and acceptance | Every accepted message is recorded once in its producer Task's log. Its writers share an issuer and log; acceptance, identical-ID retry and retained outcome lookup survive producer replacement. | [Producer log](../producer-log.md), [producer tests](../../test/test_producer_log.cpp), [public admission](../public-runtime-admission.md#sending-and-acceptance). |
| Ordinary messaging | Established ID issuance, send/status, receive, acknowledgment and release acquire no Session administrative gate. A Task serializes its own producer operations; independent owners keep their own progress. | [Independent protocol](../independent-messaging-protocol.md), [connection-independence tests](../../test/test_recorded_queue_independence.cpp), [public performance guards](../../test/test_public_runtime_performance.cpp). |
| Receiving | One Active input Queue per operational Source, with retained-log recovery and Spin waiting. Transport credit is returned independently of processing completion. | [Current profile](../public-runtime-admission.md#implemented-profile), [Queue tests](../../test/test_recorded_queue.cpp), [restart tests](../../test/test_recorded_queue_restart.cpp). |
| Processing progress | Acknowledgment is required by default. Explicit `none` provides recorded, unacknowledged delivery. Exact tokens, release/redelivery, close and replacement are consumer-owned. | [Acknowledged progress](../recorded-acknowledged-progress.md), [progress tests](../../test/test_recorded_progress.cpp), [acknowledged Queue tests](../../test/test_recorded_acknowledged_queue.cpp). |
| Message lifetime | `Message` owns its bytes; `MessageView` borrows them. `Delivery` combines a view with a processing token. Both message forms expose the same `MessageHeader`, preserved by copying. | [Public interface](../public-interface.md), [message tests](../../test/test_message.cpp), [primer](../primer.md). |
| Replay and inspection | Public `LogArchive` captures a committed cut per producer. Owning records retain exact decoding definitions and accepted destinations; flat capture export supports inspection with the matching catalog. | [Replay contract](../public-runtime-admission.md#replay-export-and-inspection), [lesson 18](../../examples/primer/18_log_replay.cpp). |
| Fork rejection | Retained process tokens replace repeated hot PID queries. Inherited handles reject before local locks or atomic Task retention, while Thread and parent ownership checks remain. | [Ownership optimization](../performance-fork-aware-ownership.md), [public tests](../../test/test_public_runtime_admission.cpp), [Linux PID guard](../../test/test_public_runtime_performance.cpp). |

Native Sessions use **format 3.0**, required-feature mask **0x7**, and synchronization revision **2**. Matching programs must create fresh development Sessions after breaking format changes. Unsupported versions reject before incompatible structures are interpreted or recovery is applied. Before 1.0, this project maintains one current engine rather than old-format readers, migration paths or a parallel compatibility runtime.

A replay cut is a fixed accepted-message boundary, not a global execution order. Queue filtering preserves accepted Queue order; deterministic producer enumeration does not recreate the order in which independent consumers invoked application code. Optional execution traces remain later tooling work.

## Explicit boundaries and reassigned work

The Registry can describe more policies than the current messaging runtime executes. Declared interfaces and historical tests do not establish current support.

| Boundary | Current state | Assigned work |
| --- | --- | --- |
| Multiple Source inputs and selection policies | Operational admission permits one input; multiple-input strict priority, equal-priority round robin, proportional budgets and weighted random remain unavailable. | [Milestone 5](milestone-5.md), with consumer-owned per-input state and one Source-wide pin (one in-flight delivery). |
| Configured stale-message loss | `allow_loss` rejects as unsupported. The retired engine's step 4.8 implementation was not carried into the new protocol. Unacknowledged delivery does not imply permitted delivery loss. | Milestone 5 adds explicit gap, receipt and protected-pin rules under the new protocol, before the milestone-6 demo. |
| Publishing Queue Sets | Registry topology exists; operational fan-out, all-destination acceptance, partial acceptance and rejected-destination retry remain unavailable. | Milestone 6, including the complete multi-process demonstration. |
| Waiting | Spin is the implemented policy. Slow/idle polling consumes CPU. | Milestone 7 adds waiting/notification policies and qualifies wakeup races and replacement. |
| Storage lifetime | Logs are preallocated and capacity-bounded. Returning credit or acknowledging processing reclaims neither recorded bytes nor accepted-record capacity. | Milestone 8 explicitly owns segmentation, retention, rotation and reclamation, with replay/borrow/acknowledgment lifetime rules. |
| Fields and shared pools | Current supported Binary schemas retain their documented limits. | Milestone 9 completes the remaining Field families, pools and serialization formats. |
| Release tooling | Current replay/export is a bounded public facility, not a complete catalog/progress bundle or exact execution trace. | Milestone 10 completes generalized inspection/replay, optional traces, converters and release qualification. |

The supported failure scope is process interruption on a running coherent host. There is no synchronous disk-flush or power-loss recovery promise. Queue credit, acknowledged processing and replay retention remain separate contracts. Finite benchmark completion does not establish indefinite operation or remove `LogFull`.

## Existing qualification and its limits

The 0.4.40 implementation and archival cleanup pass **752 local Release CTest entries**, with passing [Linux x86_64 CI](https://github.com/smangano/angaria/actions/runs/46150475919) and [Linux ARM64/macOS ARM64 CI](https://github.com/smangano/angaria/actions/runs/46150475861) on PR #71's final head `34cd8e0`. Those results belong to that implementation, not to a new final-qualification run for this documentation PR or a future integration candidate.

Current tests cover interrupted producer append/acceptance, consumer progress and repeated recovery, full-log behavior, unchanged rejection, stale tokens/claims, retained copies and replay. Held Session-gate, paused-peer, full-peer and affected-recovery cases qualify independence of established connections under their stated ownership boundaries. They do not promise immunity from host scheduling or shared hardware pressure.

The [retained ownership measurements](../../benchmarks/results/milestone-4-fork-ownership/README.md) preserve two separate populations:

- **621 full-workload attempts** across three hosts use the initial non-inlined token implementation at `9618efb`.
- Final inline confirmation at `19b76ad` repeats the **64-byte core workload**, with six balanced BASE/HEAD pairs per mode/host. It does not provide final-inline results for every larger-payload, slow-consumer or longer-stream case.

The initial and final Linux cohorts ran on different CPU/virtual-machine platforms. Each cohort has its own same-host baseline; differences between those jobs cannot isolate the effect of inlining. The report keeps every attempt, the initial hosted-Mac regression and later mixed Mac results. No universal nanomsg win or isolated acknowledgment-cost ratio follows from these measurements.

All accepted messages remain replayable in both Angaria modes. Nanomsg supplies neither Angaria's replay log nor its application-processing acknowledgment. Reports must keep those contracts visible when comparing completed throughput and latency.

## Final qualification plan

Linux latency and verified completed throughput drive performance decisions. macOS remains supported and must retain acceptable performance; a large repeatable Mac slowdown warrants investigation, while negligible or modest Mac differences can accompany substantial Linux gains. Neither platform's noisy hosted timings become hard CI thresholds.

The next focused step is a bounded qualification of the final public runtime, followed by a small measured fix only if the evidence identifies a blocker. It must publish the following evidence before integration:

1. **Freeze the candidate and comparison inputs.** Record exact source/binary hashes, framework version, compiler/flags, dependency pin, architecture, CPU, wait policy, capacities and case plan. Build both revisions before timing. Identify the baseline in the PR before collection; do not substitute an older host's rates for a new same-host control.
2. **Run the full public Linux workload matrix against nanomsg.** Preserve both acknowledgment modes, 64/256/1,024-byte payloads, offered load, prior history, credit variation, delayed/paused consumers and longer finite streams. The existing public plan has 207 attempts per host; changed plans must be labeled rather than silently presented as that full matrix. Pair baseline/candidate controls for the same core and selected non-core workloads when claiming a code improvement.
3. **Separate normal acceptance from status resolution.** Retain the historical ID/send/status/receive workload. Add an ordinary successful ID/send/receive workload without unconditional acceptance lookup, and retain explicit status resolution for uncertain outcomes. Label this as an additional workload, not a code speedup produced by removing benchmark work. Extend the collector only as needed to preserve and independently validate the new timing/completion boundary.
4. **Lead with application-visible outcomes.** Report completed messages/s, delivery p50/p99 and appropriately sampled longer-stream p99.9/maxima; report consumer-ACK latency separately. For offered-load cases retain scheduled latency and lateness, so catch-up throughput cannot hide a missed schedule. CPU, faults, retries and profiler counts explain those results rather than replacing them.
5. **Exercise limits and recovery.** Size longer runs within explicit log bounds and retain full/slow-consumer behavior. Re-run focused termination, replay and connection-independence checks for any runtime change. Keep cold attachment/recovery scans distinct from ordinary delivery cost. Do not infer an absence of stalls from a small p99 alone.
6. **Keep ordinary guards and Mac coverage.** Pass functional CI on Linux x86_64, Linux ARM64 and macOS ARM64, plus bounded Mac performance regression checks. macOS x86_64 remains request-only. Preserve deterministic guards against buffered hot-path allocation, Session gating, repeated history/catalog work, explicit file/mapping/growth work and direct Linux PID calls; retain their positive controls and stated scopes.
7. **Retain reconstructible evidence externally.** Keep all raw attempts, including failures and unfavorable results, in the verified maintainer archive. Publish reports, plots, compact summaries, provenance and checksums under the [archive policy](../../benchmarks/results/README.md). Restore raw inputs before strict reconstruction; a summary-only check is not equivalent verification.

Select any extra run length and control cells before collection and record their bounds. Quantitative acceptance ranges must reflect the actual workload and observed host variance; this review assigns no invented percentage threshold or hard-real-time guarantee.

## Exit decision and remaining tuning

An unexplained, significant and repeatable Linux regression or internal stall blocks integration until investigated and resolved or its explicit performance/guarantee tradeoff is reviewed. Replaying accepted messages, exact ownership, bounded rejection and optional acknowledged progress remain required. Record any accepted tradeoff with its measured cost, failure risk and operating assumptions. The decision is not conditional on outperforming nanomsg for every payload or policy combination.

Known limits become a ranked backlog rather than an unlimited extension of milestone 4:

| Candidate | Placement and qualification |
| --- | --- |
| Larger-message throughput | Recheck the final candidate, then profile copying, validation/encoding or other observed work before assigning a cause. Make a focused fix if the final evidence identifies a material blocker. |
| Multi-input scanning and selection | Milestone 5 measures 1/2/4/8 inputs, readiness/load skew and shared versus distinct producer logs, while retaining the one-input fast path. |
| Slow-consumer and idle-polling CPU | Primarily milestone 7's waiting policies; retain the current Spin tradeoff and investigate smaller changes when supported by measurements. |
| Credit-dependent queuing latency | Document throughput/latency sensitivity as a configuration tradeoff. Greater credit is not an unconditional improvement. |
| Page faults, writeback and finite-log lifetime | Keep tail/fault observations in current qualification; assign storage growth/rotation/reclamation to milestone 8. Do not claim a measured fault count proves a particular I/O or cache cause. |

## Integration gate

The separate qualification and integration PRs update these unchecked items with exact source IDs, results and links. This planning PR does not check them on the basis of older evidence.

- [ ] Final candidate completes the public Linux matrix, matched same-host controls and additional ordinary-send workload, with every attempted run accounted for.
- [ ] The report independently reconstructs raw measurements and distinguishes workload changes, code changes, host variation and acknowledged completion.
- [ ] Significant repeatable regressions/internal stalls are resolved or have an explicit reviewed tradeoff; accepted Mac and storage/wait limits are documented.
- [ ] Any runtime fix passes its focused performance check and the applicable recovery, replay, ownership and independence tests.
- [ ] Raw evidence is verified in external storage; Git retains the report, plots, provenance, catalog/checksums and restore instructions.
- [ ] The final supported/deferred policy table, current format/API documentation and roadmap agree, including milestone-5 allowed loss and milestone-8 retention/reclamation.
- [ ] The primer's eighteen runnable public-API lessons, source examples and commands are reviewed against the integration candidate; planned chapters remain clearly labeled.
- [ ] The separate `milestone-4` → `main` PR passes CI on its own final commit, with macOS x86_64 run only when requested.
- [ ] Integration receives its next unused `0.4.<build>` version when that PR is created; after merge, `milestone-5` starts from `main`, and its first reviewed step uses `0.5.1`.

The [milestone-5 plan](milestone-5.md) is the next feature sequence after those exit checks and integration. Its first step reviews multi-input scheduling/progress contracts; this completion review does not implement them.
