# Fork-aware messaging ownership checks

Framework **0.4.40**, milestone step **4.38**, replaces repeated hot-path PID queries with retained process tokens. The public Message/Task/Queue interfaces and native format 3.0/synchronization 2 retain their contracts. [Public runtime admission](public-runtime-admission.md) defines the supported fixed-resource and process-lifetime model.

## Reason for the change

The [public-admission controls](../benchmarks/results/milestone-4-public-runtime/README.md#same-job-publicprivate-controls) established a public-layer throughput cost on one Linux host: 20.0% unacknowledged and 24.8% acknowledged relative to the current private core. A source audit found six direct `getpid()` calls in the ordinary ID/send/status/receive path and ten with acknowledgment, before extra polls or retries. Those checks were layered independently. The file/mapping diagnostic counters did not count them.

A process ID identifies the process that opened a handle. Checking it protected against a child using its parent's inherited handle after `fork()`. Rejection has to precede a process-local mutex or atomic shared-pointer operation: another parent thread might have held its internal lock at fork. The cost of repeatedly asking the kernel for a PID was unnecessary given the existing fork-invalidation mechanism.

## Retained process identity

A **process token** is a private value bound during Task attachment or standalone producer-log reader opening. It retains the address of the child-invalidation flag in the existing process-lifetime coordination registry. Binding initializes that registry and verifies fork-handler registration before exposing the token or mapping reader data. A default token is invalid. Failed registration fails binding.

The registry already closes and invalidates inherited coordination descriptors in its child fork handler. That same handler invalidates every inherited process token before child application code resumes. An inline established ownership probe reads the retained pointer and child-invalidation flag, without a syscall, allocation, local lock or lazy initialization. The registry outlives all handles; a token allocates no per-reader descriptor and conveys no exclusive Task claim.

Task checks retain attachment, Thread-affinity and parent-claim validation. Source checks reject the wrong process/Thread before loading their atomic Task pointer, then retain and validate the Task for the operation. Buffered and owning sends each validate once at their boundary, before allocation or the producer mutex. Standalone readers keep their own process/Thread guard. Native exclusive claims remain the authority for Task/log/progress ownership.

Numeric PIDs continue to be recorded during administrative attachment and checked by cold native-resource operations. Source destruction can still relinquish a reservation and release a pin on another thread in the same process, without acknowledging processing. Inherited child destruction leaves the parent's bookkeeping and claims untouched.

## Supported scope and risk assessment

The supported process model uses registered `fork()` handlers and requires `exec` before a child attaches fresh Angaria native resources. Kernel fork/clone calls that bypass those handlers are outside that model; native coordination descriptors already rely on the same handlers. An ordinary static cached PID would not provide the required invalidation. Reusing the existing registry avoids a second fork protocol or extra per-handle native resource.

This is a runtime-only change. It changes neither stored headers nor replay/acknowledgment authority. The performance tradeoff removes repeated kernel queries while retaining the early rejection and ownership guarantees of the supported process model.

## Verification

Fresh-executable tests prove token initialization and fork invalidation even when no coordination descriptor has previously opened. Standalone readers reject inherited use, preserve the parent's reader and native claim, and retain Thread binding. Public tests fork while another thread holds the actual producer and Source mutexes; the child must return `WrongOwner` before either lock is released. A child-only empty Task pointer also checks that process rejection precedes Task validation.

On Linux, the isolated public performance executable links with `--wrap=getpid`. A positive control verifies two actual libc calls are observed. Warmed buffered and owning sends/status, ID issuance, receives, repeated pins, release/redelivery, acknowledgment/watermark and 10,000 empty polls per mode must make zero direct `getpid()` calls. This intercepts references from the static Angaria library; it does not claim to observe every syscall inside dynamically linked dependencies. Existing allocation, file/mapping, Session-gate, crash/recovery and connection-independence guards remain active.

Ordinary benchmark binaries have no PID wrapper or runtime diagnostics. Matched baseline/head runs use separate frozen builds on the same host, balance execution order and retain every raw attempt. Head qualification also compares both recorded modes against nanomsg across the complete workload matrix. Expected direction is less public ownership overhead, especially on Linux; actual latency, completed throughput, CPU, ranges and regressions must be reported before integration.

## Measured result

The [retained measurements and plots](../benchmarks/results/milestone-4-fork-ownership/README.md) contain 216 paired attempts and 621 separate full-workload attempts with no discarded runs. Final inline confirmation compares 0.4.39 directly with 0.4.40 on each host. On the final AMD Linux runner, median paired throughput improves 102.9% unacknowledged and 209.2% acknowledged; delivery-p99 revision medians fall from 7.291 to 4.522 µs and 42.859 to 17.707 µs. Nanomsg's paired throughput remains effectively unchanged. An earlier Intel Linux cohort also improves, by 28.0% and 58.0%, with the initial non-inlined probe. The differing hosts prevent attributing the difference between those experiments to inlining.

Mac changes are smaller and noisy: final local M4 pairs show +6.4% unacknowledged and −0.6% acknowledged; the hosted M1 VM shows +3.5% and +2.6%, with a larger positive nanomsg control shift. The report retains the initial hosted Mac −21.4% unacknowledged result and its analysis. It does not claim a clear Mac-wide gain. The 621 full-workload attempts qualify the initial implementation; final inline confirmation repeats the core64 pairs and Linux PID guard. Larger-payload and slow-consumer limits remain explicit in the report.

All 752 local strict Release CTest entries pass after inlining. Linux x86_64, Linux ARM64 and macOS ARM64 CI pass. The new held-lock regression uses portable `std::thread` cleanup because the macOS CI C++ library does not provide `std::jthread`/`std::stop_token`; this test fix changes no runtime behavior. The final Linux positive-control and zero-hot-PID-call checks also pass.
