# Scanning history through bounded mapped windows

Milestone 4 step **4.14**, framework **0.4.16**, continues the [performance investigation](performance-diagnostics-findings.md) after [validated-prefix reuse](performance-prefix-reuse.md). It replaces repeated tiny native reads and mappings inside a committed-prefix scan with a scan-local **64 KiB mapping window**. The public API, mapped format, and synchronization protocol are unchanged. Integration into `main` remains held for measured improvements and qualification.

## What changes

The **committed prefix** is the retained arena ending at the authoritative allocation tail. Each record has an envelope, an atomic readiness word, immutable metadata, and any mutable storage. A scan checks every record and the manifests that describe completed append batches.

Previously each visited record required two envelope reads, a tiny readiness mapping, and a bounded metadata-body read; manifests required another read. Those calls repeatedly checked file size and process ownership and created/destroyed native mappings.

A private PrefixReader now keeps one bounded mapping while the scan traverses that region. It copies only the requested immutable metadata and loads readiness through `AtomicWord`. Copies crossing a window boundary refill the window in pieces; manifests larger than 64 KiB remain supported. The last window ends at the scan's tail. Only owning metadata copies leave the scan.

The same envelope assembly and validation code serves the window reader and the ordinary NativeFile reader. Accepted-batch validation, append materialization, RuntimeAccess reads, payload mappings, and the general NativeFile API keep their existing paths. A healthy operation still performs two full-prefix scans and no separate runtime index scan; this change reduces their native work rather than skipping validation.

## Risk assessment

Under the [project guidance](../AGENTS.md#performance-and-correctness), the native-call cost is weighed against the failures each repeated check can prevent.

- Supported Session mutations use the held gate. Rechecking ownership and physical size for every immutable slice inside the same scan adds substantial cost without a supported intervening writer. Journal entry and each new mapping still perform the ordinary native checks. There is no cache between scans or API calls.
- The mapping is bounded to at most 64 KiB of logical file bytes, rather than reserving an arbitrarily large contiguous address range for the entire log. Its first page can include bytes before the arena, but reader bounds prevent accessing them. A refill clears borrowed spans before releasing their owner.
- Recovery scans map only their base prefix, never provisional EOF. Their mapping leases end at or below that tail and disappear before rollback can shrink an unaccepted append. Payload views retain their independent ownership and pins.
- A mapping may virtually cover payload pages between metadata requests; the scanner does not inspect or copy those payload bytes. Live readiness is never copied through ordinary byte reads.
- Metadata scanning now uses mapped-I/O behavior for more bytes. Unsupported external truncation after a mapping is created, or an OS mapped-page I/O fault, can fault the process rather than returning a `pread` error. Existing readiness/payload mappings already depend on that native failure model. Repeated `fstat` cannot make arbitrary external truncation safe. This is an explicit tradeoff for fewer native calls, not a promise of hardware-failure containment.

The general NativeFile adapter retains fresh-size checks, independent handle limits, alias/shrink leases, and fork ownership rejection. Its adversarial external-truncation tests remain unchanged. Supported process-death recovery, stale plans, corruption rejection during normal validation, and rollback are qualified separately from unsupported concurrent file tampering.

## Qualification and measurement

Two new private journal tests cover a Task body crossing a window boundary, a manifest larger than one window, corruption beyond the third window, a partial final window, and actual FileGrowth failure followed by rollback and retry. Their framing fixture is not application topology setup. Existing atomic ordering, metadata-only payload inspection, NativeFile, guard/fork ownership, payload lifetime, recovery, and deadline tests remain relevant.

The unchanged diagnostic workload compares fresh normal Release runs before and after the change. Enabled runs explain actual native calls and allocation requests. The exact pre-change executables are preserved before rebuilding. Source/executable hashes, compiler flags, geometry, outcomes, and raw operation samples accompany the results. Small sequential runs describe this host; they do not qualify production tail latency. CI checks correctness and records observations without timing thresholds.

The PR records measured results after qualification. The remaining work still scales with retained history, and the separate polling/gate handoff problem remains to be addressed. Further optimization of RuntimeAccess metadata reads is outside this focused scan-reader step.
