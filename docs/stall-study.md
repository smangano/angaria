# Stall diagnostics and the recorded SPSC experiment

Step **4.29**, framework **0.4.31**, studies the independent-process stalls exposed by the [first IPC comparison](nanomsg-comparison.md). It adds precise gate-acquisition observations and a private current-log experiment using a **single-producer/single-consumer (SPSC)** connection: one writer and one reader own each ring. This is an opt-in benchmark, not an admitted public delivery mode.

## What is implemented

The existing acknowledged path keeps its admission, journal, receipt, recovery, and waiting behavior. Diagnostic builds can now distinguish time in the process-local mutex acquisition, time in native `flock` acquisition, and time holding the native mutation gate. Native attempt counters distinguish blocking acquisition from nonblocking probes. Nonblocking acquisition time is syscall/probe cost, not proof of blocking. Gate-held intervals overlap ordinary phases and must not be added to their exclusive times.

The prototype keeps `next_publication_id()` and `QueueWriter::send()` as the existing serialized log authority. After acceptance, the producer exposes the committed Publication reference through a separate mapped SPSC ring. The consumer checks committed metadata and obtains a `MessageView` without a Session gate acquisition, history reconstruction, Source issuance transaction, or application acknowledgment. Only the ring coordination uses atomics; complete publication still includes existing allocation, validation, ID issuance and log commit.

The public send still creates its normal Pending receipt. The experimental consumer never reinterprets or modifies that receipt. Consequently this experiment measures consumer coordination removal while retaining producer-side legacy costs. It does not implement the proposed final receipt-free format. Do not mix its connection with legacy Source consumption.

| Boundary | Experiment behavior |
| --- | --- |
| Acceptance | Existing committed Publication is authoritative before a ring reference can be exposed. |
| Replay | The original Session log retains schema/header/payload, producer identity, Queue order and destination acceptance. Private owning replay recovers the journal once and reads the connection's retained range. |
| Crash after commit, before exposure | Consumer catch-up can discover the committed record directly; a late ring descriptor is discarded without duplicate live delivery. |
| Consumer restart | Starts at the connection's beginning; application delivery can repeat. No exactly-once processing guarantee is made. |
| Ring capacity | Producer checks credit before issuing an ID or accepting a new publication. Credit return is resource management, not acknowledgment. |
| Payload lifetime | Borrowed logged bytes remain valid while the endpoint is alive, including ring wrap. `copy_message()` preserves the complete current header and decoding dependencies after endpoint destruction. |
| Retention | Append-only, bounded log; `LogFull` rejects new work. No reclamation, rotation, or power-loss durability is added. |

## Fixed experimental scope

One two-plane Session contains one Queue, one Source and one producer. Registry/topology remain frozen after connection creation. Native role leases enforce one experimental producer and consumer; native Task claims still govern application ownership. The ring stores checked logical references and sequences, with connection/ticket generations. Existing native ABI qualification supplies aligned, always-lock-free eight-byte acquire/release operations on supported Linux/macOS profiles.

Attachment validates the connection and captures checked schema metadata. Each endpoint reserves a read-only virtual mapping through the configured log ceiling without extending the actual file. It reads only acquired, bounded committed ranges; the existing append protocol grows backing storage before commit and never truncates committed records. Future pages are never touched before publication. This mapping technique is private to the fixed POSIX experiment and is covered by actual file-growth and interrupted-append tests. It is not a portable mapped-file API promise.

Catch-up stops at provisional records. Descriptor delivery validates permanent record readiness, exact Session/producer/schema/destination identity, record bounds, Queue sequence and ticket generation. The native message-header model is retained. Consumer-local dequeue fields belong to the live view; log replay reproduces accepted messages rather than consumer execution order.

Catch-up may deliver before a producer exposes its descriptor. A subsequent receive drains any late duplicate descriptor and returns its credit. This pilot does not promise immediate credit return for a message discovered before descriptor publication, notification fairness, or a production wait policy.

Fan-out, multiple active Source inputs, mutable topology, public profile negotiation and persistent unacknowledged consumer progress remain later work. The [recorded delivery plan](recorded-unacknowledged-delivery-plan.md) records those gates.

## Build and qualify

