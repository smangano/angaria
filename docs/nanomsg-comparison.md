# Running the 1:1 IPC comparison

Milestone 4 step **4.28**, framework **0.4.30**, implements phase 1 of the [comparison plan](nanomsg-comparison-plan.md). One producer and one consumer run as independent local processes. A Python coordinator establishes the connection, checks clocks, drains preload messages, and starts each measured batch. A **cohort** is the finite batch of measured messages in one fresh run.

The two profiles provide different guarantees:

| Profile | What the benchmark observes | Library guarantees used |
| --- | --- | --- |
| Angaria acknowledged, recorded | Accepted publications, verified deliveries, and processing acknowledgments | Retained message history and receipt state under the existing process-recovery contract; no power-loss durability promise |
| Classic libnanomsg IPC, unacknowledged | Successful PUSH sends and independently verified PULL deliveries | No application processing acknowledgment or replay log supplied by this profile |

A timing ratio includes differences in recording, validation, synchronization, allocation, buffering, and transport. It cannot isolate the cost of acknowledgment. The [recorded unacknowledged Angaria profile](recorded-unacknowledged-delivery-plan.md) remains planned work. This step changes no public API or runtime guarantee. The primer's seventeen runnable examples were reviewed and require no changes.

[Retained Linux measurements and plots](../benchmarks/results/milestone-4-nanomsg-comparison/README.md) show the current baseline, including long concurrent-process stalls.

## Build and qualify

The comparison dependency is optional and OFF by default. Ordinary Angaria builds do not fetch or link nanomsg. Opting in fetches classic **libnanomsg 1.3.0**, statically linked into the comparison executable. CMake pins source commit `73e89d2ca28d11cbd39bb15e8d2e1edeb7eec68a` and the archive SHA-256 `acf65c0ef312f431aa3c4cb114326781c999ec0c977067f3a1f0c81b5ec8710f`. This is classic libnanomsg, not NNG.

Use a C/C++20 toolchain, CMake, Boost headers, and Python 3.9 or later. Configuration requires access to the pinned upstream archive. Diagnostics and Cachegrind must be OFF for this ordinary timing target.

```sh
cmake -S . -B build/ipc -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS=-g -DCMAKE_C_FLAGS=-g -DBUILD_TESTING=OFF \
  -DANGARIA_BUILD_IPC_COMPARISON=ON \
  -DANGARIA_ENABLE_DIAGNOSTICS=OFF -DANGARIA_ENABLE_CACHEGRIND=OFF
cmake --build build/ipc --target angaria_ipc_comparison --parallel 3
python3 -B test/ipc_comparison.py build/ipc/angaria_ipc_comparison
```

The qualification script exercises both real adapters, independent payload checks, nonzero history, paced releases, invalid reports, and bounded participant cleanup. It rejects acknowledgments before verification or after the data phase, and cached metrics/rates inconsistent with worker timestamps. Synthetic incomplete attempts check the whole stop interval and zero completed work. Its pass criteria are correctness and accounting, with no performance threshold.

For the five native deadline-helper regressions, install GoogleTest, reconfigure with `-DBUILD_TESTING=ON`, build the additional target `angaria_ipc_comparison_tests`, and run it directly. With both targets built, `ctest --test-dir build/ipc --output-on-failure -R '^ipc_comparison_(deadlines|smoke)$'` runs both qualification entries. These private helper tests distinguish actual deadline exhaustion from late transport, payload, schedule and clock errors. GoogleTest is not required to build or run the comparison itself.

## Record matched runs

```sh
python3 -B benchmarks/run_ipc_comparison.py build/ipc/angaria_ipc_comparison \
  --output build/ipc-results --host-label 'describe this host'
```

The destination must be new. Defaults are 16 measured messages, payloads of 64/256/1,024 bytes, and **H=0/32/128** prior verified messages. Angaria preload messages are acknowledged before measurement; nanomsg preload messages are drained without per-message return acknowledgments. H is retained log history for Angaria, and prior traffic only for nanomsg. Each of the nine cells uses five alternating ABBA/BAAB blocks, or ten runs per profile: **180 attempts** overall. No timing-based outlier removal is applied. An infrastructure, transport or validation failure stops execution and retains its raw output, even if it occurs after the deadline. Only an explicit deadline-exhaustion outcome, or otherwise successful work finishing late, is a work-budget stop.

`--smoke` uses four short attempts. `--messages`, `--histories`, `--payload-bytes`, `--blocks`, `--budget-ms`, and `--timeout` bound experiments. `--interval-ns` specifies an offered schedule. Its default zero makes all messages due at T0, a finite burst. Positive intervals retain the original release time when a producer falls behind. This prevents late messages from disappearing from scheduled latency. The last release must precede the phase deadline.

