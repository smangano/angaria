# Plan for a controlled libnanomsg comparison

Milestone 4 step **4.27**, framework **0.4.29**, records a benchmark-first execution plan. This document adds no runtime policy, benchmark adapter, dependency or CI job. Implementation and measurements belong to follow-up review steps. The existing [native measurements](native-performance.md) and [detailed diagnostics](performance-diagnostics.md) measure Angaria; they are not an existing side-by-side libnanomsg comparison.

## Questions and contract boundaries

The comparison should establish ordinary API cost, delivery latency, completed work, allocation/native work and history sensitivity for explicitly named profiles. It must distinguish message acceptance, receipt by an application, processing completion and recoverable recording.

| Profile | Acceptance and completion boundary | Status |
| --- | --- | --- |
| Current Angaria acknowledged delivery | Direct publication commits under the mutation gate; the consumer verifies a Delivery and acknowledges it. Retained history and receipts support the documented process-recovery contract. No power-loss durability is implied. | Implemented library behavior; comparison adapter planned. |
| Classic libnanomsg IPC, unacknowledged | A successful PUSH send hands the message to the messaging implementation. Consumers verify received bytes. There is no application processing acknowledgment, durable replay ledger or atomic multi-destination publication supplied by this profile. | Library behavior available; comparison adapter planned. |
| Optional recorded libnanomsg adapter | A separately specified recording layer would define acceptance, destination intent, restart replay and incomplete fan-out outcomes. Adding a file write around send is not sufficient qualification. | Deferred until the contract and fault tests below exist. |
| Future Angaria recorded/unacknowledged delivery | Requires its own accepted-record, consumption/lease/reclamation and replay contract before it can enter a comparison. | Design work, not an implemented benchmark mode. |

The first two rows are intentionally different service contracts. Report their costs and observed outcomes side by side with that difference visible. Do not describe their timing ratio as the price of acknowledgment alone: serialization, recording, validation, ownership, buffering and transport implementation also differ. The [concurrency](concurrency.md) and [native receive](native-receive.md) contracts define the current Angaria boundary. The separate [recorded unacknowledged delivery plan](recorded-unacknowledged-delivery-plan.md) specifies the future profile's technical design and qualification questions.

Use classic **libnanomsg**, not an NNG compatibility wrapper or ZeroMQ under the same column label. The unversioned upstream manual supplies the general pattern overview; implementation and option references below are pinned to official upstream **1.1.5** sources. PIPELINE consists of PUSH/PULL sockets and load-balances outgoing work; it is not broadcast fan-out. The implementation selects one available pipe and returns `EAGAIN` when none is available. `NN_DONTWAIT` returns that condition immediately; blocking sends wait subject to `NN_SNDTIMEO`, whose default is negative/infinite. These references establish the baseline semantics, not a claim that 1.1.5 is the latest release. Before execution, freeze and record the selected release's exact commit and source-archive checksum. [Unversioned PIPELINE overview](https://raw.githubusercontent.com/nanomsg/nanomsg/master/doc/nn_pipeline.adoc), [1.1.5 load balancer](https://raw.githubusercontent.com/nanomsg/nanomsg/1.1.5/src/protocols/utils/lb.c), [1.1.5 send](https://raw.githubusercontent.com/nanomsg/nanomsg/1.1.5/doc/nn_send.adoc), [1.1.5 socket options](https://raw.githubusercontent.com/nanomsg/nanomsg/1.1.5/doc/nn_setsockopt.adoc)

## Topology and implementation phases

Let **N** be the producer count, **C** the consumer count, and **F(p)** the fixed, ordered consumer set required for producer p's logical publications. A **logical publication** is a benchmark identity `(run, producer, sequence)` plus its complete required destination set. It is separate from a library's own publication identity or individual send call.

| Phase | Shape | Angaria boundary and treatment |
| --- | --- | --- |
| 1: core comparison | One producer, one consumer, one connection/Queue, IPC on one host. | Implement first using direct Queue send and a Source with one Active Queue. Qualify correctness before recording performance. |
| 2: explicit application fan-out | One producer, C=2/4/8 consumers; each logical publication is sent to every member of F(p). | Plan separate direct sends to separately owned consumer Sources/Queues. This is an application adapter with partial-send outcomes, not atomic Publishing Queue Set fan-out. Each Angaria direct send has its own library publication identity, correlated by the common benchmark identity. Qualify before running. |
| 3: producer scaling and Source fan-in | N=2/4/8 producers, C consumers, with a declared fixed edge set and bounded aggregate resources. | A consumer Source merging multiple Active Queues requires milestone 5. Mark such current-library cells `not_run_unsupported`; do not simulate the feature and label it native Source fan-in. |
| 4: native Publishing comparison | One logical acceptance covering a fixed destination set. | Requires milestone 6's implemented Publishing/fan-out contract. Repeated direct sends are not a substitute for atomic acceptance. |

