# Reusing buffers during committed-history validation

Milestone 4 step **4.22**, framework **0.4.24**, continues [schema preparation reuse](performance-payload-preparation.md). It reduces temporary allocations inside the remaining complete-history scan. Public APIs, stored formats, native protocols, headers, payload lifetimes and validation frequency are unchanged. Integration into `main` remains held for measured improvements and qualification.

## What changes

The **committed prefix** is the sequence of accepted arena records ending at the authoritative allocation tail. A full scan validates each record's framing and the transaction **manifests** that list its committed records and redo commands. See the [journal contract](concurrency.md#redo-commands-and-append-manifests).

In [mutation_journal.cpp](../src/mutation_journal.cpp), `read_range` previously allocated a byte vector to copy at most 128 bytes of each record's fixed immutable body. It now uses a 128-byte local array, passing only the exact copied length to the same validator. A mutable-only Ring still passes an empty span. Record envelopes already used local fixed arrays; their separate atomic readiness load is unchanged.

The scan-local `PrefixReader` also reuses a private byte vector for variable-length manifest input. It checks the requested range and host-size bound before growing the buffer. Smaller reads expose only the requested prefix, regardless of retained capacity. `read_manifest` immediately decodes that span into a Manifest whose record references, commands and payloads own their data. No scratch span escapes the decode or becomes a public view.

NativeFile-based variable manifest reads remain owning vectors. Fixed-body NativeFile reads use the existing `read_exact` path, which checks ownership, actual file bounds under its mutex, and short-read errors; this removes only the preceding duplicate size check. Prefix-window geometry and bytes copied remain the same.

## Risk assessment

The [performance/correctness guidance](../AGENTS.md#performance-and-correctness) supports eliminating repeated allocation without changing the checks performed. The fixed buffer's exact-length span is essential: zero-initialized unused array bytes must never make a truncated body appear valid. Bodies exceeding format bounds, short fixed prefixes, mutable-only records and physical short reads retain their existing rejection paths.

The manifest scratch buffer retains its largest requested size/capacity until that scan ends. This trades longer temporary buffer retention for fewer allocate/free operations. Capacity grows only after checked bounds, and reserve/resize is requested for the necessary size. Large legal manifests still use heap storage; no unbounded stack buffer or assumption that a manifest fits a mapping window is introduced.

A later manifest read invalidates earlier scratch contents, even without reallocation. The sole consumer decodes immediately and retains owning values, so it never consults an earlier view after another read. Record-range exports also contain values rather than scratch aliases. Error unwinding destroys the reader and its mapping leases before recovery can truncate an unaccepted append. Scratch storage is not cached across scans, operations, recovery or another process's commit.

All alignment, range, encoding, kind, predecessor, generation, revision, readiness, manifest-membership, command and padding checks remain. Readiness is still loaded through the native atomic adapter. No Publication/receipt validation or complete scan is skipped. Borrowed payload mappings use their existing independent owners, and receive/acknowledgment/cleanup ordering remains unchanged.

## Qualification and measurements

Focused private journal tests scan committed small/large/small manifests, including a body larger than 64 KiB, verify exact smaller spans and retained range ownership, reject corrupted final padding without writes, repair and revalidate. A short declared Task fixed body must also reject without writes before repair. Existing window-crossing, zero-immutable Ring, large-manifest, Prepared rollback, corruption, interruption and repeated-recovery tests remain relevant. These are storage fixtures, not application setup; the public primer API is unchanged.

Both preserved and new executables run with the same default execution request. Elevated worktree setup is kept in a separate command. Normal timing evidence comes only from adjacent alternating serial pairs and a complete matched control matrix. Separate enabled before/after populations explain work counts. No unmatched sequential normal timing comparison is used. All reports, outcomes, execution order, source/executable/harness hashes and settings are retained. Plots use observed ranges, not confidence intervals or production-tail guarantees; CI has no timing thresholds.

The [result directory](../benchmarks/results/milestone-4-prefix-buffers/README.md) records measurements and plots. One complete prefix scan and delivery validation, decoded manifest result allocations, remaining identity/successor searches, native checks/growth and idle polling remain costs. This change reduces the cost per scanned record without making operation latency independent of retained history or removing the Session-wide mutation gate.

## Observed results

The first H128 counted calls save **468/470/471/471 allocation requests** during ID/send/receive/acknowledgment. This is **28.9%/9.6%/10.6%/10.9%** fewer requests. All three repetitions agree. Across 603 matching counted samples, allocation requests/bytes fall; send also saves two duplicate native stats. Prefix, Publication, Task, mapping, gate and recovery work remains unchanged. Scratch retention is scan-local rather than a promise of lower peak memory.

Initial five-pair complete-cycle rates change −3.49%/+2.90%/−6.01% at H0/32/128. H128 operation medians fall, but longer individual calls increase cycle totals and CPU; scheduler-only attribution is unsupported. A fixed twenty-pair follow-up retains the initial data and shows cycle-rate gains of **0.98%/2.39%/3.01%**. H128 operation p50 falls **6.26%/2.34%/2.63%/2.31%**. After cycles improve in 16/20, 20/20 and 19/20 pairs. These modest host-specific timings do not prove universal or tail-latency gains. All control cases, ranges, raw observations and the failed version-check launch are documented in the [results](../benchmarks/results/milestone-4-prefix-buffers/README.md).

Local strict Release diagnostics OFF and ON each pass **822/822 tests**. Source CI passes on Linux x86_64, Linux ARM64 and macOS ARM64. Linux's 54 diagnostic workloads all complete, including six loaded cases; its unchanged mmap/munmap totals show that mapping reuse remains a separate opportunity. The primer continues to use the same public API directly.

## Follow-up candidates

Two separate candidates remain. First, gate acquisition repeats three pathname-identity checks before and after the native lock. Omitting only the pre-lock group would save three `fstatat` calls per attempt reaching it, while retaining rejection after acquisition. The explicit trade-off is delayed resource-error detection: if an externally replaced coordination file's old lock remains held, callers could receive Busy/Timeout or wait on that obsolete lock before rejection. Stable coordination files are already required; this error-timing change still needs documentation and a contention test before adoption.

Second, committed-prefix reads already reuse `MappedReadWindow`, and `read_journal_words` maps a whole requested block rather than each word. Accepted-batch readiness loads and per-record commit stores still make individual eight-byte mappings. Bounded reuse there, and reuse across compact-command block reads, should be measured with native map/unmap counters and syscall traces. Any store-window implementation must keep native atomic acquire/release semantics, crash-checkpoint order, post-growth bounds and release-before-rollback lifetimes. Neither candidate is implemented in this step.