Static endpoint identities are captured before START; measured verification compares local header values and payload bytes. Both profiles use the same 32-byte little-endian envelope and deterministic remaining bytes. The coordinator independently reconstructs the expected bytes and FNV-1a hash for every accepted/verified sequence. It rejects missing, duplicated, changed, or reordered identities in complete runs. Raw worker timestamps and actual Angaria publication identities remain available for audit.

The executable uses the public Angaria API: a fresh Session, three Tasks (setup and the two workers), one Queue, one Source with one Active Queue, an eight-slot ring, and a 64 MiB log limit. Producers include ID issuance in the complete producer path. Angaria receives a borrowed view of logged bytes; this is not an equal receive-copy-count experiment. Nanomsg uses copying PUSH/PULL calls over a fresh UNIX-domain IPC endpoint, one pair for this 1:1 edge. The requested socket buffers default to 131,072 bytes; actual values are recorded. Finite nonblocking retries/polls enforce a common work deadline. Nanomsg connection readiness is checked before START, including H=0; no hidden warmup payload is sent there.

## Interpret the measurements

Each worker names `CLOCK_MONOTONIC`, reports its resolution and the minimum/median of 256 warmed clock-read pairs, and passes three bracketed coordinator probes before measurement. Clock overhead is recorded, not subtracted from samples. START names a common future T0. Receipt is timestamped immediately after the receive call, before payload verification and any Angaria acknowledgment.

- **Logical-start-to-delivery latency** includes payload preparation, Angaria ID issuance, send/backpressure, and receive. It starts before the producer fills the message timestamp.
- **Scheduled-to-delivery latency** also includes producer lateness and earlier messages' work. With a burst, later messages remain scheduled at T0.
- **Verified publications/s** divides verified deliveries by T0 to the last verification. This 1:1 shape has one delivery per logical publication. The first-send-to-last-verification rate is retained separately to expose startup jitter.
- **Acknowledged publications/s** uses the last acknowledgment, for Angaria only. A nanomsg verification observation does not create a protocol processing acknowledgment.
- Separate samples expose actual send/receive API time, retries/waiting, ID issuance, verification, and acknowledgment. Process CPU includes all library threads from START receipt, before waiting for T0, through the common FINISH barrier (including the declared lead wait); CPU through each worker's own DONE (also including the lead wait), lifetime child CPU, peak memory, and available thread counts are retained separately.

These short fresh cohorts measure bounded completed work, **not sustained capacity**. Report startup jitter alongside throughput. A default cell has 160 delivery observations per profile; p99 and p99.9 are marked insufficient. The runner requires at least 10,000 and 1,000,000 observations respectively within an identical complete cell before reporting those tails. Summaries lead with the median of each run's nearest-rank median, and retain the range across runs.

`reports.jsonl` retains each attempt, raw per-message timings, clock probes, counts, and resource use. `runs.csv` lists all outcomes. `summary.csv` aggregates only matched cells and separates incomplete populations. Each attempt directory preserves command arguments, control events, stdout, stderr, and its report, including failures. `manifest.toml` records source/executable hashes, dependency identity and runtime/header ABI, C/C++ toolchains and flags, host, command, and limits. Incomplete verified/acknowledged rates use T0 through the later participant data-phase stop, including any stall after the last completed message. Zero completed work contributes a zero-rate observation. Last-verification/acknowledgment prefix rates remain separately named diagnostics. Complete-run rates retain their original last-completion boundaries. The plotter reconstructs all counts, timing samples and rates and rejects mismatched cached aggregates before drawing figures. Temporary Session files and IPC endpoints are removed after each attempt.

## Request hosted qualification

Apply the `compare-nanomsg` PR label to request the optional workflow against that immutable PR head. It runs correctness qualification on Linux x86_64, Linux ARM64, and macOS ARM64, then records the full matrix on Linux x86_64. The artifact `ipc-comparison-linux-x86_64` retains all measured attempts for 14 days. Manual dispatch is also available on branches containing the workflow. Ordinary CI and request-only Intel Mac qualification keep their existing behavior. Relabel to run again after a source change; the label alone does not schedule every push.

Fan-out, multiple-producer Source selection, native Publishing acceptance, long-running capacity sweeps, and recorded/unacknowledged delivery are later [qualified phases](nanomsg-comparison-plan.md#topology-and-implementation-phases). They are not results produced by this executable.
