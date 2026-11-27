# Diagnosing native transport costs

Milestone 4 step **4.12**, framework **0.4.14**, investigates the poor costs recorded in the [step-4.11 baseline](native-performance.md#recorded-milestone-4-baseline). Integration of milestone 4 into `main` is held until the dominant costs are understood, measured improvements are reviewed, and performance targets for a representative workload are agreed. This step adds measurement tools and evidence; it does not change a transport algorithm or weaken validation.

The [recorded findings](performance-diagnostics-findings.md) explain the selected costs, controls, and next fix candidates. Step [4.13 prefix reuse](performance-prefix-reuse.md) records the first measured improvement; the retained index counter/phase names are zero where the separate runtime index pass has been removed. Step [4.14 mapped windows](performance-prefix-windows.md) reduces native calls inside the remaining scans; record-visit counters still describe the complete validation work. Step [4.15 local gate handoff](performance-gate-handoff.md) measures progress under idle polling and separately checks ordinary-call cost. Step [4.16 runtime windows](performance-runtime-windows.md) reduces native calls for gate-bound metadata reads. Step [4.17 operation validation reuse](performance-operation-validation.md) removes the duplicate prefix scan on a narrowly scoped first runtime execution attempt. Step [4.18 receive validation reuse](performance-receive-validation.md) shares the already validated selection model with issuance under the same unchanged gate-held operation. Step [4.19 direct delivery lookup](performance-delivery-lookup.md) resolves the oldest-pending link without decoding earlier Publications a second time. Step [4.20 definition reuse](performance-definition-reuse.md) shares checked Task catalogs and retained topology definitions during validation. Step [4.21 payload preparation](performance-payload-preparation.md) shares one fresh Field catalog between schema resolution and codec construction. Step [4.22 prefix buffers](performance-prefix-buffers.md) replaces per-record fixed-body allocation and reuses scan-local manifest input storage while retaining complete checks. Step [4.23 gate resource checks](performance-gate-resources.md) removes only the duplicate pathname-identity checks before native lock acquisition, retains post-lock rejection, and measures full acquisition separately from native ownership time. Step [4.24 Cachegrind profiling](performance-cachegrind.md) adds an on-demand optimized, diagnostics-OFF serial-cycle profile with fixed simulated caches and branches; profiled elapsed time is not ordinary latency evidence. Step [4.25 single-destination validation](performance-single-destination.md) removes unnecessary uniqueness scratch for exactly one destination and introduces same-job base/head profiles alongside matched ordinary measurements.

Here the **committed prefix** is the sequence of committed arena records ending at the Session's authoritative allocation tail. A full-prefix scan walks all of those records. It is separate from the short, reusable journal that describes a pending mutation. See the [glossary](glossary.md) for native storage terms.

## Match execution settings as well as code

Before/after binaries must run with the same execution permissions and restrictions, coordinator, workload and compiler settings. If setup requires elevated permissions, finish setup first and run both measurements separately under matching settings. Keep timing comparisons separate from exact work-count attribution.

A step-4.21 audit found that the initial local sequential before matrices in steps 4.19–4.21 used an unrestricted execution request, while their after matrices used the default restricted request. Those timing deltas cannot isolate the optimization effect. The raw reports remain unchanged. Their adjacent normal alternating studies already launched both variants under one default coordinator, so those matched comparisons remain applicable. Step 4.21 also adds a complete [matched normal control matrix](../benchmarks/results/milestone-4-payload-preparation/matched-controls/) and explicit [execution-context provenance](../benchmarks/results/milestone-4-payload-preparation/execution-contexts.toml). No experiment here estimates the permission profile's independent performance penalty.

## Build matching normal and diagnostic executables

Private instrumentation is disabled by default. Ordinary builds compile every diagnostic scope/counter macro away, including argument evaluation. The diagnostic executable is a research tool using the implemented public API. Its allocation hooks belong to that executable, not the library or primer.

```sh
cmake -S . -B build/measure-normal -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/measure-normal --parallel 3
cmake -S . -B build/measure-diagnostics -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DANGARIA_ENABLE_DIAGNOSTICS=ON
cmake --build build/measure-diagnostics --parallel 3
```

Use the same compiler, optimization flags, host, and workload for both builds. `none` records operation wall/CPU intervals without internal observations. An enabled build also supports `counts`, which counts work without internal phase clocks, and `timings`, which records counts and nested phase times. Compare all three modes and the normal build to expose observation overhead.

```sh
./build/measure-normal/angaria_transport_diagnostics build/diag-normal \
  --messages 16 --history 128 --collection none
./build/measure-diagnostics/angaria_transport_diagnostics build/diag-counts \
  --messages 16 --history 128 --collection counts
./build/measure-diagnostics/angaria_transport_diagnostics build/diag-timings \
  --messages 16 --history 128 --collection timings
python3 benchmarks/run_diagnostics.py build/measure-diagnostics/angaria_transport_diagnostics \
  --suite diagnostic --repetitions 3 --output build/diagnostic-results
```

Every Session/output directory must be new. The Python 3.9+ runner uses only the standard library. It keeps compressed raw per-operation JSON, readable CSV summaries, and TOML experiment metadata; temporary Session files are removed between runs. These result formats are separate from Angaria's TOML configuration format.

## Controlled workloads

Four complete channels are always registered, with nine Process Tasks including Setup, four Queues/Sources, eight-entry rings, and recovery-from-log delivery. Serial/backlog keep all nine claims attached. Stream and process experiments first release those claims, then attach only the measured producer/consumer at incarnation 2, plus any requested idle consumers. This matches the active ownership cohort for thread/process comparisons; serial-versus-stream differences also include that change in active ownership. One channel carries measured messages. A history of H messages is sent and acknowledged before timing, either on that channel (`same`) or an unrelated channel (`other`). The measured window requests N new publications; a budget stop can leave only M accepted and C acknowledged. Setup and history creation are outside operation timings, but their cost still contributes to total experiment duration.

| Option | Meaning and bound |
| --- | --- |
| `--messages N` | 1–128 measured publications; default 16. |
| `--history H` | 0–2048 acknowledged publications prepared before measurement; default 0. |
| `--history-scope same\|other` | Place that history on the measured or unrelated Queue. |
| `--payload-bytes B` | 8–4096 bytes; default 8. |
| `--fields F` | 1–512 ordered references dividing B exactly; default 1. |
| `--capacity-scale C` | 1, 4, or 16 times reserved capacities, with the same registered entities. |
| `--workload serial\|backlog\|stream` | Complete each send/receive/ack cycle, publish then drain, or overlap a producer and consumer thread. The Python runner also coordinates `process` experiments. |
| `--idle-pollers K` | 0–3 additional threads polling distinct empty Sources in stream mode. |
| `--collection none\|counts\|timings` | Root intervals only, counted work, or counted/timed work. Counts/timings require the enabled build. |
| `--work-budget-ms T` | Cooperative work budget for stream/process cases, 1–60000 ms; default 5000. Serial/backlog finish their requested work. |

Payloads use one registered fixed-ASCII Field of width B/F, repeated F times, and contain B `A` bytes. This separates payload size from Field count. It differs from step 4.11's uint64 payload and must be compared using its own matched controls. Every received byte, exact schema, publication/Queue sequence, consumer/token, and header context is checked before acknowledgment. The measured checksum is `C * B * 65`, where C is the number acknowledged (`N` when complete).

The tool times public ID issuance, buffered send, receive, acknowledgment, one retained acceptance lookup, one identical retry, and an empty poll. Stream mode includes legitimate receive timeouts and optional idle polls, recorded with their result alternative. No fault injector or private send replacement is used. Heap-backed sample buffers are allocated before the measured phases; output, sorting, and summary calculation happen afterward.

Serial/backlog samples record exact publication-history counts at their boundaries. Stream/process samples record observed lower bounds and a conservative upper bound, since a producer can commit before its local completion counter is updated. Timestamps establish local observation order, not a total order across processes or a reconstruction of private commit instants. Raw idle polling storage is bounded, but pollers continue for the entire stream. Complete per-idle-actor aggregates retain every poll; if raw storage fills, its latency quantiles are explicitly labeled as a captured prefix rather than the complete population.

For `process`, the runner initializes/preloads a Session, waits for Setup to exit, then starts independent producer and consumer executables. Each claims only its own role and prepares before reporting READY. A first stdin barrier releases both; DONE marks the end of that worker's data phase, including a budget stop. A second barrier waits until both are DONE before allowing status/retry/empty-poll controls and final JSON output. `process_phase_ns` spans first release through both DONE reports, excluding creation, attachment, control calls, and JSON serialization/transfer. Each worker's sample clock is actor-relative; samples are never sorted as if those clocks shared an origin. PIDs/incarnations are retained. Timeouts and failures kill and reap both workers and preserve their diagnostic output.

Concurrent workloads can report `outcome="budget_exceeded"`. This is a valid experimental outcome, with actual accepted M, acknowledged/completed C, pending M−C, and checksum `C * B * 65`, rather than pretending that all requested N messages completed. The budget is checked between API calls. Already started operations finish consistently, so settlement and cleanup can extend beyond that interval; it is not a hard response-time limit. If receive returns after the budget, acknowledgment is skipped and `unacknowledged_at_stop=1` records that verified borrow. Closing the Source after the work phase returns it to Pending. An issued ID whose send never starts remains distinguishable from an accepted publication. Genuine API errors or incorrect data still fail the experiment.

Status/retry controls use the last actual accepted ID and are omitted when M is zero. An empty control is omitted when consumption is incomplete. `runs.csv` preserves every invocation, including zero-progress cases; latency summaries contain only observed calls and group by outcome/progress. Idle aggregates cover every poll, while truncated raw latency samples remain explicitly a prefix. A completed matrix can contain budget-exceeded workloads: CI verifies their state and reporting, not a speed requirement.

Backlog records file lengths after publishing and after draining separately. Serial/stream/process interleave those activities, so both named lengths denote their final data-phase length. Equality in those cases is not a separate no-append-drain measurement. The process coordinator also checks that its later controls leave file length unchanged.

## What is measured

Each fixed-size Sample wraps one real public call. It records a monotonic wall interval and, when available, the calling thread's CPU interval. The root interval includes its CPU-clock observation overhead. CPU availability is explicit. None of these measurements establish a maximum response time.

An **inclusive** phase includes its nested work. An **exclusive** phase subtracts nested phase intervals. Summing exclusive phases plus `other`, the unattributed remainder, gives the captured wall interval. Inclusive values overlap and must not be added together. Repeated scopes of the same name accumulate visits and times; they are not necessarily one contiguous interval. Counts mode has phase visit counts with zero internal durations. None mode has no phase/counter observations.

| Observation | Scope |
| --- | --- |
| Gate acquisition | Full acquisition request, including the local mutex, resource checks, native lock, and scheduling; not a pure lock-wait counter. Busy attempts are counted separately. |
| Native gate ownership | Completed native-lock intervals from successful acquisition to just before unlock. `native_gate_hold_ns` overlaps phase times and must not be added to their exclusive sum. It excludes native unlock and the remaining local-mutex release tail. Counts mode records intervals with zero duration. |
| Recovery | Healthy Idle checks are distinguished from non-idle processing attempts, including Prepared rollback. A zero non-idle count separates steady work from actual recovery. |
| Snapshot/prefix/index | Full committed-prefix scans and record indexing, with exact record visits. |
| Catalog/topology/delivery | Reconstructed definitions, configured Task-slot visits, full delivery validation, and Publication visits. |
| Lookup/schema/codec | Publication lookup visits, schema work, codec construction, and payload validation. |
| Journal preparation/execution | Plan building, preflight, prepared-body work, file growth, materialization/accepted-batch validation, and apply. Enclosing scopes' exclusive times subtract their observed children. |
| Native I/O/mapping | Helper-region times for reads, stats, mapping, and unmapping, including checks/bookkeeping. These are not isolated syscall durations. |
| Native counters | Actual `pread`, `mmap`, `munmap`, `fstat`/`fstatat`, and file-resize attempts, including retries. Read bytes count successful reads; mapped bytes count successful requested extents including page offset, not resident memory. |
| Allocation counters | Successful C++ `new` requests and requested bytes in the captured thread. They exclude direct `malloc`, kernel storage, other threads, allocation metadata, and live/peak memory. |

The collector uses thread-local state and fixed arrays without allocating or logging. Nested phases use stack frames; overflow, invalid nesting, and clock validity are reported rather than silently producing plausible totals. Native error context (`errno`) is preserved. Unit tests exercise enabled/disabled behavior, repeated/nested phases, unwinding, thread isolation, overflow, and error-context preservation.

The enabled build adds a private gate-timing token to each mutation guard. Its metadata contributes to instrumented allocation byte counts. Those requested-byte values therefore describe the enabled executable, rather than asserting the exact heap footprint of an ordinary build. A guard released outside its originating capture is ignored safely; the token retains no expired Sample/Capture pointer.

In [step 4.23](performance-gate-resources.md), removing the pre-lock resource checks targets full gate acquisition. Native hold timing starts after successful `flock`, so those removed checks were outside the hold interval. A lower acquisition cost does not imply a lower hold time. Compare native stat attempts as work evidence and matched ordinary timings as performance evidence; instruction/cache profiling with Cachegrind remains a separate follow-up.

[Step 4.24's Cachegrind method](performance-cachegrind.md) supplies that separate instruction/cache/branch profile. Its optional benchmark hooks bracket the existing serial cycle after preload, with private diagnostics OFF. Cold simulated caches, virtual-address effects and excluded kernel/TLB/multicore behavior limit interpretation. Use its function/source annotations to locate CPU work; use unprofiled matched runs for speed comparisons and native counters/traces for syscall cost.

[Step 4.25](performance-single-destination.md) uses the optional `profile-cachegrind-compare` label to collect both immutable PR base/head versions on the same GitHub job. Each checkout builds and runs its own nine-case profiler with the same flags and fixed model; `before/` and `after/` artifacts remain separate. An older standalone profile identifies a candidate but cannot substitute for that controlled comparison. Profiled instruction/cache changes and ordinary latency changes must still be reported separately.

## Interpretation and next changes

The controlled history sweep measures the same short window after different preload lengths. Compare `same` and `other` to distinguish Queue-specific backlog from Session-wide history work. Capacity controls vary reservations without creating more entities. Byte/Field controls separate copying from schema complexity. Backlog/stream/idle-poller controls expose work hidden when publishing and draining happen in separate phases.

The first optimization candidate is reuse of already validated information within an unchanged gate-held operation. A longer-lived cache requires separate rules for immutable definitions, append history, mutable receipts/claims/pins, cross-process commits, recovery, and rollback. Registry revision or append tail alone cannot prove all of those are unchanged. Existing corruption/recovery rejection behavior remains part of qualification.

The raw results and measured source commit are the evidence for selecting a fix. Performance observations are never CI speed thresholds. The ordinary suite, original benchmark smoke, new diagnostic smoke, and collector tests run alongside each other. Automatic Linux CI retains a bounded result matrix as an artifact; native ARM64 profiles also compile/test the enabled instrumentation. Intel Mac CI remains request-only.

[Step 4.26](performance-manifest-validation.md) targets temporary manifest reference/command vectors and payload copies within complete history validation. Allocation counters and matched native timings qualify its effect; the same-job Cachegrind comparison provides separate work evidence.
