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
| 4.16 | `milestone-4-16` | Reuse bounded mappings for runtime metadata. | Share the journal window mechanism, retain fresh atomic loads and owning copies, qualify boundary/rollback behavior, and record matched measurements. |
| 4.17 | `milestone-4-17` | Reuse operation-entry history validation once. | Keep generic journal calls fresh, consume first-attempt reuse before failures/retries, qualify ownership/staleness/recovery, and measure duplicate-scan removal. |
| 4.18 | `milestone-4-18` | Reuse receive selection validation during issuance. | Retain live claim/pin/Queue/receipt checks and fresh generic/cleanup paths, qualify unchanged-context reuse, and plot measured latency, throughput and validation work. |
| 4.19 | `milestone-4-19` | Resolve the oldest pending delivery directly. | Retain full history validation and exact committed metadata/receipt checks, qualify invalid links and bounded reads, and plot latency, throughput and allocation improvements. |
| 4.20 | `milestone-4-20` | Reuse checked definitions within an operation. | Share complete retained Queue/Source history and validated Tasks during schema/delivery checks, qualify historical attribution and fresh generic paths, and plot measured improvements. |
| 4.21 | `milestone-4-21` | Share fresh schema preparation with payload codecs. | Reuse checked Fields during send/receive preparation, retain hook/cleanup ordering and generic fresh paths, qualify owning lifetimes and read counts, and plot measured improvements. |
| 4.22 | `milestone-4-22` | Reuse prefix-validation buffers. | Use exact fixed-body spans and scan-local manifest scratch, retain atomic/read/recovery checks, qualify smaller reads after growth and corruption rejection, and plot matched improvements. |
| 4.23 | `milestone-4-23` | Remove duplicate pre-lock resource checks. | Retain post-lock rejection and other resource checks, qualify delayed mismatch detection under contention, and measure native calls and full acquisition cost with matched controls. |
| 4.24 | `milestone-4-24` | Add on-demand Cachegrind profiling. | Profile only the serial cycle after preload in nine fixed-model cases, validate the requested region, retain raw/source-annotated evidence, and keep ordinary CI and native timing interpretation separate. |
| 4.25 | `milestone-4-25` | Avoid uniqueness scratch for a single destination. | Keep all row/header/policy checks and multi-destination duplicate rejection, qualify codec boundaries, and compare same-job base/head profiles plus matched ordinary timings. |
| 4.26 | `milestone-4-26` | Validate journal history without owning manifest/command copies. | Share bounded parsing and corruption checks, retain owning recovery/application decoding, qualify malformed inputs and measure allocations, ordinary timings and matched profiles. |
| 4.27 | `milestone-4-27` | Plan recorded unacknowledged delivery and the nanomsg comparison. | Preserve replay and acknowledged behavior; define the benchmark-first sequence, guarantee matrix, latency/throughput measures and qualification gates. |
| 4.28 | `milestone-4-28` | Implement the first 1:1 nanomsg IPC comparison. | Pin optional classic libnanomsg, qualify independent-process payloads and timing/accounting, retain matched latency/throughput evidence, and provide request-only hosted measurements. |
| 4.29 | `milestone-4-29` | Diagnose cross-process stalls and prototype current-log SPSC delivery. | Separate native lock wait/hold and preparation, qualify committed atomic delivery/replay/crashes, and retain matched ordinary latency/throughput plus separate diagnostic controls. |
| 4.30 | `milestone-4-30` | Freeze the independent messaging protocol. | Specify producer-owned acceptance/replay, independent credit and optional acknowledgment ownership before replacement implementation. |
| 4.31 | `milestone-4-31` | Define producer-log codecs. | Fix private format-3 headers/references/records; qualify golden bytes, bounds, allocation-free encoding and borrowed decoding. |
| 4.32 | `milestone-4-32` | Implement producer-owned append, recovery and replay. | Reserve/map once, claim the actual data inode, persist incarnation, release-publish acceptance, recover complete prefixes and retain frozen replay cuts; qualify actual process crashes and focused append/replay latency and throughput. |
| 4.33 | `milestone-4-33` | Implement recorded/unacknowledged point-to-point endpoints. | Bind checked fixed context to the producer log and atomic ring, enforce logical credit, qualify order/wrap/empty/retry and retained copies, then compare complete ID/send/outcome-lookup/receive latency and verified throughput against nanomsg. |

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

## Step 4.16 Runtime metadata windows

Framework **0.4.18** extends [bounded mapping reuse](../performance-runtime-windows.md) to atomic words and immutable metadata within one gate-held runtime operation. Separate readers retain owning result copies and fresh atomic loads; no mappings or decoded state are cached between operations. Boundary and rollback qualification accompany matched normal/enabled measurements.

