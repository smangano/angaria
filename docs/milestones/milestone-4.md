# Milestone 4 Two-plane mapped transport

Milestone 3 and the manual Intel CI change reached `main` through [PR #30](https://github.com/smangano/angaria/pull/30) and [PR #31](https://github.com/smangano/angaria/pull/31), framework version `0.3.8`. Staging branch `milestone-4` starts at that commit. Each step uses `milestone-4-<s>` and a PR targeting staging; a separate integration PR brings the completed milestone into `main`.

Steps 4.1 and 4.2 used framework versions **`0.4.1`** and **`0.4.2`**; step 4.3 uses **`0.4.3`**. The build number increases for each new PR under the [versioning policy](../../ROADMAP.md#versioning). It does not automatically change native format or protocol versions.

The focused [helper consolidation](../helper-audit.md) after step 4.3 uses **`0.4.4`**, on `milestone-4-3-cleanup`, with a PR into `milestone-4`. It leaves the review-step numbering below unchanged. Steps **4.4** and **4.5** use framework versions **`0.4.5`** and **`0.4.6`** respectively.

## Review steps

A separate **0.4.7** [public interface review](../public-interface.md), branch `milestone-4-api-review`, follows step 4.5. It proposes the full application/log/replay surface through 1.0 without implementing new runtime operations or renumbering these development steps. Step 4.6 incorporates the approved message/header model, owning direct outcomes, and named setup while enabling direct publication at version 0.4.8.

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
| 4.12 | `milestone-4-12` | Diagnose the measured performance before integration. | Split public-operation costs, count repeated work, record history/capacity/schema/stream controls, quantify observation overhead, and identify measured optimization candidates without changing transport algorithms. |
| 4.13 | `milestone-4-13` | Reuse validated record information. | Adopt the journal's validated record ranges for runtime lookup and reuse preflight's own prefix scan; qualify behavior and record measured improvement before further optimizations. |
| 4.14 | `milestone-4-14` | Reuse bounded mappings while scanning history. | Replace per-record native reads/mappings with scan-local windows, retain atomic readiness and complete validation, qualify boundary/rollback behavior, and record matched measurements. |
| 4.15 | `milestone-4-15` | Give queued local blocking operations a gate handoff. | Prevent repeated nonblocking overtaking, qualify registration/race/cleanup behavior, and measure ordinary versus loaded progress with explicit scheduling limits. |

Split a step further if needed to keep its PR reviewable. Each implementation PR includes focused teaching tests, appropriate native crash/I/O checks, and primer updates. Existing lessons remain runnable; new examples use delivery methods only after those methods are implemented.

## Step 4.1 Contract review

The [transport contract](../native-transport.md) makes the milestone's design decisions explicit. It proposes native format 2.2 with separate required capabilities for core delivery and allowed loss, preserving the meanings of existing 2.0/2.1 Sessions. It defines full-width ring generations, receipt timestamp presence, Binary validation before acceptance, and distinct publication/enqueue/dequeue times.

The original step-4.1 plan called for a publication timestamp and explicit Source close in step 4.7. The approved interface review now uses one MessageHeader shared by Message and MessageView, wrapped by Delivery; step 4.6 promotes that model and step 4.7 implements receive and SourceQueueSet::close(). Explicit close reports cleanup errors; destruction and move replacement still need a fenced, deferred-cleanup path. Delivery-aware Session/topology validation follows in step 4.3 and Task cleanup in step 4.5; both are prerequisites of public send.

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

## Step 4.6 Direct publication and lookup

Framework **0.4.8** implements [direct publication](../direct-publication.md) and promotes the approved Message/MessageView/Delivery model with one consistent MessageHeader. Named Task/endpoint setup and owning PublicationOutcome overloads make the first operational producer path match the reviewed interface. Existing caller-buffer reports remain available.

Public factories select core **2.2/0x7**. Exact schema and Binary admission, complete append sizing, issued IDs, identical repeats, conflicts, retained earlier-incarnation status, and interrupted outcomes are covered through public tests and focused journal/process-death qualification. Known rejection changes no acceptance state. Legacy Sessions retain their existing operations and reject native publication without writes. Portable record encoding 1 remains 80 bytes and rejects optional delivery fields it cannot represent.

The [fourteenth primer lesson](../../examples/primer/14_direct_send.cpp) sends to an offline consumer and looks up acceptance after producer replacement. Receive, acknowledgment/release, and Source close remain step 4.7; advanced schemas and message replay retain their roadmap milestones.

## Step 4.7 Source receive and lifecycle

Framework **0.4.9** implements [basic Source delivery](../native-receive.md): receive, exact-token acknowledgment/release, stable borrowed payloads, full MessageHeader context, and owning message copies. Repeated receive preserves the in-flight token/time/address; retained Queue history supplies messages after ring overwrite. Fresh selection supports at most one Active Queue, with multiple-input policies left to milestone 5.

Zero-time polls and finite waits use nonblocking gate attempts; checked monotonic deadlines prevent fresh issuance after expiration. Recovery and an already started mutation still finish consistently. Explicit close uses Active/Closing/Closed states, reports failure, and releases exact unfinished work. Destruction/move replacement use preallocated deferred reservations when cleanup fails; new Task delivery/progress and Source reopening reconcile them before proceeding. Status and identical-send lookups remain available without cleanup mutation. No Task cycle or payload ownership is retained by a deferred marker.

The [fifteenth primer lesson](../../examples/primer/15_receive.cpp) demonstrates backlog recovery, release/redelivery, complete-header copying, acknowledgment, close/reopen, and decoding an owning copy after all runtime handles close. Public, fault-injection, deadline, and independent consumer-process tests qualify these operations, including progress at an exactly full log and inherited/wrong-thread rejection. Native format **2.2/0x7**, portable record version 1, and the synchronization protocol remain unchanged. Configured allowed loss follows in step 4.8.

## Step 4.8 Configured allowed loss

Framework **0.4.10** enables `DeliveryPolicy::AllowLoss` through public descriptors and the common TOML loader. Default delivery remains recovery from the log. Factories create format **2.2/0xf**, including the separate allowed-loss capability; existing core **2.2/0x7** Sessions remain attachable and reject allowed-loss options/registration without applying them.

An idle Source reports the inclusive overwritten pending prefix as a DeliveryGap before offering the retained window. Already Issued messages remain protected until release, close, or claim cleanup ends the pin. Compact LoseRange plus absolute Queue progress uses 80 reserved journal bytes, streams receipt updates without payload reads, preserves actual acknowledgment context, and finishes at a full log without reclaiming history. Complete validation precedes mutation; interrupted replay repairs every partial word using the captured loss timestamp. A committed gap not returned before a crash is not emitted again.

The [sixteenth primer lesson](../../examples/primer/16_allowed_loss.cpp) uses explicit TOML and public Result/Error checks to demonstrate gaps, pin protection, and the unchanged recovery default. Public policy, metadata/capability, malformed-redo, full-log, and independent process-death tests qualify these rules. Portable record encoding and the synchronization protocol remain unchanged. Multiple-Queue selection and Publishing retain their roadmap milestones.

## Step 4.9 Failure and exhaustion qualification

Framework **0.4.11** adds [combined failure and exhaustion qualification](../native-qualification.md), with separate producer, consumer, and recovery processes. A producer can die during Prepared or Committed publication while another process keeps an unchanged in-flight view. Repeatedly interrupted replay and later consumer replacement preserve acceptance, Queue ordering, protected pins, and the difference between recovery delivery and allowed loss. Other scenarios let a fresh producer operation finish an interrupted acknowledgment, then combine mixed receipt history and owner replacement at an exactly full log.

Counter fixtures exercise the last representable publication ID, Source issuance, and global operation generation through public calls, then verify unchanged rejection without wrapping. A child-only OS file-size limit forces a real file-growth error, proving precommit rollback preserves earlier acceptance and a live borrow before an unchanged-ID retry. The test guide identifies private hooks and boundary edits explicitly; they are not application configuration.

The primer now explains uncertain outcomes, preserved identities, stale tokens, possible application work repetition, and the difference between append exhaustion and counter exhaustion. The runnable lessons remain public-API examples. Linux CI uses an explicit three-job compiler limit, matching native-profile jobs, and all automatic profiles run the full suite. Native format, public API, and synchronization protocol are unchanged. The native multi-process primer application remains step 4.10; measurements and the completion audit remain step 4.11.

## Step 4.10 Native multi-process primer application

Framework **0.4.12** adds the [native market-monitor walkthrough](../native-pipeline.md) as primer lesson 17. Feed, Strategy, and Audit are independent processes using public APIs and explicit TOML. Feed produces two exact Quote versions with equal byte widths and reversed Field order. Strategy decodes the received schema, preserves the full MessageHeader when copying, and forwards a PriceAlert that links back to the original Feed publication and Queue sequence. Audit receives and acknowledges the derived messages through its own Source.

The coordinator starts with backlog beyond the two-entry ring, terminates Feed after accepted publication and Strategy after its first borrow but before output, then runs replacement roles and a live Audit. It verifies retained Queue order, new producer/consumer incarnations, release/redelivery with a stale old token, six linked outputs, and retained acceptance lookup after the business processes exit. Separate EOF handling abandons an unfinished input without forwarding it. Existing workspaces, duplicate initialization, competing ownership, and invalid arguments preserve prior data at their rejection boundaries.

The application compiles with tests disabled. Its CTest walkthrough invokes the same public commands and retains role transcripts when a caller requests a fresh workspace. The primer and detailed guide explain the commands, schema/identity output, lifetime rules, and the separate send/acknowledgment commits. Each Source still has one Active Queue; selection, Publishing, and the five-process trading simulation retain their later scope. Measurements and the final audit remain step 4.11.

## Step 4.11 Measurements and completion audit

Framework **0.4.13** adds the [measurement guide and baseline](../native-performance.md) and [completion audit](milestone-4-completion.md). A bounded C++ tool measures ordinary public buffered send/receive cycles and phased backlog, with constant four-channel geometry and total retained history across producer counts. A separate instrumented private profile observes gate acquisition, preparation, file growth, materialization, and apply boundaries. It reports physical append growth rather than claiming zero heap allocation or pure lock cost. Python retains raw reports, aggregate summaries, and TOML experiment metadata; CI checks correctness without speed thresholds.

The completion review covers all eleven steps, retained native profiles and capability-before-recovery rejection, current/future API boundaries, blocking progress guarantees, full-log reserved work, and all seventeen primer lessons. It corrects stale future-tense descriptions and identifies v0 examples as legacy. This step targets staging; the milestone integration into `main` is a separate PR after this review is merged.

## Step 4.13 First performance optimization

Framework **0.4.15** removes redundant record reads through [validated-prefix reuse](../performance-prefix-reuse.md). The runtime adopts framing already checked by the journal; preflight reuses its own validated ranges. Snapshot and execute entry points still validate freshly. This step also records the project's equal priority for performance and correctness, with explicit risk assessment of defensive costs.

## Step 4.14 Bounded metadata scan windows

Framework **0.4.16** introduces [scan-local mapped windows](../performance-prefix-windows.md) for committed history. Each scan retains at most one bounded mapping and releases it before recovery can shrink a provisional append. The change preserves full validation and atomic readiness while reducing repeated native calls. The guide records the explicit mapped-I/O tradeoff, qualification, and measurements.

## Step 4.15 Local gate handoff

Framework **0.4.17** gives [announced local blocking requests preference over try-only polling](../performance-gate-handoff.md). Deterministic tests cover the free-mutex interval, a registration race, and failure/exception cleanup. The guide records the class-preference tradeoff and measured progress; FIFO, cross-process fairness, and response-time bounds remain outside the claim.

## Qualification and completion

The step-4.11 measurements exposed substantial preparation cost. Step **4.12**, framework **0.4.14**, adds [detailed diagnostics](../performance-diagnostics.md). Integration into `main` is held for diagnosis, focused measured fixes, and final performance/correctness qualification. The [recorded findings](../performance-diagnostics-findings.md) retain the measured code, raw evidence, and proposed fix sequence. Additional fix PRs will target `milestone-4`; no integration PR is opened as part of diagnostics.

Completion requires real mapped publication and delivery across independent processes, retained-log recovery after ring overwrite, optional gap reporting, full-log progress, and repeatable producer/consumer recovery. Benchmarks report the cost of the existing blocking session gate; no lock-free, starvation-free, hard-real-time, or power-loss claim is introduced.

Automatic CI runs Linux x86_64, Linux ARM64, and macOS ARM64. Intel Mac qualification remains [request-only](../native-storage.md#request-macos-x86_64-qualification). The integration PR records results for its exact commit, with all supported runtime/format boundaries and primer examples reviewed together.

Publishing fan-out follows milestone 6, eventfd waiting milestone 7, segmented storage/reclamation milestone 8, advanced Fields/serializers milestone 9, and generalized native export/replay/migration tools milestone 10.

[Roadmap](../../ROADMAP.md) · [Native transport contract](../native-transport.md) · [Completed milestone 3](milestone-3.md)
