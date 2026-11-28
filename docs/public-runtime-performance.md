# Public runtime performance qualification

Framework **0.4.39**, milestone step **4.37**, measures public runtime admission through the same API used by applications: `Session`, `Task`, `QueueWriter` and `SourceQueueSet`. The public adapters do not construct a private catalog, schema reference, log owner or endpoint. Registration, activation, claims, reservation and live warmup finish before the measured suffix starts.

`public-recorded` selects recorded, unacknowledged delivery. `public-acknowledged` selects recorded delivery with explicit consumer acknowledgment. Both perform public identity issuance, buffered `send`, and buffered `publication_status`; the consumer verifies the complete payload and its public `MessageHeader`. Acknowledged delivery also validates the exact `DeliveryToken` and calls public `acknowledge`. The fixed one-destination output buffer avoids adding an owning result-vector allocation to each benchmark operation. Nanomsg remains the same classic libnanomsg 1.3.0 IPC PUSH/PULL adapter.

The public status API reports acceptance and destinations; it does not expose private record offsets. Its acceptance proof is checked against the send result in the adapter. Retained binary samples keep the public lookup completion time, publication identity and independently checked payload hash. No private record references are fabricated for the public comparison.

The [step 7 methodology](end-to-end-qualification.md) still defines the clocks, same-host order, CPU windows, payload bytes, finite completion rates and workload caveats. Public admission uses its eleven main cases and longer-stream case, with **207 full attempts per host**: six orders of the three adapters for every main case and three rotations for the longer case. The public correctness smoke uses four short cases and all three adapters, **12 attempts**. The superseded public runtime and current-log SPSC experiment remain reproducible from their pinned historical source; this step does not maintain them as another current engine.

The [retained private qualification](../benchmarks/results/milestone-4-end-to-end/README.md) is the pre-admission baseline. Public and private results may be compared only for identical message counts, warmup, history, credit, payload and completion boundaries. Different host CPUs or separate runner jobs cannot establish isolated facade overhead. The public comparison labels its API surface explicitly; the old private profile names retain their historical meaning.

Configure an ordinary Release build with diagnostics and Cachegrind disabled, build `angaria_ipc_comparison`, and commit all implementation inputs before collecting:

```sh
python3 -B benchmarks/run_end_to_end.py collect /absolute/build/angaria_ipc_comparison \
  --runtime public --output /new/output --source-commit <commit> \
  --host-label "actual host and architecture"
```

`--smoke --no-plots` checks the short process/coordinator cases without a performance threshold. The on-demand public qualification workflow runs the full matrix on Linux x86_64 and correctness smoke on Linux ARM64 and macOS ARM64. The `qualify-end-to-end` and `compare-nanomsg` PR labels request that workflow. All samples, attempts and failures are retained; reconstruction checks the public surface, counts, clocks, acceptance lookup and processing boundary before plotting. Public tests additionally qualify shared Task log ownership, multiple exact schemas, Thread/parent lifecycle and the absence of a Session gate during established messaging.

## Retained public measurements

The [step-8 results](../benchmarks/results/milestone-4-public-runtime/README.md) retain **414 full attempts** across native Mac ARM64 and hosted Linux x86_64, plus **24 native ARM correctness-smoke attempts**. All complete with matching delivery/acknowledgment counts and reconstruct from retained raw samples. The result tables and plots report both strengths and regressions: public unacknowledged latency is low, but Linux throughput/CPU efficiency, longer-stream Mac tails and polling under consumer delay remain weaknesses. These are finite streams and different recording/processing contracts; no universal nanomsg speedup or unlimited-stream capacity is claimed.

A [same-job five-profile control study](../benchmarks/results/milestone-4-public-runtime/README.md#same-job-publicprivate-controls) independently measures public-layer overhead: 25 balanced attempts put public unacknowledged throughput 20.0% below its current private core and public acknowledged throughput 24.8% below its private core on that Linux runner. Repeated PID checks are a concrete next optimization candidate. These controls use the current implementation, not a retained superseded engine. The `qualify-public-controls` label requests the focused workflow; its collector validates and retains all attempts with no timing pass/fail threshold.
