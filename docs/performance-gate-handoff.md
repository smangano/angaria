# Giving queued operations a local gate handoff

Milestone 4 step **4.15**, framework **0.4.17**, addresses the polling stalls retained in the [diagnostic findings](performance-diagnostics-findings.md) and [mapped-window measurements](performance-prefix-windows.md). It gives an announced local blocking operation an opportunity to take the mutation gate before nonblocking polls reacquire it. The public application interface, mapped format, and synchronization protocol retain their current versions. Milestone integration into `main` remains held for measured improvements and qualification.

## Why handoff matters

`Source::receive` uses nonblocking gate attempts so a deadline does not wait indefinitely for ownership. When an empty Source keeps retrying, its active thread can repeatedly win the controller's process-local mutex while a producer's blocking acquisition waits to be scheduled. The earlier Linux trace records a producer waiting about five seconds while the consumer acquires the gate 336 times. Faster metadata work reduced each visit but did not resolve the loaded polling cases.

A **blocking request** now announces itself just before attempting the local mutex. The controller counts these requests in process-local atomic state. A try-acquire checks that count before allocating a guard and again after winning the mutex. If a blocker is announced, it returns `OwnershipBusy`, leaving a handoff opportunity. The second check covers a blocker arriving between the first check and the actual mutex attempt.

The announcement ends once the blocker owns the local mutex; that ownership then excludes polls while resource checks and native locking proceed. RAII withdraws an announcement on errors, exceptions, and lock failures. Guard allocation happens before announcing, reducing the interval in which a request reserves preference without being ready to lock.

The count belongs to the shared controller for this Session in this process. It is not persisted. Native `flock`, ordering fences, thread-affine guards, recursive-entry rejection, process ownership checks, and recovery remain the existing mechanisms. A private observer coordinates deterministic qualification; application calls supply no observer.

## Risk assessment

The [project guidance](../AGENTS.md#performance-and-correctness) gives performance and correctness equal priority. This small admission rule targets the measured overtaking problem without adding a persistent queue or changing native lock ownership.

- Preference belongs to announced blocking requests. A try caller can receive Busy while the mutex is momentarily free, including when the announced thread is descheduled. A finite receive can therefore still time out.
- Sustained blocking demand can delay try-only consumers. This is a class preference, with no FIFO ordering among blockers, starvation-free scheduling, cross-process fairness, or hard response-time promise. Bounded workloads must be measured in both ordinary and loaded cases rather than inferring fairness from one run.
- Returning Busy before checking native resources matches the existing contention paths: resource errors are checked once an attempt can proceed. Fork ownership is rejected before touching the inherited counter or mutex. Successful local acquisition withdraws the count before later native/resource failures, and RAII covers earlier failures.
- Uncontended blocking calls add an atomic increment/decrement, and try calls add counter checks. These use sequential consistency for a straightforward ordering argument. Measurements compare ordinary-call cost as well as progress under polling load. The live count is bounded by simultaneous local acquisition attempts; additional overflow machinery is not warranted by that failure model.

The change preserves exclusion and recorded transaction semantics. It deliberately changes local admission preference, so Busy/Timeout results can occur in an otherwise free interval. An already started operation still finishes consistently even when its caller's work budget expires.

## Qualification and measurements

Three new private controller tests force a free-mutex handoff, the first-check/lock race, and announcement cleanup after returned errors and exception unwinding. Their handshakes establish ordering; deadlines only detect a broken test protocol. Existing tests cover recursive acquisition, guard moves/lifetimes, wrong-thread/controller/fork use, native exclusion, resource failures, and process death.

The unchanged public diagnostic workload compares preserved 0.4.16 normal executables with the new normal build, then uses enabled counts/timings to locate remaining acquisition delay. Fresh Sessions, matched geometry and active claims, three repetitions, raw partial progress, and the [existing measurement method](performance-diagnostics.md) are retained. Serial/backlog and independent-process controls distinguish local handoff effects from unrelated work. Small sequential studies do not qualify production tail latency. CI records observations without timing thresholds.

Measured results and exact provenance are recorded with the PR after qualification. Native metadata access and history-dependent validation remain separate optimization candidates.
