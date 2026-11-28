# Milestone 5 Source Queue Sets

This is the reviewed development sequence for receiving from several Queues through one consumer-owned **Source Queue Set**. A selection policy chooses the next input while preserving each Queue's order. The [current public runtime](../public-runtime-admission.md) admits only one Active input; this plan does not enable multi-input receiving or an unavailable policy.

Begin after milestone 4's current-runtime completion review, final performance qualification and integration into `main`. Create staging branch `milestone-5` from that integration, then use the step branches below for PRs targeting staging. The first milestone-5 PR uses **0.5.1**; assign later build numbers when their PRs are created under the [versioning policy](../../ROADMAP.md#versioning). Split a step if its change becomes too large to review.

The [independent messaging protocol](../independent-messaging-protocol.md) supplies the ownership and recovery foundation. The [configuration contract](../configuration.md), [public interface status](../public-interface.md) and [glossary](../glossary.md) distinguish available behavior from planned declarations. Configuration remains TOML through the common cpptoml loader.

## Scope and invariants

- Each Queue has one producer Task and one owning Source. One Task may own several Sources or send through several outgoing Queues sharing its producer log.
- Setup records a bounded, frozen membership and resolved policy before messaging admission. Existing Process/Thread claims, parent claims and fork rejection remain authoritative; this milestone does not add online topology editing.
- Keep one progress and scan cursor per input Queue, including when inputs share a producer log. A cursor identifies the already examined or delivered position for that input. Sharing one cursor between inputs can skip another Queue's messages.
- An acknowledged Source has **one Source-wide pin**: its one outstanding `Delivery` and exact token. Repeated receive returns that same delivery without selecting again. Do not implement multi-input delivery by exposing an independent pin for every input.
- An acknowledgment-free Source returns `MessageView` without a processing token. Its existing borrow ends at the next Source receive attempt, including an empty attempt, or close. Copying preserves the observed `MessageHeader` and exact decoding context.
- The consumer owns scheduling and processing progress. Normal receive, acknowledgment, release and ID/send/status work acquire no Session administrative gate and write no common message journal or Session sequencing counter.
- Transport credit permits further sends; acknowledgment certifies processing. Neither acknowledges another input nor erases accepted messages. Per-Queue order, producer-owned acceptance and immutable replay remain intact.
- Preserve the specialized one-input path. Selection cost may depend on configured input count and newly committed routing work; it must not repeatedly scan retained history or rebuild topology.

Several outgoing Queues of one producer Task still share that Task's issuer, recording owner and producer serialization. Per-input cursors do not promise independent producer ownership for Queues sharing a log.

## Review steps

| Step | Branch | Scope | Exit evidence |
| --- | --- | --- | --- |
| 5.1 | `milestone-5-1` | Review multi-input ownership, selection, progress and policy semantics. | Specify the decisions below, supported combinations, capacity/layout changes and failure/performance tests before enabling a new runtime. This is design work with no measured speedup claim. |
| 5.2 | `milestone-5-2` | Implement bounded per-input state and strict-priority selection with round-robin ties. | Public calls receive from multiple producers without reordering any input. Validate setup and unsupported combinations before mutation; qualify empty inputs, deterministic ties, one Source-wide pin and the one-input fast path. Run focused selection-cost checks and matched end-to-end measurements. |
| 5.3 | `milestone-5-3` | Qualify multi-input acknowledgment, release, close, restart and independence. | Actual interruption repairs committed progress and derived hints for every input. Stale tokens reject, pins redeliver correctly, credit never returns twice, and unrelated established connections remain usable. Measure recovery separately from resumed messaging. |
| 5.4 | `milestone-5-4` | Implement proportional-budget fairness. | Implement the reviewed budget units, reset and empty-input rules with exact reference cases. Demonstrate configured service shares and bounded selection work; measure per-input latency and throughput under skewed loads. |
| 5.5 | `milestone-5-5` | Implement weighted-random selection. | Implement reviewed weight, effective-seed, draw and restart rules. Exact fixtures verify reproducible choices under specified readiness; appropriate statistical checks cover expected shares without asserting a starvation bound that the policy does not provide. Measure selection cost and end-to-end behavior. |
| 5.6 | `milestone-5-6` | Implement configured allowed loss under the producer-owned recording protocol. | Qualify visible gap ranges, protected issued pins, mixed delivery policies, replay, credit and interrupted loss progress. Programmatic and TOML admission agree. Measure delayed-consumer catch-up and live costs against recover-from-log controls. |
| 5.7 | `milestone-5-7` | Finish the public multi-input example, primer and qualification. | Independent producers feed one Source through actual public APIs. All supported selection/delivery/acknowledgment combinations pass order, progress, replacement and capacity checks. Complete Linux-first performance evidence, Mac regression coverage and the milestone completion review before integration. |

