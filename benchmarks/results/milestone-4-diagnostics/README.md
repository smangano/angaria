# Milestone 4 diagnostic evidence

Framework **0.4.14**, step **4.12**. Read the [findings](../../../docs/performance-diagnostics-findings.md) and [method/reproduction guide](../../../docs/performance-diagnostics.md) before interpreting timings. Measurements were made November 26, 2026 in New York; UTC metadata crosses into November 27.

## Retained populations

| Directory | Source commit | Validated runs | Outcome |
| --- | --- | ---: | --- |
| [Normal macOS ARM64](macos-arm64-disabled/) | `d2b84634bfee781818481f39c22bfbc9ac9a6b58` | 33 | 30 complete workloads; 3 budget stops, all three-idle-poller stream cases. Diagnostics OFF, collection None. |
| [Enabled macOS ARM64](macos-arm64-enabled-bounded/) | `d2b84634bfee781818481f39c22bfbc9ac9a6b58` | 75 | 69 complete workloads; 6 budget stops, all three-idle-poller stream cases. None, Counts, and Timings controls. |
| [Hosted Linux x86_64](linux-x86_64/) | `bfb7b6c8deee797014d4786cc771a9dd9216485e` (PR merge of `d2b8463`) | 54 | 43 complete workloads; 11 budget stops. Enabled build, 8-message CI windows, None/Timings controls. Includes an additional whole-program syscall trace. |
| [Original executable checks](environment/README.md) | `d2c728c502fc2365711f21f0f34e6980548f5293` | Six timing runs plus resource/stack checks | Framework 0.4.13, exact original executable, no internal instrumentation. |
| [Initial interrupted matrix](macos-arm64-enabled/) | `27b47ccfba0fa8d73a33d16bda34b7aaf778e219` | 63 completed workloads retained | Archived failed matrix: the first three-idle-poller stream exceeded the original 30-second runner timeout. No raw report exists for that killed workload. This is not a completed study. |

The final matrices use the same measured code snapshot. Subsequent PR commits add documentation/results only. Their manifests record tracked-clean source, complete input/source hashes, executable hashes, compiler/optimization flags, actual binary format, temporary Session filesystem, exact case/worker arguments, and repetitions. `git_dirty=true` records unrelated pre-existing untracked workspace copies and untracked result directories; `git_tracked_dirty=false` is the relevant source state. Those unrelated copies were not compiled or hashed as inputs.

The Linux checkout is GitHub's synthetic PR merge of base `dbbaa66e904788557878811ccedccf7c1a016370` and head `d2b84634bfee781818481f39c22bfbc9ac9a6b58`. All **127 recorded measurement input hashes match** the local measured head. The runner is hosted Linux x86_64, AMD EPYC 9V74, GCC 13.3, Release, with Sessions under `/tmp`; see its manifest for kernel/filesystem/flags. Its CI suite has 18 cases repeated three times, with eight messages per window, rather than the local 16-message matrix. Do not compare absolute times as if these were equivalent hosts/windows.

[Linux run 45915602783](https://github.com/smangano/angaria/actions/runs/45915602783) generated the artifact `native-diagnostics-linux-x86_64`, downloaded unchanged here. Its [syscalls.txt](linux-x86_64/syscalls.txt) covers setup, 32 historical messages, 8 measured serial messages, controls, and teardown in a **separate traced invocation**. [syscall-profile-report.json](linux-x86_64/syscall-profile-report.json) verifies that invocation's results. Tracing changes execution time; its counts are more useful here than its timing percentages. The workflow records the exact `strace -f -c` command.

The local host is Apple M4, ten logical CPUs, 16 GiB, macOS 27.0.1, native ARM64 executable, Apple clang 21, Release `-O3 -DNDEBUG` plus `-Wall -Wextra -Werror -g`. The Python 3.10.15 coordinator is x86_64. Sessions reside on the local Data volume. No CPU affinity, power/thermal controls, isolated-host load, cache flushing, or randomized case order was used. Absolute times are observations of this host and workload.

Each final matrix retains:

- `reports.jsonl.gz`: full raw reports, operation wall/CPU intervals, exact/bounded history position, phase visits/times, native/allocation counters, actual progress, and complete idle-actor aggregates.
- `runs.csv`: every invocation, including zero-progress outcomes. Accepted M, completed C, pending M−C, and borrowed-but-unacknowledged U remain separate.
- `summary.csv`: median of per-run statistics for matching case/ownership/outcome/progress/operation/result. Per-run quantiles use nearest rank. Different histories and incomplete outcomes are not pooled.
- `phases.csv`: aggregated inclusive/exclusive phase observations. Exclusive phases sum to root wall time within a captured call. Inclusive values overlap.
- `manifest.toml`: provenance and commands, including the two-worker process roles.

Complete raw storage for idle polling is bounded. If filled, latency rows are labeled `captured_prefix`; complete per-actor aggregate counters/times still include every poll. A valid `budget_exceeded` result is a measurement, not a correctness failure or successful full-workload throughput. The five-second cooperative work budget is checked between calls, so settlement/cleanup can extend elapsed time.

## Reproduce and inspect

Build matching normal/enabled Release executables from the measured commit using the flags above, then run:

```sh
python3 -B benchmarks/run_diagnostics.py build/measure-normal/angaria_transport_diagnostics \
  --suite diagnostic --repetitions 3 --output NEW_NORMAL_RESULTS \
  --host-label 'DESCRIBE THE ACTUAL HOST'
python3 -B benchmarks/run_diagnostics.py build/measure-diagnostics/angaria_transport_diagnostics \
  --suite diagnostic --repetitions 3 --output NEW_ENABLED_RESULTS \
  --host-label 'DESCRIBE THE ACTUAL HOST'
```

Every output directory must be new. Manifests list every executable argument; the process case is coordinated by the Python runner rather than invoking the executable's default serial role. Startup/history preloading is outside measured operation intervals but can dominate total study time. Do not casually use the maximum history bound for a full matrix: the current implementation does repeated work while constructing that history too.

The [figure generator](generate_figure.py) reads retained raw data and requires matplotlib. The experiment runner itself requires only Python's standard library. Its output [history-cost.svg](history-cost.svg) compares the ordinary build's per-run median latencies and exact first-send native work. Ranges show the minimum/maximum of three run p50 values, not confidence intervals. It does not combine the historical uint64 workload with the new fixed-ASCII cases.

The archived initial matrix retains its original failed manifest and data without rewriting them into a success. The tool subsequently learned to report consistent partial outcomes; runtime algorithms and locking behavior did not change.