The study is OFF by default and requires the optional pinned nanomsg comparison dependency. Build ordinary and diagnostic executables in different directories. Install the [comparison dependencies](nanomsg-comparison.md#build-and-qualify), plus GoogleTest for qualification.

```sh
cmake -S . -B build/stall-normal -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS=-g -DCMAKE_C_FLAGS=-g -DBUILD_TESTING=ON \
  -DANGARIA_BUILD_IPC_COMPARISON=ON -DANGARIA_BUILD_STALL_STUDY=ON \
  -DANGARIA_ENABLE_DIAGNOSTICS=OFF -DANGARIA_ENABLE_CACHEGRIND=OFF
cmake --build build/stall-normal --parallel 3
cmake -S . -B build/stall-diagnostics -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS=-g -DCMAKE_C_FLAGS=-g -DBUILD_TESTING=OFF \
  -DANGARIA_BUILD_IPC_COMPARISON=ON -DANGARIA_BUILD_STALL_STUDY=ON \
  -DANGARIA_ENABLE_DIAGNOSTICS=ON -DANGARIA_ENABLE_CACHEGRIND=OFF
cmake --build build/stall-diagnostics --target angaria_ipc_comparison --parallel 3
build/stall-normal/angaria_recorded_spsc_tests
python3 -B test/stall_study.py build/stall-normal/angaria_ipc_comparison \
  build/stall-diagnostics/angaria_ipc_comparison
```

The prototype tests cover real producer termination after commit and during a provisional append, owning replay, restart duplicates, ring wrap/credit, retained borrows/copies, ownership and stale/malformed references, and log exhaustion. The study script qualifies real process exchanges and separate observation scopes, including empty controls and invalid inputs. It imposes no machine-speed threshold. The seventeen public primer programs were reviewed and remain unchanged; experimental methods are not taught as implemented public Angaria APIs.

## Collect latency, throughput and diagnostics

```sh
python3 -B benchmarks/run_stall_study.py build/stall-normal/angaria_ipc_comparison \
  --diagnostic-executable build/stall-diagnostics/angaria_ipc_comparison \
  --output build/stall-results --host-label 'describe this host and execution permissions'
```

The destination must be new. The default ordinary concurrent study records **120 attempts**: 64/256-byte payloads, H=0/128, sixteen measured messages, five balanced forward/reverse blocks, and ten runs per profile/cell. Profiles are current recorded/acknowledged Angaria, experimental current-log recorded/unacknowledged SPSC, and classic unrecorded/unacknowledged nanomsg IPC. Angaria rings have 32 slots in this study. Their control/retention guarantees differ, so a timing ratio cannot isolate acknowledgment or atomic-instruction cost.

Twelve separate ordinary backlog controls let the producer finish before starting the consumer, with enough ring capacity for the entire cohort. They isolate interference from an empty waiting consumer. Scheduled latency deliberately includes this delayed consumer start; these controls are not pooled with concurrent performance.

Eighteen separate diagnostic attempts use 64-byte messages at both history values, three repetitions of concurrent, backlog and empty-wait scenarios. Empty controls stop after a 100 ms data budget and retain zero-delivery rates. Setup/history uses at least a ten-second budget, independently of the short data phase, within the coordinator's lifetime timeout. `--diagnostic-collection counts` provides an optional lower-overhead observation control. Instrumented observations are never substituted for ordinary latency/throughput results.

Reports retain the existing independent wire/hash and monotonic-clock checks, raw process/control output and reconstructed aggregates. Each scenario/collection has its own manifest, reports and summary; source hashes include the native library, headers, prototype and study driver. Diagnostic CSVs report operation totals, phase counts/inclusive/exclusive time and work counters. Allocation interception is not installed in this driver; allocation-counter zeroes are not allocation measurements. Per-operation CPU is the collector's thread CPU; common FINISH resource totals include whole worker processes and library background threads.

Lead interpretation with ordinary scheduled and logical-start delivery latency, verified publications/s, complete/incomplete counts and full run ranges. Short fresh cohorts are not sustained capacity or production tail guarantees. Primary incomplete rates include the full stop interval and zero work; prefix rates remain separate diagnostics.

Apply the `study-stalls` PR label to request qualification on Linux x86_64, Linux ARM64 and macOS ARM64, plus the full Linux study. `stall-study-linux-x86_64` retains all measured attempts for fourteen days. Ordinary CI and request-only Intel Mac qualification keep their existing scope.