## Step 4.17 Operation validation reuse

Framework **0.4.19** [reuses the initial validated prefix](../performance-operation-validation.md) for one execution attempt under the continuously held runtime guard. Current roots/size, plan/domain checks, accepted-byte checks and recovery remain checked; generic calls and subsequent attempts validate freshly. The guide defines the internal trust boundary and accepted change in validation frequency.

## Step 4.18 Receive validation reuse

Framework **0.4.20** [reuses the complete topology/delivery validation from selection](../performance-receive-validation.md) when issuing under the same unchanged held RuntimeAccess. Generic progress and cleanup validate freshly, while live ownership/pin/Queue/receipt checks remain. The guide records the private trust boundary, qualifications and before/after plots.

## Qualification and completion

The step-4.11 measurements exposed substantial preparation cost. Step **4.12**, framework **0.4.14**, adds [detailed diagnostics](../performance-diagnostics.md). Integration into `main` is held for diagnosis, focused measured fixes, and final performance/correctness qualification. The [recorded findings](../performance-diagnostics-findings.md) retain the measured code, raw evidence, and proposed fix sequence. Additional fix PRs will target `milestone-4`; no integration PR is opened as part of diagnostics.

Completion requires real mapped publication and delivery across independent processes, retained-log recovery after ring overwrite, optional gap reporting, full-log progress, and repeatable producer/consumer recovery. Benchmarks report the cost of the existing blocking session gate; no lock-free, starvation-free, hard-real-time, or power-loss claim is introduced.