For libnanomsg fixed fan-out, use one **1:1 PUSH/PULL socket pair for each producer-consumer edge**. Sharing one PUSH socket among several PULL workers would distribute messages between them instead of delivering every logical publication to all required consumers. A later work-distribution experiment may use that load-balanced shape, with a separate name, denominator and expected-recipient rule. Do not combine it with broadcast/fixed-fan-out results.

For N>1, preserve separate per-producer sequences. Record per-consumer delivery order and violations of the declared ordering contract; do not require a total order across independent producers unless a future adapter explicitly provides it. An application-level polling merge of multiple single-Queue Sources would be another adapter experiment, with its polling policy and cost included, not the planned native Source fan-in result.

IPC is the primary transport: independent local processes, POSIX UNIX-domain sockets for libnanomsg, and Angaria's native mapped Session on the same filesystem/host. Record filesystem and IPC path placement. TCP loopback may be a later separately labeled libnanomsg transport control; it does not become an Angaria network comparison. Cross-host tests need another clock/transport plan. [1.1.5 IPC documentation](https://raw.githubusercontent.com/nanomsg/nanomsg/1.1.5/doc/nn_ipc.adoc)

## Common bytes and application work

Use application payload sizes **64, 256 and 1,024 bytes**, including the following 32-byte benchmark envelope. Both adapters use the same wire layout, payload size, deterministic body and equivalent identity/timing field meanings. Actual timestamps vary between framework runs and must be checked against that run's producer observations. Library framing, schemas, stored headers and receipts are separate overhead and must be reported separately.

| Byte offset | Bytes | Little-endian field |
| ---: | ---: | --- |
| 0 | 2 | Benchmark wire version. |
| 2 | 2 | Nonzero producer ID. |
| 4 | 4 | Flags, with reserved bits zero. |
| 8 | 8 | Producer-local logical publication sequence. |
| 16 | 8 | Scheduled release time in the run's declared monotonic clock domain. |
| 24 | 8 | Logical send/fan-out start time in that same clock domain. |

The first fields occupy 8 bytes, followed by three 8-byte fields: **32 bytes total**. The remaining payload contains a deterministic checked pattern. Run identity, destination membership, seed and clock identity belong to the run manifest and fresh endpoints; freeze the exact wire version and flag meanings before harness implementation. Register an Angaria schema that represents these bytes exactly and verify both adapters against independent expected bytes.

Sample the logical send start once at the beginning of the producer's logical send/fan-out operation, before filling that timestamp and sending the first copy. Retain the value in every copy of the logical publication. Delivery/all-required-consumer latency then includes producer preparation, sequential fan-out and earlier-edge backpressure. Record each edge's actual API call start/return separately for API-cost measurements, including in the 1:1 case. Declare and match the destination-send order, or its deterministic balancing rule, across adapters.

An optional **8-byte API-only** case may carry a checked 64-bit sequence tag on a known edge. It has no room for this envelope and must not publish scheduled-release or one-way delivery latency. Keep its results separate from the 64/256/1,024-byte end-to-end cells.

The initial consumer work is payload/identity verification only. In Angaria, record receipt and verification boundaries before acknowledgment, then acknowledge normally. In libnanomsg, do not add a per-message return acknowledgment to manufacture the same cycle. Consumer summaries and end-of-run barriers may report observed completion afterward; they must not pace individual sends. Any later fixed CPU-work stage must be identical, separately named and included in the stated completion boundary.

Start with ordinary copying send APIs and preallocated application buffers on both sides. Any `NN_MSG` ownership-transfer or other allocation/copy variant is a separately labeled experiment with its ownership rules and costs recorded. Do not silently combine different buffer-preparation work in one comparison.

## Barriers, clocks and latency boundaries

Every run uses a fresh Session/endpoints. After connection establishment, registration and declared warmup/preload, all participants announce **READY** over a control channel. The coordinator verifies expected participants and drained queues, then releases a **START** barrier. Where the shared clock domain below is validated, START supplies a common future **T0**, with a predeclared lead interval, and participants begin data work at T0. Each participant reports **DONE** after its data phase; a second barrier keeps status checks, cleanup, serialization and aggregation outside the measured phase. A missing participant, premature exit or failed barrier makes the run incomplete. Use bounded coordinator waits and retain participant diagnostics.

