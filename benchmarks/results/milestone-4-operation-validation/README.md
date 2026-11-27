# Operation validation reuse measurements

Step **4.17**, framework **0.4.19**. The [guide and risk assessment](../../../docs/performance-operation-validation.md) define first-attempt reuse and its private trust boundary. The [diagnostic method](../../../docs/performance-diagnostics.md) defines geometry, counters, timing windows and incomplete outcomes.

## Provenance

The **before** normal executable is the exact retained 0.4.18 binary, SHA256 `be3fc0425268480a0c1925f923b6efbc624b3b52f1cfc2fc7568a2e5905831b8`, compiled from `47da91cbcded7bf6c6bd160129eaccfa8de88b51`. It was preserved before rebuilding. Its measurement checkout is merged staging `a9d5a6d2fa1fe41c9dd2b89479cb0eb17d4fc60a`, with all **128 input hashes** identical to that measured source. The manifest identifies this measurement checkout rather than claiming the binary was compiled there. Original compiler/cache settings were retained beside it.

The **after** executables are built from `b805fd880e5769dd9e0069499ae4735d96a8863c`, version 0.4.19, with **128 input hashes** shared by normal and enabled builds. Differences are VERSION and thirteen source files implementing the private runtime/journal path and migrating its production callers. Later commits add documentation/results/CSV attributes only. Builds use Apple clang 21, Release `-O3 -DNDEBUG`, plus `-Wall -Wextra -Werror -g`. Diagnostics are OFF for timing comparisons and ON for attribution.

The local host is Apple M4, ten logical CPUs, 16 GiB, macOS 27.0.1, native ARM64 executables, with an x86_64 Python 3.10.15 coordinator. Measurements occurred November 26, 2026 in New York (November 27 UTC). Sessions use the local Data volume. No builds or tests ran during measurement. Host isolation, thermal/frequency/affinity controls and cache flushing were not used. Small studies do not qualify production tail latency.

## Populations and ordering

- [Fresh before normal](macos-arm64-before-normal/): 33 validated complete runs.
- [After normal](macos-arm64-after-normal/): 33 validated complete runs.
- [After enabled](macos-arm64-after-enabled/): 75 validated complete runs with None/Counts/Timings controls.
- [Alternating serial cost](alternating/): 30 validated complete runs, fifteen per variant; five pairs at H=0/32/128. [order.csv](alternating/order.csv) names each adjacent pair and execution sequence.
- [Hosted Linux after](linux-x86_64-after/): 54 validated complete runs, all six loaded cases included, with a separate native syscall trace.

The full matrices ran sequentially: before normal, then (after builds/tests completed) after normal and after enabled. Fresh Sessions use sixteen-message windows, four channels, eight-entry rings and unchanged history/capacity/payload/Field controls. Serial/backlog keeps nine active claims; concurrent thread/process cases keep two plus requested idle consumers. Their rates are separate populations.

The extra serial study uses fifteen adjacent before/after pairs, alternating AB then BA (eight AB and seven BA pairs), with H=0,32,128 in every repetition. Ordering is deterministic, not randomized. It reuses the [preceding comparison harness](../milestone-4-gate-handoff/run_interleaved.py), whose hash is recorded in both manifests. The harness imports each checkout's own runner/version/metadata globals and reuses its validators and artifact writer. Whole-invocation durations in order.csv include setup/preload, not just operation latency.

Raw reports are compressed JSON lines with CSV run/summary/phase tables and TOML metadata. Latency summaries take nearest-rank quantiles within each run, then medians across runs; samples are not pooled. Ratios of variant medians differ from medians of paired percentage changes. The guide's main table uses five alternating runs per variant and compares variant medians. All initial sequential results remain retained: H=0 send is flat/slightly higher there, and loaded-window elapsed ranges overlap despite lower send-call medians. The observations do not establish a universal speedup.

Count comparisons use identical-shape [retained 0.4.18 enabled cases](../milestone-4-runtime-windows/macos-arm64-after-enabled/) and new enabled runs. All three repetitions agree for each quoted first-call counter. Before counted input hashes match the preserved baseline; fresh before normal runs provide timing comparisons. The [figure](operation-validation.svg) uses five alternating H=128 normal runs per variant on the left and first-call counted prefix visits on the right. Bars are observations, not confidence intervals. Its [generator](generate_figure.py) reuses the preceding report's readers/quantiles and needs matplotlib; benchmark runners need only Python's standard library.

## Linux evidence

[Linux CI run 45931104566](https://github.com/smangano/angaria/actions/runs/45931104566) produced the unchanged downloaded artifact. Its synthetic merge is `15c4b3f72dc9fb3ba0ed6eace6742289e9a6f034`; all 128 inputs match the measured after source. Its eighteen-case matrix uses eight-message windows and completes **54/54** workloads, including all six three-idle-poller cases at 8/8. Timed first H=128 send agrees with the local prefix/native counters: one scan/319 visits, zero independent index scans, 21 maps, 7 reads and 65 stats. Heap counts differ by platform/library (5,323 Linux versus 5,194 Mac allocations for that call), so they are not asserted equal. Hardware, compiler, scheduling and window length differ; absolute timings are not a controlled platform comparison or fairness guarantee.

The separate [syscalls.txt](linux-x86_64-after/syscalls.txt) covers startup, setup, H=32 preload, eight serial messages, controls and teardown. Compared with the [retained 0.4.18 trace](../milestone-4-runtime-windows/linux-x86_64-after/syscalls.txt), whole-program totals are:

| Native call | Before | After |
| --- | ---: | ---: |
| getpid | 90,159 | 89,342 |
| fstat | 7,285 | 6,876 |
| pread64 | 2,169 | 2,169 |
| mmap | 3,206 | 3,002 |
| munmap | 3,185 | 2,981 |

These modest native-call reductions and unchanged reads are consistent with the focus on duplicate scan/heap work. Both traces use matching arguments and corresponding source, but include much more than the measured eight-message window. Do not divide their totals by eight or use traced time shares/seconds as ordinary latency evidence.

## Reproduce

Build matching before/after checkouts at the recorded commits, then use each checkout's corresponding executable/driver and a new output directory:

```sh
python3 -B benchmarks/run_diagnostics.py MATCHING_EXECUTABLE \
  --suite diagnostic --repetitions 3 --output NEW_RESULTS \
  --host-label 'DESCRIBE THE ACTUAL HOST'
```

For the extra serial check:

```sh
python3 -B benchmarks/results/milestone-4-gate-handoff/run_interleaved.py \
  --before-root BEFORE_CHECKOUT --before-executable BEFORE_BINARY \
  --after-root AFTER_CHECKOUT --after-executable AFTER_BINARY \
  --output NEW_COMPARISON --host-label 'DESCRIBE THE ACTUAL HOST'
```

Temporary Session files are removed. Rebuilds can change binary hashes; retain your own settings/hashes. Result directories must be new. Read actual outcomes/progress: a validated run that reached its cooperative budget is not a completion.