Every implementation PR runs either a focused performance unit test or a full matched benchmark against nanomsg, appropriate to its change. A new usable messaging path or material delivery-policy change requires end-to-end latency and verified throughput. Reuse existing meaningful controls; do not postpone all performance evidence to step 5.7.

## Step 5.1 Decisions to review

The policy names and configuration vocabulary already exist. Detailed scheduling rules are not implied by those names. Resolve the following in the design PR before implementation:

| Area | Required decision |
| --- | --- |
| Strict priority | Numeric priority ordering, when readiness is sampled, tie rotation, and whether an empty input retains its rotation position. Explain that continuously ready higher-priority inputs can starve lower-priority inputs. |
| Fair budgets | A budget's unit, its relationship to Queue priority, replenishment/reset rules, treatment of idle inputs and unused allowance, and checked arithmetic at limits. State whether fairness applies to message count, bytes or another explicit unit. |
| Weighted random | Priority-to-weight mapping, eligible input set, effective seed, random algorithm/state, draw consumption on timeout/error, and state preservation or reset after close/replacement. Retain the recorded effective seed behavior in the configuration contract. |
| Scheduling transitions | Which successful operations advance rotation, budgets or random state; how release/redelivery and failed/uncertain operations affect them. Repeating a pinned receive must not select again. |
| Source progress | One authoritative Source pin/scheduler state, per-input receipt/progress rows, bounded operation images, admission reservations and interrupted hint repair. Define the normal operation cost and any reset/recovery cost separately. |
| Allowed loss | Loss eligibility, producer admission/credit at `QueueFull`, exact gap representation, progress repair and protection of already issued messages in both acknowledgment modes. Distinguish skipped progress from certified processing watermarks. Review its storage needs before reserving the multi-input layout, then implement/qualify it in step 5.6. |
| Configuration and admission | Valid priority/seed ranges and unsupported combinations; bounds on memberships, reserved bytes and mappings. Define required native format/features before mutable access. |

The current configuration contract permits strict priorities 0–255, fair/random priorities 1–255, and a recorded effective seed for weighted random. Any proposed change to that public vocabulary must be explicit in the design review and accompanying TOML/error examples.

The design must distinguish ordinary empty inputs from an input that requires recovery or has failed validation. Specify the Source's error and settlement behavior; do not silently skip a corrupt input to claim progress. Preserve independently owned connections without promising that one consumer can continue a Source whose own authoritative progress is invalid.

Before 1.0, a necessary breaking layout change requires fresh development Sessions and explicit version rejection. This step does not introduce old-format readers or a parallel transport engine.

## Progress and recovery qualification

A **Source operation image** records one consumer operation's selected after-state so a replacement can finish an interrupted installation. Extend that mechanism for bounded scheduling changes and the selected input's progress; avoid copying every input's full ledger on every message. The design review fixes the required layout and commit boundaries before step 5.2 exposes multi-input behavior.

Step 5.2 needs basic pin, token and progress correctness, including interruption checks on both sides of new commit points, to expose a usable Source. Step 5.3 expands actual process-interruption and failure combinations; it does not defer correctness of the first implementation.

At minimum, the tests demonstrate:

- Per-Queue FIFO order when several producers are ready, when inputs become empty, and when several inputs refer to one producer log. No input skips another input's committed records.
- One outstanding acknowledged delivery across the whole Source. Receive repetition, release, acknowledgment, close, replacement and owning copies retain their current header/lifetime rules.
- Monotonic credit and separate committed processing watermarks per input. Acknowledging one input does not certify another; repeated recovery cannot return the same transport credit twice.
- Interruption before and after selection/progress commitment, installation and hint publication. Recovery repairs every owned input's derived credit/acknowledgment hints, including inputs absent from the last selected operation image.
- Producer exit or pause before/after acceptance and descriptor exposure, a slow/full input, full message storage, stale claims, parent/Thread ownership and counter exhaustion. Measure affected-owner recovery separately from unrelated live messaging.
- Honest policy behavior: strict priority's permitted starvation, exact fair-budget transitions and weighted choices under specified readiness. A seeded selector alone does not reconstruct the original availability or execution order; optional execution tracing remains separate work.

## Allowed loss is delivery policy

The retired engine implemented an earlier allowed-loss protocol. The admitted format-3 runtime rejects `AllowLoss`; its reimplementation is an explicit milestone-5 obligation, not a completed feature inherited from that engine.

Allowed loss permits a consumer to skip eligible stale deliveries and report a **gap**, an explicit Queue sequence range that was skipped. It does not imply message acceptance without recording or erase accepted messages from replay. Selecting `acknowledgment = "none"` does not enable loss.