One-way timestamps require a **validated shared clock domain**. For local IPC, choose and record an explicit OS monotonic clock ID and epoch shared by the processes on the same host/boot; do not assume unrelated `std::chrono::steady_clock` implementations share an epoch. Record clock resolution, timestamp overhead and a startup cross-process consistency check. If the common domain cannot be established, record API durations and aggregate throughput only; mark one-way latency unavailable. Do not substitute wall-clock timestamps or add a network acknowledgment to estimate it.

Measure and retain START dispatch/receipt times, each participant's actual data-start minus T0, first-send and last-verified-delivery times, and coordinator DONE receipt times. Report barrier-release/start jitter and first-send-to-last-verified intervals separately from T0-to-completion and control-channel intervals. With 16 messages, scheduling and barrier costs can dominate libnanomsg's apparent rate. These are **bounded-cohort throughput observations**, not steady-state transport capacity; larger cohorts and offered-load runs are required before making sustained-throughput claims. Without a validated shared clock, coordinator barrier-to-DONE throughput includes control/scheduling overhead and must be labeled accordingly.

Record these distinct intervals:

- **Send API cost:** each edge's actual send return minus that edge's actual call start, including any blocking/backpressure in that call. Angaria ID issuance/preparation cost is reported separately and also included in an explicitly named complete producer path.
- **Scheduled acceptance latency:** each successful edge send return minus the logical publication's predeclared scheduled release time. Also report the final acceptance boundary when every required edge succeeds; it is unavailable for partial fan-out. This includes late producer scheduling and previous backpressure.
- **Logical-send-start-to-delivery latency:** consumer receipt time minus the shared logical send/fan-out start in the payload envelope. The separate per-edge observations define API-call cost; do not relabel this whole-operation boundary as an individual send-call start.
- **Scheduled-to-delivery latency:** consumer receipt time minus scheduled release time. This includes producer delay and backpressure and is the primary offered-load latency.
- **Verification and acknowledgment cost:** record separately; only Angaria's acknowledged profile has the latter operation.

Do not use consumer receipt minus producer send-return as the primary one-way interval: a fast consumer can receive before the producer returns. Precompute an offered-load schedule and retain original scheduled timestamps when the producer falls behind. Rescheduling late work to the current time would hide queueing delay through **coordinated omission**, meaning the benchmark stops observing the delays caused by its own overload.

Use both a finite closed-loop cohort and, in a later phase, an explicit offered-rate sweep. Closed-loop behavior must name its outstanding-work limit. The offered-rate sweep must retain attempted, accepted, failed and pending counts at every rate, rather than reporting only the messages that happened to arrive.

## Completed work and fan-out denominators

Report **accepted logical publications**, **logical publications delivered to all required consumers**, **verified per-consumer deliveries**, and **acknowledged deliveries** as separate counts. A logical publication contributes to all-required-consumer throughput only when every member of its frozen F(p) has reported the expected identity and verified bytes. An average consumer rate or one fast consumer cannot stand in for that condition.

For fixed F, delivery volume is F times the logical publication volume only when all expected deliveries complete. Report logical publications/s, aggregate deliveries/s, and each consumer's deliveries/s separately. Show missing, duplicate and unexpected deliveries, sequence gaps, per-consumer lag and partial fan-out. Angaria's terminal acknowledgment throughput remains a distinct stronger boundary. Libnanomsg's observed verified delivery count is a benchmark observation, not a durable processing acknowledgment supplied by its protocol.

Define the primary common bounded-cohort denominator before execution: from the validated common T0 until all required consumers finish verification of the accepted cohort, or until the declared stop condition. Retain the first-send-to-last-verified interval, coordinator barrier interval, producer acceptance time and each consumer's phase time alongside it. If only the coordinator clock is usable, use its barrier-to-DONE interval and label the included control/scheduling overhead. A budget stop uses actual work and duration, with pending work visible; it is not a successful full-cohort run.

## History, buffers and finite run limits

Start with bounded **16-publication measured cohorts** at H=0/32/128, then increase cohort sizes only after confirming log and runtime budgets. H is the exact number of prior acknowledged/verified logical publications and, for fan-out, must also specify per-edge delivery counts. Drain all preload/warmup work before START. Warmup messages retained by Angaria count toward H: either include them in the declared preload or perform them in a separate discarded Session. A purported H=0 run cannot silently retain a warmup history.

Angaria retains Publication history after acknowledgment; base libnanomsg does not. Equal preload traffic therefore does not create equal retained storage. State that difference in every history comparison. A recorded libnanomsg profile must retain the same declared history/cohort and destination information before being used to investigate comparable recording/replay work.

Record every capacity and policy rather than equating unrelated numbers:

