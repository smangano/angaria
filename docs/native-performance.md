# Native transport measurements

Milestone 4 step 4.11, framework **0.4.13**, measures the implemented direct Queue transport. The [completion audit](milestones/milestone-4-completion.md) records its compatibility and progress boundaries. These are measurements of a correctness baseline with a blocking Session-wide mutation gate, not a latency guarantee or a comparison with other frameworks.

## Reproduce a run

Build with optimization and keep the build type and compiler flags with the results. The executable needs neither GoogleTest nor Python; the matrix runner uses Python 3.9 or later from the standard library.

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/release --parallel 3
python3 benchmarks/run_native.py build/release/angaria_transport_benchmark \
  --output build/native-results --messages 128 --warmup 16 --repetitions 3 \
  --payload-bytes 8,256 --producers 1,2,4 --modes serial,phased,profile
```

The output directory must be new. It holds raw `reports.jsonl`, an aggregate `summary.csv`, and a TOML metadata manifest. Session files are created in separate temporary directories and removed after each invocation. The manifest identifies the source commit, working-tree state, measurement-source hashes, executable hash, compiler/build information when available, host, command, and UTC run times. JSON here is measurement output, not an Angaria configuration format.

For a single run, give the executable a new Session directory:

```sh
./build/release/angaria_transport_benchmark build/native-single 128 16 4 256 phased
```

Arguments are `DIRECTORY TOTAL WARMUP PRODUCERS PAYLOAD_BYTES MODE`. Total is 1–4096; warmup is 0–256. Both counts must be divisible by the number of producers, which is 1, 2, or 4. Payload sizes are multiples of 8 from 8 to 4096 bytes. Serial mode uses one producer. A failed operation reports its Angaria error and exits unsuccessfully rather than emitting a success report.

## Workload and timing boundaries

Every fresh Session registers the same four producer/consumer channels: nine Process Tasks including Setup, four Queues, four Sources, and one Active Queue per Source. All channels are attached even when only one or two produce. Each Queue has an eight-entry ring and the default recovery-from-log policy; the Session has a 64 MiB append limit. The schema repeats one uint64 Field, and its Binary payload contains words of value 1. Schema registration and encoding finish before timing.

Warmup publications are sent and acknowledged before measurements. They remain in retained history but contribute no timing samples, checksum, or completed count. For a matrix with total N and warmup W, every case traverses the same total publication-history range W through W+N. Increasing the producer count partitions that work among independent Queues; it does not multiply the total workload or registered catalog geometry.

| Mode | Measured work | Throughput denominator |
| --- | --- | --- |
| `serial` | Issue ID, public buffered send, receive, verify, and acknowledge, repeated on one channel. | `cycle_phase_ns`: time for N complete cycles. |
| `phased` | P producer threads issue IDs and send; after all finish, P consumer threads receive, verify, and acknowledge. | Separate `publish_phase_ns` and `drain_phase_ns`, each for N messages. |
| `profile` | Same phases, with public ID issuance and consumer operations, but an instrumented private send engine. | Instrumented phase times; these are not public API throughput. |

Workers start at a condition-variable barrier after thread creation. Phase time ends at the last worker's completion, before joins. Phase and serial-cycle times include harness checks, metric recording, payload verification, and checksum work. Per-operation timers surround just the API call (or private engine region); they exclude subsequent harness validation. Timers use a monotonic clock. UTC publication/enqueue/dequeue times are checked as message metadata, never subtracted to measure latency.

Phased consumers drain a backlog after publishing stops. This exercises log fallback beyond the ring, rather than a concurrent producer/consumer streaming workload. The serial case acknowledges between publications; phased cases retain pending receipts until draining. Those different states are part of the workload and prevent treating serial-versus-phased differences as an isolated lock-cost experiment. Active Queue counts, per-Queue history, scheduling, OS cache state, and history validation can also affect comparisons among producer counts. This tool uses threads in one process; [independent-process qualification](native-qualification.md) and the [market-monitor application](native-pipeline.md) cover process replacement separately.

## Report fields and statistics

Each success report identifies the framework version, mode, `api_kind`, N, W, producer count, registered channels, payload size, ring capacity, history range, completed count, payload checksum, phase durations, and physical Session lengths. The checksum must equal `N * payload_bytes / 8`; every received schema, full header, Queue sequence, token, and byte is also checked before acknowledgment. No timing threshold determines success.

The `metrics` array contains one entry per observed operation. Each includes count, minimum, maximum, total, p50, p95, and p99 nanoseconds. Quantiles use the nearest-rank rule: sort N samples and select rank `ceil(percent * N / 100)`, counting from one. Samples from all active channels in that invocation are combined. The CSV reports medians across repeated runs, including the median of each run's quantile; it does not claim to calculate a pooled quantile without individual samples. With only 128 samples per run, p99 is a small-tail observation rather than a stable service-level estimate.

Public `serial`/`phased` reports have `api_kind="buffered_public"`. Their `issue`, `send`, `receive`, and `acknowledge` metrics time actual implemented calls; serial also records `cycle`. Caller-buffer sends use space for the one destination. Owning convenience outcomes and application payload encoding are outside this baseline.

`append_bytes` is the physical file-length difference across N publications, including records, receipts, manifest, redo, and alignment. `file_bytes_after_drain` must equal `file_bytes_after_publish`. In serial mode both fields denote the final length because send and drain are interleaved. This measures append storage, **not process heap allocations, mapping count, resident memory, storage-device traffic, or reclamation**. Reserved progress can allocate process memory or read/map history while appending no bytes.

## Instrumented gate and commit profile

The `profile` report has `api_kind="instrumented_private_engine"`. It adopts the real mutation guard and observes existing journal checkpoints through a read-only `JournalFaultInjector`. It neither injects failures nor changes production code. Producers own no Sources in this workload. The private call omits public ownership/deferred-Source wrapper work; its timings cannot be substituted for public send latency. The observer also adds virtual callbacks at word boundaries and clock reads at four selected checkpoints. Do not subtract profile times from public timings as if instrumentation were free.

| Metric | Start → end | Includes |
| --- | --- | --- |
| `gate_wait` | Before guard acquisition → acquired | Local mutex/native gate acquisition and scheduling. Only this send acquisition is profiled. |
| `gate_held` | Acquired → RuntimeAccess destroyed | Recovery, history/index validation, append preparation, journal execution, and release overhead. |
| `prepare` | Acquired → Prepared checkpoint | Recovery/validation/preflight and writing the prepared journal body. |
| `file_growth` | Prepared → FileGrowth checkpoint | Actual native file resize and checkpoint overhead. |
| `materialize` | FileGrowth → Committed checkpoint | Mapping/writing appended records, readiness, and accepted-batch validation. |
| `apply` | Committed → Idle checkpoint | Applying roots/live state and finishing journal state transitions. |
| `profile_send` | Before acquisition → after release | Instrumented engine wait plus held time. |

The four interior stages omit the Idle-to-release tail, so they need not sum to `gate_held`. ID issuance and consumer operations also acquire the gate; their waits are included in their public call latencies, not in `gate_wait`. A small send-only gate-wait sample therefore cannot establish that a workload had little contention. Gate wait includes scheduling and acquisition overhead even with one producer, and is not a pure lock-contention counter.

## Interpretation and scope

Run on an otherwise quiet machine, record power settings and load if controlled, and repeat before drawing conclusions. This baseline does not pin CPUs, control frequency, flush caches, or establish disk-cold behavior. Coherent local mappings and process-crash recovery remain the [supported native scope](native-storage.md); there is no `fsync`-based power-loss guarantee. A paused live gate owner can block others. No lock-free, starvation-free, hard-real-time, or cross-platform performance claim follows from these numbers.

The benchmark intentionally leaves production algorithms unchanged. Any future optimization of history scans, allocation, or gate partitioning must preserve the [qualified recovery outcomes](native-qualification.md) and use the same workload and timing boundaries for a meaningful comparison. Larger Sessions, long-running mixed traffic, process-versus-thread contention, end-to-end application work, and other policies require additional experiments.

## Correctness checks in CI

With `BUILD_TESTING=ON`, `native_benchmark_smoke` checks bounded serial/phased/profile cases, report invariants, payload counts, no-append draining, and rejected arguments/existing paths. It asserts correctness only. Automatic CI runs Linux x86_64, Linux ARM64, and macOS ARM64; macOS x86_64 remains request-only. Use `-DANGARIA_BUILD_BENCHMARKS=OFF` to omit the tool and its smoke check.

## Recorded milestone-4 baseline

The [raw reports, CSV, and manifest](../benchmarks/results/milestone-4-macos-arm64/) record **42 successful runs**, three repetitions of fourteen cases, measured on **2026-11-26**. The native executable is ARM64 on an Apple M4 with ten logical CPUs and 16 GiB memory, macOS 27.0.1, Apple clang 21.0.0, Release `-O3 -DNDEBUG`, with `-Wall -Wextra -Werror`. The measured source commit is [`d2c728c502fc2365711f21f0f34e6980548f5293`](https://github.com/smangano/angaria/commit/d2c728c502fc2365711f21f0f34e6980548f5293). The follow-up results commit changes documentation/data only. Source and executable SHA256 values are retained in the manifest.

The native benchmark source/binary is unchanged after that measurement. A later runner-only fix preserves non-BMP Unicode in TOML metadata; it does not change native timing or report aggregation. The manifest retains the exact runner hash used for the experiment. The runner's Python 3.10.15 process reports `x86_64`; it launches the separately verified ARM64 executable. Do not mistake that manifest `machine` field for the benchmark's architecture. The [dataset note](../benchmarks/results/milestone-4-macos-arm64/README.md) records verification and the distinction between tracked-clean source and unrelated untracked workspace files. CPU discovery was unavailable to that process; the host label records separately verified hardware. Execution used the managed Codex environment, with no CPU affinity, power controls, isolated-host, or cache-cold guarantee.

All cases use **N=128, W=16**, four registered channels, eight-entry rings, and retained publication history **16 → 144**. The table reports medians across three runs; latency columns are medians of each run's quantile. Rates include ID issuance in publishing and acknowledgment in draining, plus harness work as defined above. They exclude Session setup and encoding.

| Payload bytes | Public phased producers | Publish messages/s | Drain messages/s | Send p50 ms | Send p95 ms |
| --- | --- | --- | --- | --- | --- |
| 8 | 1 | 107.2 | 58.4 | 5.237 | 7.955 |
| 8 | 2 | 107.3 | 56.8 | 9.541 | 15.299 |
| 8 | 4 | 106.8 | 57.9 | 19.003 | 29.526 |
| 256 | 1 | 108.9 | 59.5 | 5.111 | 7.670 |
| 256 | 2 | 108.0 | 56.9 | 9.724 | 14.923 |
| 256 | 4 | 109.9 | 58.3 | 18.676 | 29.040 |

Serial send/receive/acknowledge cycles complete at **49.4/s** for eight-byte payloads and **50.1/s** for 256-byte payloads. Serial send p50 is **5.272 ms** and **5.156 ms** respectively. Each accepted publication appends **704 bytes** or **960 bytes** respectively, including native records/journal geometry. Draining adds zero file bytes in all 42 runs. All **5,376 measured messages** have verified headers, tokens, payloads, and acknowledgment; warmup messages are additional and excluded.

The instrumented eight-byte send profile gives the following median-of-run p50 values. These are private engine measurements with observation overhead, not the public send values above.

| Producers | Send gate wait µs | Gate held ms | Preparation ms | File growth µs | Materialization µs | Apply µs |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 12 | 5.024 | 4.902 | 78 | 17 | 12 |
| 2 | 15 | 5.161 | 4.993 | 79 | 17 | 13 |
| 4 | 15 | 5.069 | 4.959 | 77 | 17 | 12 |

Across both payload sizes and all producer counts, preparation occupies **97.5–97.8%** of aggregate profiled gate-held time (median per-run ratio). Preparation includes history/recovery validation and append/journal planning; this coarse profile does not identify one function as the cause. The public ID-issuance p50 also rises with more producers, while the instrumented send-only gate wait stays small. Wait can occur during ID issuance and other calls; the small send-only sample does not establish little contention.

In this workload, more producers raise call latency without improving aggregate throughput, consistent with serialized mutation. Changing payload size from 8 to 256 bytes has little effect relative to total operation cost. This is a substantial performance limitation for latency-sensitive applications, not a release-ready trading-system benchmark. The next useful investigation is finer profiling of history validation and preparation before proposing an optimization. Longer histories, streaming traffic, independent-process contention, and Linux measurements remain separate experiments; this dataset supports no claims about their rates.
