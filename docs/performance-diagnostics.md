# Diagnosing native transport costs

Milestone 4 step **4.12**, framework **0.4.14**, investigates the poor costs recorded in the [step-4.11 baseline](native-performance.md#recorded-milestone-4-baseline). Integration of milestone 4 into `main` is held until the dominant costs are understood, measured improvements are reviewed, and performance targets for a representative workload are agreed. This step adds measurement tools and evidence; it does not change a transport algorithm or weaken validation.

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

Four complete channels are always registered, with nine Process Tasks including Setup, four Queues/Sources, eight-entry rings, and recovery-from-log delivery. Serial/backlog keep all nine claims attached. Stream and process experiments first release those claims, then attach only the measured producer/consumer at incarnation 2, plus any requested idle consumers. This matches the active ownership cohort for thread/process comparisons; serial-versus-stream differences also include that change in active ownership. One channel carries measured messages. A history of H messages is sent and acknowledged before timing, either on that channel (`same`) or an unrelated channel (`other`). The measured window always contains N new publications. Setup and history creation are outside operation timings, but their cost still contributes to total experiment duration.

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

Payloads use one registered fixed-ASCII Field of width B/F, repeated F times, and contain B `A` bytes. This separates payload size from Field count. It differs from step 4.11's uint64 payload and must be compared using its own matched controls. Every received byte, exact schema, publication/Queue sequence, consumer/token, and header context is checked before acknowledgment. The measured checksum is `N * B * 65`.

The tool times public ID issuance, buffered send, receive, acknowledgment, one retained acceptance lookup, one identical retry, and an empty poll. Stream mode includes legitimate receive timeouts and optional idle polls, recorded with their result alternative. No fault injector or private send replacement is used. All sample buffers are allocated before the measured phases; output, sorting, and summary calculation happen afterward.

Serial/backlog samples record exact publication-history counts at their boundaries. Stream/process samples record observed lower bounds and a conservative upper bound, since a producer can commit before its local completion counter is updated. Timestamps establish local observation order, not a total order across processes or a reconstruction of private commit instants. Raw idle polling storage is bounded, but pollers continue for the entire stream. Complete per-idle-actor aggregates retain every poll; if raw storage fills, its latency quantiles are explicitly labeled as a captured prefix rather than the complete population.

For `process`, the runner initializes/preloads a Session, waits for Setup to exit, then starts independent producer and consumer executables. Each claims only its own role and prepares before reporting READY. A first stdin barrier releases both; DONE marks completed publication or draining. A second barrier waits until both are DONE before allowing status/retry/empty-poll controls and final JSON output. `process_phase_ns` spans first release through both DONE reports, excluding creation, attachment, control calls, and JSON serialization/transfer. Each worker's sample clock is actor-relative; samples are never sorted as if those clocks shared an origin. PIDs/incarnations are retained. Timeouts and failures kill and reap both workers and preserve their diagnostic output.

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

## Interpretation and next changes

The controlled history sweep measures the same short window after different preload lengths. Compare `same` and `other` to distinguish Queue-specific backlog from Session-wide history work. Capacity controls vary reservations without creating more entities. Byte/Field controls separate copying from schema complexity. Backlog/stream/idle-poller controls expose work hidden when publishing and draining happen in separate phases.

The first optimization candidate is reuse of already validated information within an unchanged gate-held operation. A longer-lived cache requires separate rules for immutable definitions, append history, mutable receipts/claims/pins, cross-process commits, recovery, and rollback. Registry revision or append tail alone cannot prove all of those are unchanged. Existing corruption/recovery rejection behavior remains part of qualification.

The raw results and measured source commit are the evidence for selecting a fix. Performance observations are never CI speed thresholds. The ordinary suite, original benchmark smoke, new diagnostic smoke, and collector tests run alongside each other. Automatic Linux CI retains a bounded result matrix as an artifact; native ARM64 profiles also compile/test the enabled instrumentation. Intel Mac CI remains request-only.
