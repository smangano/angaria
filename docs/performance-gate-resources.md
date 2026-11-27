# Checking stable resources after acquiring the mutation gate

Milestone 4 step **4.23**, framework **0.4.25**, follows [prefix-validation buffer reuse](performance-prefix-buffers.md). It removes one repeated group of pathname-identity checks from mutation-gate acquisition. Public signatures, stored formats and atomic protocols retain their versions. Detection of externally replaced Session resources can occur later, as specified below. Integration into `main` remains held for measured improvements and qualification.

## What changes

The **mutation gate** combines a process-local mutex with an exclusive native file lock on `mutation.lock`. It serializes cooperating Session readers and writers. The [ownership contract](concurrency.md#ownership-and-lock-order) requires coordination files to remain present and unchanged throughout the Session's life.

In [session_controller.cpp](../src/session_controller.cpp), `check_resources` checks the identities and file types of three names relative to the retained Session directory: `mutation.lock`, the `claims` directory, and `session.ang`. Each check uses `fstatat`, a native operation that inspects a named file. Acquisition previously called the group immediately before `flock` and again after obtaining that native lock.

Acquisition now goes from the existing local ownership/handoff checks to the native lock attempt, then performs the same complete resource check after the lock succeeds. No usable `MutationGuard` is returned until that check passes. Rejection releases the provisional guard's native lock and local mutex. Controller process ownership, guard ownership, Task-claim checks, controller opening, and resource checks when accessing the Session file remain in place.

The expected work reduction is three pathname-stat attempts for each acquisition attempt reaching the native lock. A try-only attempt that returns early because of the local mutex or an announced blocking request never reached the removed check and saves none. The gate's local handoff policy, native lock mode, atomic fences, journal recovery and subsequent history/delivery validation are unchanged.

## Risk assessment and error timing

The [performance/correctness guidance](../AGENTS.md#performance-and-correctness) supports assessing repeated checks against supported operating conditions. Cooperating participants do not unlink or replace coordination files. A pre-lock pathname check cannot prevent a replacement immediately after that check, so it never removed the need to reject mismatched resources after acquiring the lock. The retained check still prevents an acquisition from exposing a usable guard for a resource mismatch it observes.

The accepted tradeoff concerns diagnosis when an external actor violates the stable-resource contract. An open descriptor can still refer to an old file after its directory entry changes. If another process holds that old `mutation.lock`:

- A nonblocking attempt can return `OwnershipBusy` before inspecting the changed names. A public receive can report `ReceiveTimeout` while such attempts remain busy, rather than immediately reporting the namespace problem as `IoError`.
- A blocking attempt can wait indefinitely on the old held lock. There is no new bound on that wait or promise that `IoError` takes precedence over contention.
- If the lock is eventually obtained, the retained resource check rejects the mismatch with `IoError` before returning a usable guard. Later file-access and claim checks also retain their existing resource validation.

The three names above remain required. This change accepts delayed detection; it does not make replacement a supported way to repair or rotate a live Session. Repeated checks, including the retained post-lock check, cannot make arbitrary external namespace changes safe after a check has completed. Applications must keep the Session and its coordination files intact while participants use it.

## Qualification and measurements

One focused private ownership test replaces each checked resource while an independent process holds the old gate. It verifies a try-only Busy result, then post-lock IoError after orderly release, unchanged retained Session bytes and empty replacement resources. Restoring each original identity permits an independent process and then the retained parent controller to acquire again, checking native unlock and local ownership cleanup. Pipe handshakes establish ownership, and the test bounds its own waits and reaps workers. It does not measure blocking-wait duration. Existing ownership, fork, local-handoff, Task-claim, recovery, crash, delivery and timeout qualification remains relevant. These deliberate file changes are test fixtures, not application usage.

Before/after executables must use the same execution settings, workload and compiler options. Preserved normal builds supply adjacent alternating ordinary-call comparisons; separate enabled populations attribute native calls and acquisition phases. Complete matrices retain contention, idle polling, outcomes, source/executable hashes and all observations. Work counts and ordinary timing are separate evidence, and CI uses no speed threshold.

The **gate acquisition** phase starts before the removed checks and includes local coordination, resource checks, native locking and scheduling. It is not a pure lock-wait counter. The **native gate ownership** interval starts only after successful `flock`; the removed checks were outside it. A shorter acquisition interval therefore need not produce a shorter native hold interval. Compare both boundaries without adding overlapping timings.

Strict local Release builds with diagnostics OFF and ON each pass **823/823 tests** (18.47/18.54 seconds), including the new three-resource contention case. The [result directory](../benchmarks/results/milestone-4-gate-resources/README.md) will retain qualification, work counts and matched timing observations. Measurements and CI qualification are pending; no speedup is asserted here.

## Follow-up scope

This step targets repeated native identity checks. Remaining short mappings for accepted-batch readiness and record-commit stores, plus mapping reuse across compact-command reads, are separate [documented candidates](performance-prefix-buffers.md#follow-up-candidates). Their atomic ordering, checkpoint sequence and rollback lifetimes require their own qualification and measurements.

Cachegrind instruction/cache profiling is a separate follow-up after this syscall change. It can help locate CPU work but does not replace ordinary latency measurements, native syscall counts or correctness qualification. Complete-history validation, decoded-result allocation, idle polling and the Session-wide gate remain costs.