- Angaria ring slots bound its ring fast path. Under recovery-from-log policy, ring overwrite can fall back to retained history; the ring is not a hard bound on pending messages. Record ring capacity, log byte limit, topology reservations, actual history and pending receipts.
- Classic libnanomsg `NN_SNDBUF`/`NN_RCVBUF` are byte settings, with documented allowance for an additional message, and transport buffers can add storage. Record their actual values and effective socket/OS limits. They are not exact equivalents of Angaria ring slots.
- Use the same payload/cohort/offered rate and publish actual outstanding work. Any matched adapter-level credit window must be a separate experiment, with the same control mechanism and cost on both sides; it is not the unacknowledged baseline and does not add a persistence guarantee.

Before a run, declare maximum publications, elapsed budget, Session/log bytes, free-disk reserve and participant deadline. Stop bounded cohorts at the declared limit; record `LogFull`, timeout, send error or budget exhaustion with the accepted/completed/pending state. A byte cutoff may stop work between calls before exhausting the configured log, but a racing append can still reach the real limit and must be reported. The coordinator deadline is a containment mechanism; it does not turn blocking library calls into a hard real-time API.

Long quantile studies must not require an unlimited append log or an ever-growing Session. Repeated fresh fixed-history cohorts may accumulate enough samples within a declared population, but must be labeled as such, including their within-cohort history range. Do not present them as one continuous steady-state run. Record log growth before/after preload and measurement, final pending work, run duration and every early cutoff.

## Repetitions, tails and retained artifacts

Build both adapters with the same optimized compiler/toolchain settings and run under matched permissions, CPU placement, filesystem and host conditions. Freeze the source/library/executable hashes, selected commit IDs, build flags, dependency versions, transport options, clock identity and exact matrix. Keep profiling/tracing builds separate from ordinary timings. Libnanomsg remains an optional benchmark dependency; it must not become a core library requirement or an automatic performance gate.

Account for CPU and resources across **all participant and library background threads**, plus coordinator and optional recorder/replay helpers. Retain process/thread counts, total CPU seconds, per-participant CPU, affinity, peak memory and file/IPC resources within the declared phase. Separate coordinator-only overhead where possible without omitting it from the complete system total. Matching application process counts does not establish equal resource use when a library starts background workers.

Use predeclared **ABBA blocks** across independent fresh runs to limit one-way drift: Angaria, libnanomsg, libnanomsg, Angaria, with the starting variant balanced between blocks. An initial five blocks provide ten runs per framework/cell. Report medians of per-run statistics and complete observed run ranges; retain pair/block order and do not drop slower runs. These short cohorts are useful initial measurements, with descriptive p50 and actual sample counts, not strong tail evidence.

Publish p99 only for an adequately sampled, explicitly matched population; use at least **10,000 delivered samples per reported cell** as a minimum gate and report its rank and tail count. Publish p99.9 only after at least **1,000,000 delivered samples per reported cell**, along with repetitions, rank, upper-tail population, uncertainty and run/cohort composition. These minimums do not prove independence or a production bound. If log/time budgets prevent them, mark the quantile `insufficient_samples` and retain the short-cohort results. Never pool incompatible histories, payloads, consumers or failure states just to reach a threshold, and never extrapolate a p99.9 from a few hundred observations.

Retain per-run manifests, raw publication/consumer observations, scheduled and actual boundaries, all outcomes, logs/stderr, capacity/history measurements and machine metadata. Produce side-by-side plots for API costs, scheduled-to-delivery distributions, logical/all-consumer throughput, per-consumer deliveries, CPU/allocation/native work and log growth, with sample sizes and full variation visible. Record unsupported, incomplete and failed cells as those outcomes. Tail plots and replay results remain pending until their required experiments exist.

## Optional recording/replay profile: qualification before comparison

A logged libnanomsg adapter needs an explicit contract for the canonical record, all required destinations, the exact acceptance point, partially completed fan-out, duplicate suppression/replay behavior, producer identity and restart ownership. It must specify which bytes and progress remain after process termination and which consumer observations survive. Process-crash recovery and power-loss durability are different scopes; state the required flush policy rather than assuming that a successful write provides both.

Qualify termination before recording, after a complete record but before acceptance, after acceptance but before any send, between destination sends, after receive but before processing, during replay, and across repeated restarts. Verify exact missing/duplicate/partial sets and replay completeness using an independent identity ledger. A record appended before sending is not proof of atomic log-plus-fan-out; a successful PUSH send is not proof of consumer processing. Until that profile passes its declared guarantees, label it experimental and exclude it from claims of equivalent recovery service.

The execution order is therefore: review this plan; implement and qualify the optional **1:1 IPC comparison harness**; retain a bounded ordinary baseline with complete provenance; expand supported topology/load cells; qualify any recorded adapter independently; then perform sufficiently sampled tail and failure studies. A request-only performance workflow can be added with the harness. This planning step supplies no benchmark results and changes none of the existing public delivery guarantees.
