# Reserved delivery progress and claim cleanup

Milestone 4, step 4.5, framework **0.4.6**, implements private progress operations for the core format **2.2/`0x7`** profile. They build on [history validation](native-validation.md) and [committed payload mapping](native-storage.md#committed-payload-storage-and-append-planning). Step 4.6 now enables [public direct send and status](direct-publication.md), core Session creation, and the fourteenth [primer lesson](primer.md#14-send-directly-and-look-up-acceptance). Step 4.7 now exposes [public receive, acknowledgment/release, and Source close](native-receive.md) with bound payload views and the fifteenth primer lesson.

**Reserved progress** means updating the mutable receipt and fixed runtime rows that already exist, without appending another record. These operations use the journal's FixedState command area. Acknowledgment can complete when the publication log is full; it does not reclaim history or make append space reusable.

## Private receipt operations

The [progress primitives](../src/delivery_progress.h) take a recovered, gate-bound `RuntimeAccess` and a captured claim or delivery token. A future public endpoint must first authenticate its native Task owner. The primitives check the core capability, Session identity, current attached Task incarnation, Thread parent context, complete topology/history, and Source ownership under the guard.

| Operation | Effect |
| --- | --- |
| Issue | Select the chosen Queue's oldest nonterminal Pending receipt, assign a new Source issuance, and install matching Issued receipt and Source pin. Retained history supplies a delivery even when its ring entry has been overwritten. |
| Repeat issue with an existing pin | Return that exact token, message reference, and dequeue time unchanged, before considering fresh Queue selection or exhausted counters. |
| Acknowledge | Require the exact current token, mark its receipt Acknowledged with explicit timestamp presence, advance the contiguous Queue watermark/oldest-pending link, record the last acknowledgment time, and clear the Source pin. |
| Release | Require the exact current token, restore Pending, preserve its historical consumer/issuance/dequeue fields, and clear the Source pin. Queue acknowledgment progress stays unchanged. |
| Release Source | Release its current issuance for the exact current claim, or succeed without mutation if it is idle. Preserve its issuance counter and consumer context. This is the reserved cleanup foundation used by step 4.7 public close and deferred-cleanup ownership. |

A delivery token includes Session, consumer Task/incarnation, Source, Queue, Queue sequence, and issuance. A stale token cannot acknowledge a redelivery. Closing/reopening within a claim preserves its Source counter. A new claim starts a new issuance context on its first fresh issue, while retained receipts keep the older context. Counters reject exhaustion rather than wrapping. Epoch-zero and negative UTC times are valid because presence flags, rather than timestamp values or wall-clock ordering, describe the recorded events.

Ordinary progress uses absolute WriteRange commands. Issue needs 152 encoded bytes, acknowledgment 184, and release 88, all within the 1,792-byte inline limit. The full command stream and every target are validated before mutation; recovery reinstalls absolute values after any partial word application. No progress operation spills into the append arena. A successful private operation updates its gate-bound generation so a caller can issue and then settle that issuance under the same guard. An unresolved interrupted result requires recovery before further progress.

## Publication-aware ReleaseClaim

The compact [ReleaseClaim](concurrency.md#redo-commands-and-append-manifests) frame remains 32 encoded bytes: a command header plus the ending incarnation and release time. The admitted core capability selects its publication-aware execution; legacy Sessions retain their Task-only zero-progress checks. ResetBudgets, LoseRange, and Retry execution remain unsupported.

Cleanup finds receipts using their retained consumer identity and finds Source ownership through committed immutable definitions. It restores matching Issued receipts to Pending before clearing Source slots, preserving historical receipt fields and Source counter/context. Already Pending and terminal receipts need no state change. It also marks the ending Task detached. Releasing a Process claim includes Thread claims tagged with that captured parent incarnation; a different parent incarnation is outside that cleanup.

Replay never depends on a Source pointer that an earlier pass may have cleared. It also recognizes the immediate successor Task incarnation while finishing a replacement's absolute attachment writes. Complete intent validation rejects writes that could erase the definitions or old identities used for discovery. Publication metadata is scanned without copying payloads; generated live-state writes are bounded by the configured Task/Source tables and affected pins, rather than by retained message payload size.

Task attachment and last-owner detachment now use this cleanup for valid core publication-bearing state. Replacement still acquires the native claim under the mutation gate before installing its incarnation. The OS lease remains the liveness authority. Task/Thread claims, historical acknowledgments, and Source counters follow the existing [ownership contract](native-storage.md#public-session-and-task-lifetimes).

## Qualification and remaining work

The [test guide](../test/README.md) distinguishes full-domain private delivery fixtures, deliberate corruption, and real process interruption from public API examples. Qualification covers exact tokens and counters, retained history after ring overwrite, full logs, old parent/Thread cleanup, partial absolute stores, replacement attachment, and repeated interrupted recovery. It tests process termination on a running local filesystem, without adding a power-loss durability guarantee.

Factories now select **2.2/`0x7`** in step 4.6 for [direct send and publication lookup](direct-publication.md). Existing profiles are never upgraded in place. Step 4.7 implements public receive, payload-view ownership, explicit Source close, and deferred cleanup. Its [public contract](native-receive.md) explains view lifetimes and failure fencing. Allowed-loss semantics and LoseRange replay remain step 4.8.