Current logical credit rejects a new send when a Queue is full; it prevents unconsumed delivery hints from being overwritten by further accepted sends. The loss design must explicitly review producer admission and consumer loss/credit commitments, rather than adding consumer skipping to unchanged full-Queue behavior. Review how acceptance can continue without the producer writing consumer-owned credit, and qualify descriptor-generation/read safety and interrupted loss settlement under those rules. The current contiguous acknowledged-position assumptions also need review: delivery resolution after a gap and certified processing progress are distinct. Skipping a message must not falsely certify that the application processed it.

Step 5.6 must define and qualify gap ordering alongside ordinary messages, separate Queues' loss ranges, protected issued pins, loss/release/restart races and full-log progress. Recover-from-log remains the default. Keep unsupported combinations rejected until their implementation and tests are complete.

Returning credit, acknowledging or skipping delivery still does not reclaim the producer's reserved log. Growth, rotation, retention and reclamation remain milestone 8, with their own replay/pin lifetime contract. Do not claim unlimited operation from a finite stream test.

## Performance plan

Linux latency and completed-message throughput drive decisions. Keep macOS correct and measure enough regression cases to assess whether its performance remains acceptable. A repeatable substantial regression needs explanation and review; an improvement on every platform, payload and policy is not required.

Freeze the final milestone-4 public one-input measurement as the regression control. Continue the [end-to-end methodology](../end-to-end-qualification.md) and [public-call measurement rules](../public-runtime-performance.md), keeping recording and acknowledgment differences visible in comparisons with nanomsg.

| Dimension | Required coverage |
| --- | --- |
| Input count | 1, 2, 4 and 8, within declared Session capacity; retain one-input cost and latency as an explicit control. |
| Readiness/load | All inputs ready, mostly idle inputs, uneven producer rates, bursts and delayed consumers. Include enough lower-priority traffic to observe its latency and service share. |
| Producer layout | Distinct producer Tasks/logs and several input Queues sharing one Task/log. Measure routing scan work rather than assuming independent per-input cursors eliminate duplicated scanning cost. |
| Processing/delivery | Required and no acknowledgments; default recover-from-log, then supported loss cases after step 5.6. Keep transport delivery, processing completion and skipped deliveries separate. |
| Policy | Equal and unequal priorities, proportional budgets and random weights, with exact configuration and readiness assumptions recorded. |
| Resources/recovery | Capacity/full-log cases and cold recovery/catch-up, reported separately from warm messaging and selection. |

Lead reports with verified aggregate throughput and **per-input** delivery latency, including the sample-supported tails. Also record per-input service shares/starvation, processing-completion latency where applicable, CPU, polling/scan work and recovery time. Aggregate throughput alone can hide a starved Queue.

Nanomsg is a transport comparison; its buffering, unrecorded/unacknowledged delivery and input-selection behavior do not establish the same policy guarantees. Keep compatible one-input controls and clearly label multi-input workload/semantic differences. Preserve the historical ID/send/status/receive workload if adding a normal successful-send workload without unconditional status lookup; a changed workload is not a measured code speedup.

Use deterministic allocation, bounded-scan and no-Session-gate guards in ordinary CI where they express the required local cost. Retain the existing guards requiring zero direct `getpid` calls and no repeated native metadata work during warmed messaging. Hosted timing measurements retain every attempt and observed range, without noisy wall-clock pass thresholds. Store bulky raw evidence in the [maintainer archive](../../benchmarks/results/README.md), retaining reports, plots, source/configuration provenance, hashes and restore instructions in Git.

## Programmer path and completion

Maintain the [primer](../primer.md), current API status, TOML examples, policy documentation and teaching tests as each step becomes usable. New programs use public Angaria calls directly, explicit configuration and checked results. Compile-only declarations must remain identified as planned rather than presented as runnable features.

The final example adds several independent producers to one consumer's Source, demonstrates supported selection policies, then exercises replacement and the distinction between delivery gaps and processing acknowledgments. Keep its bounds and expected order/share assertions explicit. Extend the existing multi-process example instead of requiring the full milestone-6 Publishing application early.

Completion requires the supported policy matrix, per-input order/progress/recovery evidence, reviewed Linux results and Mac regression evidence, the runnable public example, and the primer review. The final exact commit passes Linux x86_64, Linux ARM64 and macOS ARM64 CI; macOS x86_64 remains request-only. Integration into `main` uses a separate PR and the next assigned milestone-5 build number.

Publishing fan-out and the complete five-process application remain milestone 6. Additional waiting/notification policies remain milestone 7. Storage growth/rotation/reclamation remain milestone 8, advanced Fields milestone 9, and complete tools/replay/optional execution traces milestone 10. None is silently enabled by this plan.

[Project roadmap](../../ROADMAP.md) · [Current public runtime](../public-runtime-admission.md) · [Independent messaging protocol](../independent-messaging-protocol.md)
