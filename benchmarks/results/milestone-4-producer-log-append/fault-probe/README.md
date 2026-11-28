# Supplemental first-touch fault probe

This independent diagnostic follows the [frozen append/replay study](../README.md). Its three invocations are **not pooled with that baseline**: natural, physical-prefault, natural, with **one prefault invocation**. Each invocation runs nine history/payload cases and five 64-append blocks per case. H is the accepted-record history before the first block.

The copied test adds `getrusage(RUSAGE_SELF)` immediately before and after each append block, outside its clock interval. Its raw JSON pairs `prepared_append_ns_samples` with `probe_append_minor_faults` and `probe_append_major_faults`. The normal Release static library is unchanged; the probe executable is separate.

| Invocation | Minor faults, 45 blocks | Major faults, 45 blocks |
| --- | ---: | ---: |
| Natural 1 | 3 | 110 |
| Physical-prefault 1 | 113 | 0 |
| Natural 2 | 3 | 110 |

The physical-prefault mode touches one byte per **16,384-byte native page**, only in the uncommitted reserved data area, through a temporary mapping before writer open and timing. It preserves accepted records and header bytes. The temporary mapping closes before writer open, so faults establishing the writer's later page-table entries remain possible. Prefault setup cost is excluded from these measurements.

For example, at H=0 the second natural invocation's append medians are 414.719 ns (64-byte payload), 810.547 ns (256 bytes), and 510.422 ns (1,024 bytes). The single prefault invocation records 61.188, 72.266, and 117.828 ns respectively. Every raw block is retained in the logs and [parsed data](parsed.json); [analysis.txt](analysis.txt) lists all case medians and fault totals.

These observations support a material first-touch VM/backing cost in this fixture. They do not isolate all cache, scheduling, or filesystem effects, provide confidence intervals, or establish an end-to-end transport speedup. Darwin's `ru_majflt` classification alone does **not prove physical disk I/O**. Touching the reservation is an experimental setup contrast, not a public prefault policy or a change to ordinary runtime behavior.

The [manifest](manifest.json) records the frozen source commit, compiler flags, native page size, unchanged original test hash, normal-library hash, run order and payload hashes. [source.sha256](source.sha256) and [original-build-command.txt](original-build-command.txt) preserve the original probe capture; the former uses the original local source paths and `fault_probe.cpp` name. The archived copied source is deliberately named [fault_probe.cpp.txt](fault_probe.cpp.txt), so retaining evidence does not add another tracked C++ implementation input. [probe.patch](probe.patch) shows its complete change from the frozen test. No binaries are included.

To reproduce, use the recorded source commit, build a normal Release archive with diagnostics disabled as described in the parent README, and copy this supplemental directory into that checkout. Run [build-command.txt](build-command.txt) from the repository root; it copies the archived text to a temporary `.cpp`, links the existing normal archive, and runs the same sequential contrast. The `/usr/local/lib` GoogleTest paths are those of this macOS host. Keep new outputs separate from the retained evidence.