Automatic CI runs Linux x86_64, Linux ARM64, and macOS ARM64. Intel Mac qualification remains [request-only](../native-storage.md#request-macos-x86_64-qualification). The integration PR records results for its exact commit, with all supported runtime/format boundaries and primer examples reviewed together.

Publishing fan-out follows milestone 6, eventfd waiting milestone 7, segmented storage/reclamation milestone 8, advanced Fields/serializers milestone 9, and generalized native export/replay/migration tools milestone 10.

[Roadmap](../../ROADMAP.md) · [Native transport contract](../native-transport.md) · [Completed milestone 3](milestone-3.md)

## Step 4.19 Direct delivery lookup

The [lookup guide and risk assessment](../performance-delivery-lookup.md) replaces issuance's repeated Publication metadata search with exact resolution of the already validated oldest-pending link. The direct-send profile has one destination at a canonical position; the resolver checks subtraction, committed boundary/kind, bounded metadata and exact Queue/sequence/link identity before fresh receipt checks. Complete history validation, pin repeats and generic progress/cleanup remain. Acknowledgment still searches for its successor link. Matched normal/enabled controls and plots record the improvement without a timing threshold or a bounded-latency claim.

## Step 4.20 Definition reuse

The [definition reuse guide](../performance-definition-reuse.md) removes duplicate Task loading for Field/Schema creator checks and duplicate decoding of retained Queue/Source history during delivery validation. Borrowed definitions stay within the same unchanged RuntimeAccess and continuously held guard. All historical effective-definition, creator, index, schema and live delivery checks remain; generic entry points load freshly and no state is cached between operations. Focused teaching tests and matched normal/enabled measurements qualify reuse and record plots.

## Step 4.21 Payload preparation

The [preparation guide and risk assessment](../performance-payload-preparation.md) introduces an owning private schema/Field context loaded fresh at the original preparation point. Codec construction uses those checked definitions instead of reloading Fields and Tasks. Receive still maps before schema preparation and retains exact schema-reference checks, hooks, binding and failure cleanup. Non-Binary sends do not construct a Binary codec. The context stores no Task authority or cross-operation cache; public headers, formats and payload lifetimes remain. Focused teaching tests and matched normal/enabled controls qualify behavior and record plots.

## Step 4.22 Prefix-validation buffers

The [prefix buffer guide](../performance-prefix-buffers.md) replaces each small fixed-body vector with a bounded local array and reuses variable manifest input within one scan. Exact requested lengths, full decoding/validation and owning results remain. A later scratch read invalidates only private input consumed immediately by an owning decoder. Scratch and mapping leases end before recovery truncation. Tests qualify small/large/small manifests, short bodies, padding rejection/repair and existing rollback behavior; matching execution settings and paired studies supply plots and work counts.

## Step 4.23 Gate resource checks

Framework **0.4.25** [removes the three pathname-identity checks immediately before the native gate lock](../performance-gate-resources.md). The same post-lock check must pass before a usable guard is returned; file-access, claim and ownership checks remain. The guide explicitly accepts delayed `IoError` after unsupported resource replacement: try-only callers can see Busy/public receive timeout, and a blocking caller can wait indefinitely on a held old lock. Focused contention qualification and matched syscall/acquisition measurements are required. Native hold timing begins after acquisition, so its improvement is not assumed; Cachegrind profiling remains a separate follow-up.

## Step 4.24 Cachegrind profiling

Framework **0.4.26** adds [a benchmark-only region and on-demand profiling workflow](../performance-cachegrind.md). Optimized Linux x86_64 builds use debug locations, private diagnostics OFF and optional Cachegrind hooks ON. Three repetitions at H0/32/128 each profile the existing 16-message serial loop after setup/preload, with cold simulated caches, a fixed cache model and branch simulation. Public-result/region validation, raw profiles, source annotations, build provenance and all attempt outcomes accompany the evidence. PR labeling or manual dispatch requests profiling; ordinary CI and public APIs are unchanged. The primer review requires no new lesson or example changes.

## Step 4.25 Single-destination validation

Framework **0.4.27** [skips the uniqueness bitmap only for exactly one Publication destination](../performance-single-destination.md). The sole row still passes full validation; common header, acceptance and direct-send checks remain, and the general branch retains duplicate detection. Focused codec fixtures cover malformed sole rows and multi-destination boundaries. The optional comparison label profiles immutable base/head checkouts with the same toolchain/model on one job, retaining 18 raw profiles; matched ordinary measurements remain separate. [Retained results](../../benchmarks/results/milestone-4-single-destination/README.md) show +4.77% complete-cycle throughput at H=128 in twenty native pairs, with noisier low-history results; matched Cachegrind records −4.78% instructions and −23.71% data-write references per completed cycle there. Existing work counters remain unchanged. Local diagnostics OFF/ON and all three automatic CI platforms each pass 825 tests. Public APIs and the primer's seventeen examples require no changes.

## Step 4.27 Recorded unacknowledged delivery plan

Framework **0.4.29** records the [delivery plan](../recorded-unacknowledged-delivery-plan.md) and [nanomsg comparison specification](../nanomsg-comparison-plan.md). The runnable same-host comparison is the next implementation step, ahead of selecting more targeted optimizations. It starts with supported direct Queue delivery and an explicitly labeled acknowledged baseline; the recorded unacknowledged profile, Source fan-in and Publishing fan-out follow their contract and implementation gates. This planning step changes no native layout, runtime policy, public API or runnable primer program.
## Step 4.26 Manifest validation

Framework **0.4.28** [uses bounded validation-only manifest/command traversal](../performance-manifest-validation.md) during committed-history scans. Owning decoding remains for recovery/application. Shared validation preserves corruption rejection and error ordering; borrowed bytes are consumed synchronously within their input lifetime. [Retained measurements](../../benchmarks/results/milestone-4-manifest-validation/README.md) show +9.42% complete-cycle throughput at H=128 across twenty native pairs, with noisier low-history results; the first ID-issuance allocation count falls 1144→133. All 603 matched serial Counts sample dictionaries retain identical nonallocation counters. Same-job Cachegrind records −8.07% instructions and −9.26% data-write references per cycle there, with mixed cache/branch outcomes. Local diagnostics OFF/ON and all three source CI platforms each pass 830 tests. Public APIs and the primer's seventeen examples require no changes.

## Step 4.28 Independent-process IPC comparison

Framework **0.4.30** implements the [first comparison phase](../nanomsg-comparison.md): one producer and one consumer, shared checked bytes, bounded process coordination, explicit monotonic clocks, and ten runs per profile at each payload/history cell. Classic libnanomsg 1.3.0 is an optional statically linked benchmark dependency, pinned by commit and archive checksum; ordinary builds do not fetch it. Current acknowledged/replayable Angaria and unacknowledged/unrecorded nanomsg retain distinct labels and completion boundaries. Qualification checks actual adapters, corrupt reports, history/drain behavior and participant cleanup without performance thresholds. On-demand CI adds Linux measurements and three-platform adapter qualification. [Retained Linux measurements](../../benchmarks/results/milestone-4-nanomsg-comparison/README.md) include all 180 complete attempts and expose concurrent-process stalls; the profiles retain different guarantees. No runtime/public API or primer-example behavior changes; the seventeen lessons were reviewed.

## Step 4.29 Stall diagnostics and current-log SPSC experiment

Framework **0.4.31** adds [focused cross-process diagnostics and a private recorded/unacknowledged experiment](../stall-study.md). Native lock acquisition and local mutex acquisition have distinct opt-in phases; no current locking policy changes. The prototype retains public serialized append/commit and legacy Pending receipts while delivering committed log references through an atomic connection without Source issuance or acknowledgment. Fixed-topology, ownership, credit, payload lifetime, log-full and real process-crash/replay qualification precede measurements. Ordinary three-profile comparisons, backlog controls and diagnostic empty/concurrent controls remain separate. [Retained Mac/Linux results](../../benchmarks/results/milestone-4-stall-study/README.md) separate ordinary performance from diagnostic attribution. This step admits no new public native profile or API; the public primer was reviewed and remains runnable.

## Step 4.32 Producer-owned append, recovery and replay

Framework **0.4.34** implements workstream step 3's private [producer log](../producer-log.md), following the protocol design in **0.4.32** and byte codecs in **0.4.33**. Mappings and bounded indexes are prepared at open. One producer owns the actual data inode, persists its next incarnation before issuing IDs, and accepts a complete immutable record with one release-store. Recovery checks the acquired committed prefix without truncating accepted history. Frozen replay cuts and owning storage copies preserve publication headers and payloads independently of later appends or reader destruction.

Tests terminate or pause actual append operations at partial, before-commit and after-commit boundaries and cover exclusive/fork ownership, retry/conflict, count/byte exhaustion and independent streams. A focused ordinary-CI performance test measures ID-plus-append latency and rate, incremental capture, replay and recovery separately, with allocation and diagnostic guards. [Retained evidence](../../benchmarks/results/milestone-4-producer-log-append/README.md) establishes this component's baseline; end-to-end nanomsg comparisons await the ring delivery step. Public admission and the seventeen runnable primer programs remain unchanged, and the primer explains the new building block without demonstrating private APIs as application code.

## Step 4.33 Recorded point-to-point delivery

Framework **0.4.35** supplies workstream step 4's private [recorded producer/consumer endpoints](../recorded-queue.md). A checked frozen binding opens native resources and exact codecs once. New acceptances obey logical Queue credit; ring descriptors are atomic hints and cannot replace log commitment. A consumer can discover a committed record before its live producer publishes the descriptor, return credit, and ignore a later hint without duplicate delivery. Bounded live polling never rescans old history. Received MessageView headers and retained owning copies include exact publication, Queue and consumer observations.

Independent byte fixtures, functional/process tests, ownership/uncertainty/replacement cases and isolated allocation/native-work guards qualify the initial one-schema, one-outgoing-Queue Process profile. A matched independent-process [nanomsg study](../../benchmarks/results/milestone-4-recorded-connection/README.md) includes ID issuance, send, ordinary outcome lookup and verified receive in its complete path; finite-cohort latency/rates remain distinct from sustained capacity and processing acknowledgment. The seventeen public primer lessons remain runnable, with private progress explained separately. Workstream step 5 broadens failure qualification; public admission must expand shared Task ownership, exact-schema caching and lifecycle support before this becomes the supported application runtime.

## Step 4.34 Restart, catch-up and connection independence

Framework **0.4.36** qualifies workstream step 5's [private restart behavior](../recorded-queue.md#restart-and-connection-independence). Independent processes are killed at six producer boundaries and on both sides of consumer credit publication. Replacement preserves accepted identities and ordering, replays credited history across ring reuse without returning credit twice, and rejects stale descriptor generations. Owning Message copies and frozen replay cuts retain their documented lifetimes. The new recovery observer pauses only cold opening after its actual log claim; ordinary messaging gains no checkpoint or gate.

Distinct streams in one Session continue while its actual administrative gate is held, another producer is paused, a peer Queue is full, a peer log is exhausted, or affected-stream recovery is paused and killed. [Focused measurements](../../benchmarks/results/milestone-4-recorded-restart/README.md) keep replacement opening, replay, the first post-catch-up block and a following 4,096-message stream separate, with allocation/native-work guards and page-fault observations. They are local component costs, not independent-process capacity or a new nanomsg speedup. Optional acknowledged progress is the next workstream step. The public runtime and seventeen primer programs remain unchanged.

## Step 4.35 Optional acknowledged progress

Framework **0.4.37** implements workstream step 6's [private acknowledged consumer](../recorded-acknowledged-progress.md) over the existing recorded transport. An atomic selector commits one fixed Source/receipt after-image; applied-generation recovery repairs interrupted installation and dependent hints. Exact delivery tokens, repeated pins, release/redelivery, timestamp presence, close and predecessor cleanup retain their processing semantics without a Session gate or message append. Fresh issuance reserves a terminal generation, so counter exhaustion cannot strand its pin. Unacknowledged bindings have no receipt file.

Independent byte fixtures and actual process interruption qualify selected-only recovery, image reuse, partial receipt/Source installation, repeated interrupted repair and the connection-incarnation publication boundary. Both modes progress under the actual Session gate and disjoint paused/full/exhausted owners. Focused allocation/native-work guards and full matched nanomsg comparisons distinguish delivery latency, consumer acknowledgment-return latency and completed-publication throughput. These private one-Queue/one-schema Process endpoints precede public Task lifecycle integration and runtime admission; all seventeen public primer programs continue using their existing API.
