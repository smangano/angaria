# Milestone 4 Two-plane mapped transport

Milestone 3 and the manual Intel CI change reached `main` through [PR #30](https://github.com/smangano/angaria/pull/30) and [PR #31](https://github.com/smangano/angaria/pull/31), framework version `0.3.8`. Staging branch `milestone-4` starts at that commit. Each step uses `milestone-4-<s>` and a PR targeting staging; a separate integration PR brings the completed milestone into `main`.

Steps 4.1 and 4.2 used framework versions **`0.4.1`** and **`0.4.2`**; step 4.3 uses **`0.4.3`**. The build number increases for each new PR under the [versioning policy](../../ROADMAP.md#versioning). It does not automatically change native format or protocol versions.

The focused [helper consolidation](../helper-audit.md) after step 4.3 uses **`0.4.4`**, on `milestone-4-3-cleanup`, with a PR into `milestone-4`. It leaves the review-step numbering below unchanged. Steps **4.4** and **4.5** use framework versions **`0.4.5`** and **`0.4.6`** respectively.

## Review steps

A separate **0.4.7** [public interface review](../public-interface.md), branch `milestone-4-api-review`, follows step 4.5. It proposes the full application/log/replay surface through 1.0 without implementing new runtime operations or renumbering these development steps. Step 4.6 should incorporate the relevant approved API decisions before public send is enabled.

| Step | Branch | Scope | Completion criterion |
| --- | --- | --- | --- |
| 4.1 | `milestone-4-1` | Review the [native transport contract](../native-transport.md). | Specify compatibility/capabilities, message validation, timestamps, ring/receipt encodings, view lifetimes, and Source cleanup. This PR changes documentation and framework version only. |
| 4.2 | `milestone-4-2` | Implement checked Publication, destination, receipt, and ring codecs. | Independent golden bytes and malformed-input tests cover widths, references, bounds, state/presence combinations, and full-width sequences. No public delivery yet. |
| 4.3 | `milestone-4-3` | Extend Session and history validation. | Validate committed publications, Queue links, receipts, and live state without breaking Registry/snapshot operations. Unsupported capabilities fail before recovery. |
| 4.4 | `milestone-4-4` | Implement mapped payload storage and allocation planning. | Preflight complete append costs; map committed immutable payloads safely across file growth; rollback never truncates pinned committed bytes. |
| 4.5 | `milestone-4-5` | Extend reserved progress, replay, and claim cleanup. | Private receipt transitions and publication-aware Task/Source cleanup are idempotent, fit FixedState storage, and work at `LogFull`. Complete these before enabling public send. |
| 4.6 | `milestone-4-6` | Enable direct send and publication lookup. | `QueueWriter::send` and `Task::publication_status` implement atomic acceptance, issued-ID checks, identical repeats, conflicts, retained lookup, and interrupted outcomes. Enable creation of the core delivery profile only after its prerequisites are complete. |
| 4.7 | `milestone-4-7` | Enable basic Source delivery. | Existing receive/acknowledge/release APIs support one Active Queue, ring/history fallback, stable payload views, exact tokens, spinning, deadlines, and the reviewed explicit-close lifecycle. |
| 4.8 | `milestone-4-8` | Enable configured allowed loss. | Negotiate its separate capability, execute/replay `LoseRange`, report gaps, and preserve issued pins. Programmatic and TOML validation enable the same behavior. |
| 4.9 | `milestone-4-9` | Complete failure and exhaustion qualification. | Combine producer/consumer termination, interrupted/repeated recovery, wraparound, full logs, counter boundaries, and unchanged-request rejection. Focused fault tests also accompany earlier mutation steps. |
| 4.10 | `milestone-4-10` | Add a native multi-process primer application. | Independent producers/consumers exercise the real public mapped path, exact schemas, backlog, acknowledgment/redelivery, and replacement, using at most one Active Queue per Source. |
| 4.11 | `milestone-4-11` | Measure and finish the milestone. | Record reproducible latency, throughput, and allocation/commit contention baselines; audit progress guarantees, compatibility, documentation, and the full primer before integration. |

Split a step further if needed to keep its PR reviewable. Each implementation PR includes focused teaching tests, appropriate native crash/I/O checks, and primer updates. Existing lessons remain runnable; new examples use delivery methods only after those methods are implemented.

## Step 4.1 Contract review

The [transport contract](../native-transport.md) makes the milestone's design decisions explicit. It proposes native format 2.2 with separate required capabilities for core delivery and allowed loss, preserving the meanings of existing 2.0/2.1 Sessions. It defines full-width ring generations, receipt timestamp presence, Binary validation before acceptance, and distinct publication/enqueue/dequeue times.

Two process-local API additions are planned for step 4.7: `DeliveryView::_publishedAt` and `SourceQueueSet::close()`. Headers are unchanged in step 4.1. Explicit close reports cleanup errors; destruction and move replacement still need a fenced, deferred-cleanup path. Delivery-aware Session/topology validation follows in step 4.3 and Task cleanup in step 4.5; both are prerequisites of public send.

Fresh Source selection supports at most one Active member Queue in this milestone. Existing in-flight delivery and acknowledgment/release remain usable if another Queue becomes Active. Multiple-Queue selection policies follow milestone 5 through the same Source APIs.

## Step 4.2 Local delivery codecs

Private codecs now encode and decode the [Publication, destination, receipt, and ring layouts](../mapped-format.md#local-delivery-codecs). Their tests use independent expected bytes, deliberately malformed inputs, exact byte ceilings, receipt presence combinations, and ring sequences through `UINT64_MAX`. Framing tests check the boundary between immutable bytes, mutable receipt storage, and the journal's allowed write targets.

These functions operate on local byte copies. They do not establish committed acceptance, resolve Registry/history references, access live atomic words, or enable native delivery. Factories remain at format 2.1. Step 4.3 adds the cross-record and live-state validation described below before any public send is enabled.

## Step 4.3 Session and history validation

The [core reader validator](../native-validation.md) checks committed acceptance manifests, unique issued identities, exact registered schema references, historical active topology, complete Queue chains/roots, receipt/Source pins, acknowledgment progress, and authoritative ring windows. Existing Registry topology and historical snapshots remain inspectable with valid retained Publications. Invalid quiescent fixtures reject without writes; incompatible profiles reject before pending journal recovery.

Readers admit canonical 2.2/`0x7` while factories still create 2.1/`0x3`. Loss and unsupported requirements remain fenced out before recovery. Task attachment to publication-bearing state keeps its cleanup guard until step 4.5. New private tests clearly distinguish native record fixtures from still-unimplemented public delivery calls, and all thirteen primer programs remain the public learning path.

## Step 4.4 Mapped payload storage and allocation planning

Private [payload owners and complete append sizing](../native-storage.md#committed-payload-storage-and-append-planning) now provide the storage foundations for delivery. A payload owner maps only a canonical committed Publication's application bytes and retains that address across file growth and rollback of a later append. Empty payloads avoid native zero-length mappings. This local mapping lifetime is separate from the persisted receipt/Source pin, which later progress and receive steps must establish.

The journal and topology/configuration preflight share checked complete-batch geometry. Sizing includes immutable/mutable boundaries, all receipts, envelopes, manifest references, redo, and padding before journal image construction. These private foundations leave public delivery disabled and factories on 2.1/`0x3`; progress and claim cleanup remain step 4.5 prerequisites of sending.

## Step 4.5 Reserved progress and claim cleanup

The private [reserved progress path](../native-progress.md) issues, acknowledges, and releases exact deliveries with bounded inline FixedState commands. It preserves Source issuance context on cleanup, increments issuance on redelivery, and uses retained Queue history after ring overwrite. Old tokens reject without new progress; an already pinned receive repeat preserves its identity/time even at exhausted fresh counters.

Publication-aware ReleaseClaim restores old Issued receipts to Pending, clears owned Source pins independently of partially cleared slot pointers, and detaches the exact old Task/parent-bound Thread claims. Its compact persisted intent remains fixed-size, replay validates all targets before applying, and replacement/progress need no new append space at `LogFull`. Legacy profiles retain their Task-only checks. Factories and public delivery remain unchanged until step 4.6.

## Qualification and completion

Completion requires real mapped publication and delivery across independent processes, retained-log recovery after ring overwrite, optional gap reporting, full-log progress, and repeatable producer/consumer recovery. Benchmarks report the cost of the existing blocking session gate; no lock-free, starvation-free, hard-real-time, or power-loss claim is introduced.

Automatic CI runs Linux x86_64, Linux ARM64, and macOS ARM64. Intel Mac qualification remains [request-only](../native-storage.md#request-macos-x86_64-qualification). The integration PR records results for its exact commit, with all supported runtime/format boundaries and primer examples reviewed together.

Publishing fan-out follows milestone 6, eventfd waiting milestone 7, segmented storage/reclamation milestone 8, advanced Fields/serializers milestone 9, and generalized native export/replay/migration tools milestone 10.

[Roadmap](../../ROADMAP.md) · [Native transport contract](../native-transport.md) · [Completed milestone 3](milestone-3.md)
