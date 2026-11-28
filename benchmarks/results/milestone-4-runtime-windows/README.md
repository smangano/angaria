# Runtime metadata window measurements

Raw samples and profiler payloads described below are retained in the maintainer archive, not this checkout. Reports, plots, summaries and provenance remain here. [Archive catalog and restore instructions](../README.md#restore-archived-evidence) identify snapshot `2026-11-28-332118be2291`; restore its payloads before running the verification or figure commands below.

Step **4.16**, framework **0.4.18**. The [guide and risk assessment](../../../docs/performance-runtime-windows.md) explain the operation-local readers. The [diagnostic method](../../../docs/performance-diagnostics.md) defines operation windows, geometry, counters and incomplete outcomes.

## Provenance

The **before** normal executable is the exact retained 0.4.17 binary, SHA256 `01e60d4a21e3de1e2ae63d108aad1059c6fd723363775ad19a4df2c1fa6a9e8e`, built from `5feaaac03350acec53efb8d76aa4e2b8eda21ecc`. It was preserved before rebuilding. Its measurement checkout is merged staging `c15c43e07a1536199fafcc8c68c30e04d238612c`, with all **127 input hashes** identical to that measured source. The manifest names the measurement checkout rather than claiming the binary was compiled there. Original compiler/cache settings were retained beside it.

The **after** executables are built from `47da91cbcded7bf6c6bd160129eaccfa8de88b51`, version 0.4.18. They share **128 input hashes**: the shared reader is new, and VERSION, runtime_state.h/.cpp and mutation_journal.cpp differ among the common inputs. Later commits add results/documentation/CSV attributes only. Builds use Apple clang 21, Release `-O3 -DNDEBUG`, plus `-Wall -Wextra -Werror -g`. Diagnostics are OFF for ordinary comparisons and ON for attribution.

The local host is Apple M4, ten logical CPUs, 16 GiB, macOS 27.0.1, native ARM64 executables, with an x86_64 Python 3.10.15 coordinator. Measurements occurred November 26, 2026 in New York (November 27 UTC). Sessions use the local Data volume. No builds or tests ran during measurement. Host isolation, thermal/frequency/affinity controls and cache flushing were not used. Small studies do not qualify production tail latency.

## Populations and ordering

- [Fresh before normal](macos-arm64-before-normal/): 33 validated complete runs.
- [After normal](macos-arm64-after-normal/): 33 validated complete runs.
- [After enabled](macos-arm64-after-enabled/): 75 validated complete runs with None/Counts/Timings controls.
- [Alternating serial cost](alternating/): 30 validated complete runs, fifteen per variant; five pairs at H=0/32/128. [order.csv](alternating/order.csv) names each adjacent pair and execution sequence.
- [Hosted Linux after](linux-x86_64-after/): 54 validated complete runs, including all six loaded polling cases, plus a separate native syscall trace.

The full matrices ran sequentially: before normal, then (after builds/tests completed) after normal and after enabled. They use fresh Sessions, sixteen-message windows, four channels, eight-entry rings and unchanged history/capacity/payload/Field controls. Serial/backlog holds nine claims; concurrent thread/process cases hold two plus requested idle consumers. These cohorts must not be compared as equivalent workloads.

The additional serial study uses fifteen adjacent before/after pairs, alternating AB then BA (eight AB, seven BA pairs). Each repetition visits H=0,32,128. Order is deterministic, not randomized. It reuses the [preceding comparison harness](../milestone-4-gate-handoff/run_interleaved.py), whose hash is recorded in each manifest. The harness imports each checkout's own runner/version/metadata globals and reuses its validators and artifact writer. Whole-invocation durations in order.csv include setup/preload; they are not operation latency.

Raw reports remain in compressed JSON lines with CSV run/summary/phase tables and TOML metadata. Latency summaries use nearest-rank quantiles within a run, then medians across runs; they do not pool samples. Ratios of variant medians and medians of within-pair percentage changes are different statistics. The guide uses ratios of variant medians. Its primary ordinary-cost table uses five alternating runs per variant; the initial three-run matrices remain visible. At H=128, issue medians increase slightly in both studies, even as send/receive/ack improve substantially.

Count comparisons use the identical-shape [retained 0.4.17 enabled cases](../milestone-4-gate-handoff/macos-arm64-after-enabled/) and new enabled runs. All three matching repetitions agree for each quoted first-call counter. The before count input hashes match the preserved baseline; fresh before normal runs supply the timing comparison. The [figure](runtime-windows.svg) shows five alternating H=128 serial runs per variant on the left and first-send counted native calls on the right. Bars are observations, not confidence intervals. [Regenerate it](generate_figure.py) with matplotlib; benchmark runners require only Python's standard library.

## Linux evidence

[Linux CI run 45928936647](https://github.com/smangano/angaria/actions/runs/45928936647) produced the unchanged downloaded artifact. Its synthetic merge is `6eff86876f40307ad12228a0474fa74e5abaf12b`; all 128 inputs match the measured after source. The eighteen-case CI suite uses eight-message windows and completes **54/54** workloads, including all six three-idle-poller cases at 8/8. Timed first H=128 send agrees with local counted work: two scans/638 visits, zero independent index scans, 23 maps, 7 reads and 68 stats. Hardware, compiler, scheduling and window length differ from the Mac, so absolute timings are not a controlled platform comparison or fairness guarantee.

The separate [syscalls.txt](linux-x86_64-after/syscalls.txt) covers startup, setup, H=32 preload, eight serial messages, controls and teardown. Whole-program counts corroborate the reduction:

| Native call | Retained 0.4.17 trace | New trace |
| --- | ---: | ---: |
| getpid | 123,425 | 90,159 |
| fstat | 68,754 | 7,285 |
| pread64 | 40,914 | 2,169 |
| mmap | 20,289 | 3,206 |
| munmap | 20,268 | 3,185 |

The [preceding trace](../milestone-4-gate-handoff/linux-x86_64-after/syscalls.txt) uses matching arguments and its corresponding source. These totals include more than the measured eight-message window. Do not divide them by eight or use traced percentages/seconds as ordinary latency evidence. Remaining process checks are visible in the counts, but traced time shares do not establish their untraced cost.

## Reproduce

Build matching before/after checkouts at the recorded commits, then run each checkout's own binary and driver with a new output directory:

```sh
python3 -B benchmarks/run_diagnostics.py MATCHING_EXECUTABLE \
  --suite diagnostic --repetitions 3 --output NEW_RESULTS \
  --host-label 'DESCRIBE THE ACTUAL HOST'
```

For the additional serial check, use the existing comparison harness with both source trees and binaries:

```sh
python3 -B benchmarks/results/milestone-4-gate-handoff/run_interleaved.py \
  --before-root BEFORE_CHECKOUT --before-executable BEFORE_BINARY \
  --after-root AFTER_CHECKOUT --after-executable AFTER_BINARY \
  --output NEW_COMPARISON --host-label 'DESCRIBE THE ACTUAL HOST'
```

Temporary Session files are removed. Rebuilding can change executable hashes; preserve your own new hashes/settings. Result directories must be new. Always inspect actual progress: a validated matrix does not turn a budget-stop workload into a completion.
