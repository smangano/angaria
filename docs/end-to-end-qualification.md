# End-to-end performance qualification

Framework **0.4.38**, milestone step **4.36**, qualifies workstream step 7 of the [recorded messaging plan](recorded-unacknowledged-delivery-plan.md#implementation-steps-for-review). The benchmark compares the new recorded/unacknowledged and recorded/acknowledged connections with classic **libnanomsg 1.3.0 IPC PUSH/PULL**, on the same host and ordinary Release build. It changes no messaging algorithm or public API.

The new bindings remain private: one Process producer, one Process consumer, one outgoing Queue, one Source input and one exact Binary schema. Shared producer-log ownership, multiple exact schemas, Thread/parent lifecycles, public configuration and public runtime admission remain step 8. The current public acknowledged default and the public-API primer remain unchanged. These limits describe this qualification binding, rather than permanent framework limits.

## What is compared

| Adapter | Acceptance and processing observed by the benchmark |
| --- | --- |
| Recorded/unacknowledged Angaria | Producer-local identity issuance, committed replay record, send and acceptance lookup; independently verified consumer delivery. No application processing acknowledgment. |
| Recorded/acknowledged Angaria | The same recording and producer operations; verified delivery followed by explicit consumer acknowledgment. Acknowledgment certifies consumer progress, separately from transport credit. |
| Classic nanomsg IPC | Successful PUSH send and independently verified PULL delivery over a Unix-domain IPC endpoint. This adapter supplies neither a replay log nor a processing acknowledgment. |

Both Angaria modes preserve the accepted message log. Returning Queue credit does not reclaim its records. Acknowledged delivery does not add synchronous disk flushing. The supported failure scope remains process interruption on a running coherent local system, without a power-loss recovery promise. The different recording, progress, buffering and wait contracts remain part of the measured result; a timing ratio does not isolate a single feature's cost.

Short controls also run the legacy public acknowledged path and the retained current-log SPSC experiment in the same executable. These are historical comparison adapters, not a second public runtime or an old-format reader. Their small cohorts remain separate from the warmed-stream results and cannot supply tail percentiles.

## Workloads and order

Each attempt creates a fresh one-to-one connection. It preloads **H** messages, drains them, then sends live warmup messages and measured messages through the same connected endpoints. H means retained accepted history for Angaria and prior traffic for nanomsg. Warmup exercises the live transport; it is not merely a preloaded file. Each message contains the same checked 32-byte envelope and deterministic remaining payload bytes. Payload size includes that envelope.

The standard plan uses **16,384 measured messages after 4,096 live warmup messages** per attempt. The basic cells use H=8,192 and eight Angaria ring slots (**K=8**). Nanomsg uses its recorded socket-buffer settings; its byte-buffer capacity is not equivalent to K logical Queue credits.

| Case family | Variation from the basic 64-byte case |
| --- | --- |
| Payload | 64, 256 and 1,024 bytes; unpaced, saturated producer. |
| Offered load | Fixed releases at 50,000 and 100,000 publications/s. |
| Prior history | H=0, 8,192 and 65,536; the H=8,192 cell is shared with the basic case. |
| Credit | K=1, 8 and 64; the K=8 cell is shared with the basic case. |
| Slow application | Consumer performs 50 microseconds of simulated processing before verification and any acknowledgment for each message. |
| Paused application | Consumer pauses once for 20 milliseconds before measured message 4,096. Actual pause boundaries are retained. |
| Longer stream | 131,072 measured messages after 8,192 live warmup messages, 64 bytes, H=8,192, K=8. |
| Legacy controls | 16 messages, no warmup, 64 bytes, H=0/128; five adapters. |

Every standard case uses all six permutations of the three primary adapters: each adapter appears twice in each order position. The long case uses three rotations, and each five-adapter legacy control uses five rotations. The full plan has **257 attempts per host**. The manifest records the actual case plan, dimensions and order; reduced diagnostic runs must retain their changed plan and are not the full qualification.

The producer uses a fixed offered schedule for paced cases and spins until each release. A late producer retains the original scheduled time rather than starting a fresh timer: this exposes lateness instead of hiding queued work, often called *coordinated omission*. Saturation cases have no imposed per-message release interval. Simulated consumer work and its pause are actual application-side delays, not adjustments added afterward. Spin waiting and simulated work consume CPU and are included in the resource accounting.

These are finite streams in preallocated storage. They measure a warmed transport operating under the specified workload; they do not establish endless sustained capacity. Immutable records accumulate even when consumers keep up. Once the configured retained-log bound is reached, a later publication returns `LogFull`; log rotation and reclamation remain a later milestone. Existing full-log correctness tests qualify that boundary separately from this timed stream.

## Latency, throughput and CPU

The latency clock is `CLOCK_MONOTONIC` in each process. Workers record resolution and warmed clock-read calibration; the coordinator checks bracketed cross-process probes before START. Clock overhead is retained without subtraction, so values near a coarse clock's resolution need care.

- **Logical-start-to-delivery latency** begins before producer payload preparation and identity issuance, and ends immediately after the consumer receive returns. It includes send work, contention, Queue/backpressure delay and waiting. Payload verification and consumer processing after receipt are outside this delivery timestamp.
- **Logical-start-to-consumer-ACK latency** ends when the explicit acknowledgment call returns, after simulated application work and payload verification. Only acknowledged adapters have this metric. It does not mean that a producer performed a synchronous acknowledgment round trip for every message.
- **Scheduled-to-delivery latency** begins at the fixed offered release, including producer lateness and earlier work. Paced-case plots keep this distinct from logical-start latency. Schedule-lateness samples expose where the producer falls behind.
- **Completed measured publications/s** counts only the measured suffix. Its interval starts at the first measured producer logical start and ends after the last measured consumer verification, every producer send return, every recorded acceptance lookup and the last selected consumer acknowledgment. One publication has one delivery in this topology. The completion boundary is stronger for acknowledged Angaria than for nanomsg.
- The common-T0 whole-cohort rate, all-stream active interval and startup/warmup data remain separately retained. They are not mixed into the warmed measured rate. Short legacy controls retain their own finite-cohort interpretation.
- **Process CPU per measured publication** sums producer and consumer user/system CPU, including library background threads. The active resource window ends at each worker's own DONE. The inclusive window continues through the common FINISH barrier to include pending library work and control-tail activity. Resource capture starts at that worker's first measured operation, after its warmup. The two windows are labeled separately rather than treating CPU after producer DONE as free.
- Minor and major VM faults are retained over the same windows. They are not direct disk-I/O counts or a cache-miss measurement. Retry, backpressure, credit-wait and timeout observations remain per-adapter diagnostics; different waiting implementations can produce different counts for equivalent delivered work.

Every latency percentile is a nearest-rank percentile **within one run's measured suffix**. p99 requires at least 10,000 measured observations in that run; p99.9 requires at least 100,000. The long case therefore supplies a limited p99.9 observation per run. Those sample thresholds do not establish production worst-case guarantees or independence of correlated samples. Plots show every run's quantile, their median and full range. They never pool messages across attempts or different workloads to manufacture a tail estimate. Short controls do not report p99 or p99.9.

## Reproduce and audit

Build the optional comparison target as an ordinary diagnostics-OFF Release executable, using the [nanomsg build instructions](nanomsg-comparison.md#build-and-qualify). Also enable `ANGARIA_BUILD_STALL_STUDY` to include the retained current-log SPSC control. Instrumented runs explain work but are kept separate from ordinary wall-clock results.

The collector, [run_end_to_end.py](../benchmarks/run_end_to_end.py), requires a new output directory and a committed source snapshot. Its command-line help lists collection controls. The manifest retains exact source and binary hashes, framework version, dependency identity, compiler and flags, architecture, CPU/host metadata, case plan and all attempted commands. All three primary adapters on a host run in the same job; results from separate hosts remain separate datasets. A Linux versus macOS difference is not evidence of one specific kernel, cache, CPU or synchronization cause.

Every attempt retains its original process output, control events, worker reports and lossless binary timing samples in a separate compressed archive. The small published report is derived data. Reconstruction checks archive hashes, planned order, control barriers, worker identities/counts, exact cross-role sequences and hashes, timestamp ordering, acknowledgment selection, resources and recomputed metrics before plotting. The compact sample files are an evidence format, not an Angaria message-log format. Failed or incomplete attempts remain in the status record; an incomplete qualification cannot silently omit them and publish a headline comparison.

To reconstruct retained evidence and redraw all figures:

```sh
python3 -B benchmarks/end_to_end_report.py path/to/retained-results
python3 -B benchmarks/end_to_end_report.py path/to/retained-results --validate-only
```

`comparison.csv` and `qualification-summary.json` retain every attempt value for each identical case/profile/metric. Figures cover delivery and completed throughput first, followed by retained history, offered load, backpressure, process CPU/faults, longer-stream tails and short legacy controls. PNG and SVG versions use the full attempt range, with no timing-based outlier removal. The retained result index records measured findings and links to each native host's plots. Hosted wall-clock outcomes are reported evidence, rather than noisy pass/fail speed thresholds; deterministic work and correctness checks remain CI gates.
