# Milestone-4 macOS ARM64 baseline

Recorded 2026-11-26, framework **0.4.13**. See the [measurement guide](../../../docs/native-performance.md#recorded-milestone-4-baseline) for the workload, calculations, timings, and limitations.

| File | Contents |
| --- | --- |
| [reports.jsonl](reports.jsonl) | All 42 validated invocation reports, each with repetition number and native operation quantiles. |
| [summary.csv](summary.csv) | 94 dimension/metric groups; each row is the median of three per-run statistics. Samples are not pooled. |
| [manifest.toml](manifest.toml) | Actual runner arguments, source/executable hashes, exact measured commit, compiler/build settings, times, and runner platform metadata. |

The measured commit is `d2c728c502fc2365711f21f0f34e6980548f5293`. Its tracked files were clean throughout the run. `git_dirty=true` includes pre-existing unrelated untracked files in the shared workspace; `git_tracked_dirty=false` distinguishes those from source changes. The results commit adds documentation/data only. A later runner-only fix preserves non-BMP Unicode in TOML metadata; the native benchmark source, binary, timing, and aggregation remain unchanged. The manifest records the exact earlier runner hash, reproducible from the measured commit. Session binaries are temporary and are not included.

The host is an Apple M4 with ten logical CPUs and 17,179,869,184 bytes of memory (16 GiB), macOS 27.0.1. Those values were checked using the host's system information. The runner uses Python 3.10.15 and reports an x86_64 process environment; CPU discovery was unavailable to it. Its raw `machine`/`platform` fields are preserved rather than rewritten. Separate checks returned:

```text
uname -sm
Darwin arm64

file build/release/angaria_transport_benchmark
Mach-O 64-bit executable arm64

c++ --version
Apple clang version 21.0.0 (clang-2100.4.1.1)
Target: arm64-apple-darwin27.0.0
```

The executable hash in the manifest identifies that ARM64 binary; timings occur inside it with a monotonic clock. Python coordinates launches and summarizes results outside the timed operations. This dataset does not qualify an Intel Mac or supply comparative Intel results. No CPU affinity, frequency, power, load isolation, or cold-cache control was applied. Results came from the managed Codex execution environment.

To reproduce, build the measured commit with the recorded Release flags, then use the manifest's runner arguments with a new output directory. Step 4.12 later factors shared setup into a benchmark helper header; this historical dataset remains tied to its original snapshot. To recalculate each throughput, compute `messages * 1e9 / phase_ns` per report and take the median across the three matching repetitions. Serial uses `cycle_phase_ns`; phased uses separate publish/drain times. Latency summaries take the median of the matching run quantiles. Never treat profile mode as a public API measurement or combine independent modes' samples.
