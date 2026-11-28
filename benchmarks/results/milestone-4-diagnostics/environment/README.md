# Original-binary environment and stack checks

These checks use the exact step-4.11 **0.4.13** ARM64 executable, SHA256 `aa4f8b6548e0fb8d0706742a0f27bf64440763544f416df5d56171152c2d96e0`, built from `d2c728c502fc2365711f21f0f34e6980548f5293`. No new internal instrumentation is present. [manifest.toml](manifest.toml) records arguments, provenance, and the plain-text stack report's hash.

The machine is the same Apple M4/ten-logical-CPU/16-GiB/macOS-27.0.1 host as the earlier baseline. Experiments occurred on November 26, 2026, local New York time. CPU frequency, affinity, thermal state, host load, and caches were not controlled. Three managed runs preceded three approved direct-host runs; the order was not randomized. The Python coordinator runs as x86_64, while the timed executable and successful sampler are ARM64.

| Context | Three run send-p50 values, ms | Median of run p50, ms |
| --- | --- | --- |
| [Managed execution](managed.jsonl) | 5.491, 5.180, 5.301 | 5.301 |
| [Direct host](direct-host.jsonl) | 5.059, 5.129, 5.421 | 5.129 |

Each run uses 128 measured messages, 16 warmups, one serial producer, and an eight-byte uint64 payload. The distributions overlap. This small comparison gives no evidence of a large environment-induced slowdown; it does not establish identical environments or a precise causal difference.

The [whole-program resource report](resource-usage.txt), from `/usr/bin/time -l`, describes another successful run with the same arguments. Its [benchmark report](resource-usage-report.json) verifies 128 completions. It recorded 3.22 s real time, 0.70 s user CPU, 2.34 s system CPU, **627,603 page reclaims**, and 59 page faults. These OS counters include initialization, warmup, work, and teardown. They are not per-operation allocations or storage-device traffic measurements.

The [archived compressed native stack report](../../README.md#restore-archived-evidence) samples a separate 384-message/16-warmup serial run for five seconds. The process completed all 384 messages. Its 3,372 main-thread observations include setup/warmup and multiple public operations, rather than isolating send. The sampler's collapsed top-of-stack table reports:

| Top frame | Observations | Share of main-thread observations |
| --- | --- | --- |
| `fstat` | 884 | 26.2% |
| `pread` | 797 | 23.6% |
| `__mmap` | 607 | 18.0% |
| `__munmap` | 457 | 13.6% |
| Combined four frames | 2,745 | **81.4%** |

These are sampled instruction locations, not exact function CPU durations or syscall counts. The call tree places those operations beneath committed-prefix scans, record indexing, topology validation, and progress operations. Allocation-related frames occur too, but this profile gives priority to repeated native metadata/mapping work. Detailed counters and controlled history experiments provide the corresponding per-operation evidence.

The first Instruments launch attempt never entered the benchmark, and an x86_64 sampler returned no usable stacks. Neither is used as evidence. Explicitly selecting the ARM64 sampler produced the retained call graph:

```sh
arch -arm64 /usr/bin/sample BENCHMARK_PID 5 1 -mayDie -file sample.txt
```

Launch a sufficiently long original benchmark in another terminal and sample only its owned PID. To read the retained report, use Python's `gzip` module or `gzip -dc`. Reproduce the binary from its recorded source/flags before comparing a later implementation.
