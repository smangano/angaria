# Native two-plane transport contract

**Step 4.1, framework version `0.4.1`, is a design review.** This document specifies planned milestone-4 behavior. It does not enable native send, receive, acknowledgment, new native formats, or the API additions below. Current runnable behavior remains the thirteen [milestone-3 primer lessons](primer.md); the [implementation sequence](milestones/milestone-4.md) introduces this contract in small PRs.

The [mapped format](mapped-format.md) defines stored representations, and the [concurrency contract](concurrency.md) defines journal commits and process recovery. This contract fills their native-delivery details without changing the agreed one-producer-per-Queue model or the blocking session mutation gate.

## Storage and acceptance

A **Publication** is one immutable message plus its original destination snapshot. A **receipt** is the reserved mutable record of one destination's delivery progress. A **delivery link** points to an accepted destination row and its predecessor for that Queue. These links retain Queue order independently of its bounded ring index.

The two planes are the append-only publication/history storage and fixed-capacity rings containing references to it. A ring slot is a cache of a committed delivery, not its acceptance authority. A full ring is overwritten and is not `QueueFull` or a pending-delivery limit. An offline consumer is legal.

Recover-from-log delivery follows retained links when a ring entry has been overwritten. Allowed loss can instead commit skipped ranges, subject to pin protection below. Neither policy reclaims physical history in milestone 4. At `max_log_bytes`, new publication allocation can return `LogFull`; acknowledgment does not make append space reusable. Retirement/reclamation follows milestone 8.

## Native compatibility and required capabilities

A **required capability** is a bit in the immutable superblock required-feature mask. A live binary must understand every required bit before it acquires a usable controller or performs journal recovery. It is distinct from a framework version string and from the Session definition's policy metadata, which is read later.

The reviewed future profiles are:

| Native format | Required mask | Meaning |
| --- | --- | --- |
| 2.0 | `0x1` | Existing journal/scalar profile. |
| 2.1 | `0x3` | Existing journal plus basic logical Fields. |
| 2.2 core | `0x7` | Existing capabilities plus `native_delivery_v1 = 0x4`. |
| 2.2 allowed loss | `0xf` | Core plus `allow_loss_v1 = 0x8`. |

Every listed format requires the journal bit. The basic-Field bit is absent on 2.0 and required on 2.1/2.2. Both new delivery and allowed-loss bits are invalid on 2.0/2.1; format 2.2 requires native delivery, and its allowed-loss bit implies that capability. Reject noncanonical combinations such as 2.1/`0x7`, not merely unknown bits. Unknown required bits, unsupported minors, and incompatible ABI/protocol profiles reject live attachment before recovery or writes. Existing required bits retain their meanings.

Native Publication and Ring record envelopes keep encoding 1; embedded destination and receipt rows keep their 48-byte and 128-byte sizes. The synchronization protocol remains 1, and fixed section sizes and ABI identifiers remain unchanged. Portable message-record encoding also remains 1. The new minor/capabilities protect previously unused delivery state and newly executable redo semantics; they do not make native structures portable between live ABI profiles. Later selection, Publishing/retry, and waiting capabilities require their own admission review; the milestone-4 profile does not advertise those operations.

Step 4.1 changes no capability constants or creation behavior. Private codec/validation fixtures can exercise 2.2 in subsequent steps. Public creation switches to the core profile only after its codecs, loaders, redo, and claim cleanup are complete, with direct send in step 4.6. Allowed-loss creation is enabled in step 4.8 only after its validation/replay/cleanup is complete. From that implementation step, public Session factories create `0xf` Sessions even when their initial Queues use recovery delivery, allowing later allowed-loss Queue registration.

New binaries retain Registry, claim, payload, capture, and inspection support for 2.0/2.1. Native send/status/receive/acknowledge/release methods on those Sessions return `UnsupportedPolicy` without starting new delivery work. Closing an existing identity-only Source handle remains valid resource cleanup; a legacy Session has no native issuance to release. A 2.2/`0x7` Session remains usable for recovery delivery but rejects allowed-loss defaults/Queue registration without writes. Loss-bearing state without its required capability is invalid. An earlier core-only binary rejects a `0xf` Session before encountering a loss journal.

Capabilities are bound at creation. Do not promote an existing header in place: the required-feature word is immutable and is not a journal mutation target. To use a capability absent from an existing Session, create a new appropriately configured Session; generalized migration remains later tooling. Current 2.0/2.1 data is not silently rewritten.

## Message admission and publication identity

Direct send uses the Active Queue owned by the acting Task, one destination, and the Queue's committed definition revision as topology context. Its caller-issued `PublicationId` identifies Session, producer Task/incarnation, and publication sequence. Fresh sends require the current live claim and a positive sequence already issued by that claim.

Before preparing a fresh acceptance:

- Validate ownership, capability, exact registered schema, known payload format, caller byte span, counter bounds, and the destination-output buffer. Direct send needs capacity for one destination result.
- Validate Binary payloads against the exact registered layout, including size and Boolean/closed-enum/fixed-ASCII constraints. Preserve the existing scalar value rules, including valid ordinary char bytes and IEEE NaN bit patterns. Use a checked validation walk rather than materializing decoded values solely for validation. Known JSON/XML/CSV/Text formats remain opaque application bytes; this adds no serializer or syntax validator for them.
- Compute the entire allocation: envelopes, fixed bodies, destination rows, payload, all receipt slots, manifest/reference/redo data, and alignment. Maximum sizes are ceilings, not eager reservations of that amount of process memory.
- Resolve timestamps, sequences, and absolute redo values once. Commit Publication, receipt initialization, Queue links/roots, and ring updates as one existing journal operation.

The native Publication's payload slice contains application bytes alone. Do not put the portable 80-byte header inside that payload. Its logical header is represented by Publication fields plus Session context; generalized native export remains milestone 10 work.

An identical repeat of an already committed current-claim ID returns its recorded outcome without another delivery. Resolve that existing publication before fresh allocation/counter checks, so a full log or exhausted fresh counters do not invalidate an identical repeat. Endpoint, exact schema, format, flags, and bytes must match; disagreement returns `Conflict`. Caller output-buffer failure is an operation/lookup error and cannot turn an earlier acceptance into rejection. Check `PublicationReport::_error` before interpreting its status.

`Task::publication_status` settles recovery before lookup and permits retained earlier incarnations of that same logical producer. Under milestone 4's no-reclamation model, provable absence reports rejected with an empty destination view. An old incarnation cannot use an absent ID to create a fresh send. Reclamation and `HistoryUnavailable` retirement fences remain milestone 8 obligations.

Known rejection adds no publication, receipt, Queue sequence, or log allocation. Recovery of an earlier operation can legitimately repair existing state before a new request is rejected. Precommit failures with proven rollback create no new obligation. An unresolved interrupted result is `OutcomeUnknown`; a postcommit failure never becomes a known rejection. The caller retains its publication ID for recovery lookup.

## Timestamp meanings and public view

Sample initial publication UTC time once when preparing the accepted Publication. Direct send uses that same sample for its destination enqueue time. A future retry retains the original publication time while recording its own newly accepted destination enqueue time. New delivery issuance samples dequeue time; repeated receive preserves that issuance's time. Acknowledgment/loss samples its terminal time once. Replay reinstalls recorded values and never reads the clock again.

UTC values can be zero or negative, and wall clocks can move backward. Presence flags establish whether a stored time exists; zero alone does not mean absent. Wait deadlines use the monotonic clock and have no relationship to these UTC fields.

Step 4.7 plans an appended `DeliveryView::_publishedAt` member of the same UTC chrono type as `_enqueuedAt` and `_dequeuedAt`. They respectively expose initial Publication time, this destination's enqueue time, and this issuance's dequeue time. The new member is process-local API metadata, not a native struct layout or an encoding-version change. It is not added to headers in step 4.1.

## Ring generations

Keep each existing 32-byte slot as four `u64` words: Queue sequence, delivery-link reference, Publication reference, and **generation**. The previously provisional “generation/state” word has this precise meaning under the delivery profile:

- Empty slot: all four words are zero.
- For accepted Queue sequence `q > 0` and ring capacity `C > 0`, the slot index is `(q - 1) % C` and generation is `1 + (q - 1) / C`.
- Occupied slots have nonzero references validated against their committed destination/Publication and Queue ownership. A cached terminal receipt is not a pending delivery.
- Use the complete 64-bit generation, without a packed valid bit or a hidden 63-bit sequence limit. Capacity 1 with the maximum valid sequence must remain representable.

Read/write mutable slots under the session gate through the native atomic adapter. Recovery completes before interpreting them. Generation is not a second commit protocol; journal and committed Publication evidence establish acceptance.

The Queue's authoritative last assigned sequence `N` determines its current ring window. If `N = 0`, all slots are empty. Otherwise the window starts at 1 when `N < C`, or at `N - C + 1` when `N >= C`, and ends at `N`. Validate exactly those latest `min(N, C)` committed links against their slots/generations. Allowed loss may skip only a pending prefix below this window after pin checks. An arbitrary cache miss, stale slot, or corrupted reference is not proof that a delivery may be lost.

## Receipt fields and presence

Keep the reserved 128-byte receipt size. Offsets below are relative to its start. The second 64 bytes and the word at 56 remain zero/reserved in this profile.

| Offset | Stored value |
| ---: | --- |
| 0 | Receipt state (`u64`): Unused 0, Pending 1, Issued 2, Acknowledged 3, Lost 4. |
| 8 / 10 / 12 | Consumer Task (`u16`), Source (`u16`), presence flags (`u32`), installed as one atomic word. |
| 16 / 24 | Consumer incarnation and issuance (`u64` each). |
| 32 / 40 / 48 | Latest dequeue, acknowledgment, and terminal UTC nanoseconds (`i64` each). |

Presence bits are `0x1` for last consumer/issuance/dequeue metadata, `0x2` for acknowledgment time, and `0x4` for terminal time. Other bits are invalid. Wherever `0x1` is present, all consumer Task/Source/incarnation/issuance tags must be nonzero and valid for that captured delivery; when it is absent, those tags and the dequeue word are zero. Absent timestamp words are zero; epoch zero with a present bit is valid.

| State | Required combination |
| --- | --- |
| Unused | Entire receipt zero; no delivery obligation. |
| Initial Pending | State 1; remaining fields zero. |
| Issued | Presence `0x1`, complete consumer/Source/incarnation/issuance tags, no acknowledgment/terminal time. Exactly one matching Source slot establishes the pin. |
| Pending after release/cleanup | Retain the last consumer/issuance/dequeue tags and `0x1`; no current pin. |
| Acknowledged | Presence `0x7`; acknowledgment time equals terminal time. |
| Lost | Presence `0x4` or `0x5` when retaining historical dequeue tags; acknowledgment absent. Terminal time is loss time. |

Release and claim cleanup preserve historical tags so repeated `ReleaseClaim` replay can rediscover matching receipts. Terminal receipts remain terminal. An acknowledgment never becomes loss, and loss never revokes an Issued pin. Compact loss replay reinstalls both state and metadata for already Lost rows before advancing the absolute terminal watermark.

## Delivery views and payload mapping

A **delivery pin** is the persisted Issued receipt and matching Source in-flight slot protecting that delivery. A mapping owner separately keeps its read-only address range alive in the receiving process. A copied `DeliveryView` owns neither resource; its payload span borrows them.

Establish the pin and mapping before returning a view. If mapping acquisition fails, do not expose an unowned span or silently consume a delivery; settle or recover its reserved progress. Prepare local cleanup identity independently of the returned view: an issuance committed before a later I/O/mapping/result failure must remain discoverable by close/destruction even when no `DeliveryView` reached the application. Keep the current mapping while that issuance remains in flight. Repeated receive returns the same token/time and must preserve previously returned spans for that pin.

Append growth and other producers' ring overwrites must not move or invalidate these bytes. Existing mapping leases permit growth while preventing local shrink below exposed ranges. Rollback cannot truncate committed pinned bytes; no other participant may receive provisional append bytes. An empty payload still has a receipt pin but needs no zero-length native mapping.

Successful acknowledgment/release, valid Source close, replacement of a destination handle during move assignment, and ending the claim end the relevant borrowed-view lifetime. Stale-token or other proven unchanged-state rejection preserves the current pin. If progress becomes uncertain, stop using the old view and settle recovery/re-receive before borrowing again. Application processing occurs outside the mutation gate; callers coordinate it with operations ending their views.

## Explicit close and deferred cleanup

Step 4.7 plans `OperationResult SourceQueueSet::close()`. Current declarations are unchanged in step 4.1. This gives a caller an observable cleanup result while its Task remains alive.

The local lifecycle is **Active → Closing → Closed**. Validate process/thread ownership before entering Closing. Starting a valid close ends that handle's views and prevents further receive/acknowledge/release through it. Only close retry remains available. Wrong-owner rejection before Closing changes nothing.

Successful close releases the exact current issuance to Pending if one is present, clears its Source pin through reserved FixedState work, and then relinquishes the local exclusive opening/mapping. An open Source without an in-flight delivery closes successfully too. Preserve the Source issuance counter; a later delivery increments it. Closing an already closed or moved-from empty handle is an idempotent no-op.

A failed explicit close returns its error, retains the Task claim and Source reservation, and leaves the handle Closing. A second opening cannot bypass it and returns `OwnershipBusy`. Retrying close first settles recovery and recognizes an already completed cleanup without another transition.

For a valid local owner, destruction and noexcept move assignment attempt the same cleanup without throwing. If it fails, turn the reservation allocated on open into **DeferredClose**: fixed old Source/claim/issuance identity that must be cleaned before that Task performs further delivery/progress or reopens that Source. Include any unreturned issuance discovered during preparation/recovery, not only the last successfully returned view. Recording this marker must not allocate and must not create an owning cycle back to the old Source state. Reconciliation either completes exact old cleanup or returns an error; it never grants a handle containing the abandoned token or clears a newer issuance. Ending the last Task owner still releases its OS claim; replacement's extended `ReleaseClaim` handles remaining persisted state.

Move construction transfers the current pin/mapping and preserves its views. Move assignment ends the destination's previous view lifetime, deferring old cleanup if necessary, while preserving the incoming views under their transferred owner. Take the mutation gate before endpoint-bookkeeping locks, or release bookkeeping before waiting for the gate. Drop final Task-owner references only after releasing the operation gate, avoiding recursive detach. Inherited-process handles fail ownership checks before touching inherited mutexes.

## Source scope, loss, and waiting

Milestone 4 uses the existing Source APIs. Fresh selection supports at most one Active member Queue. Zero Active members can time out; more than one returns `UnsupportedPolicy` before fresh selection or loss progress. Draft members are not eligible. If a Source gains another Active Queue while a delivery is in flight, repeated receive still returns that token, and acknowledgment/release/close remain available. Multiple-Queue selection follows milestone 5.

Choose the oldest eligible pending Queue sequence, using either the current ring entry or its retained history. One Source owns at most one in-flight delivery. Re-reading it performs no new issuance or selection. Acknowledgment checks every token component, records its receipt, advances only contiguous acknowledged/lost progress, and clears the pin. Release clears the pin and returns that receipt to Pending without acknowledging it. Source close/reopen and consumer replacement must not reuse the full old token.

With the allowed-loss capability, a later receive may commit a `DeliveryGap` for an overwritten pending prefix after any issued pin has ended. Gap/timeout results allocate no delivery slot. A crash after loss commit can hide that gap result from the caller, but cannot resurrect lost deliveries. Recovery-delivery Queues never lose messages merely because their ring wrapped.

Use the existing Spin/deadline contract: reject negative/overflowing timeouts; zero makes one immediate availability attempt; positive timeouts use checked monotonic deadlines and bounded gate-acquisition attempts. Existing in-flight delivery is returned after recovery; expired positive deadlines do not issue a fresh delivery. Release the gate between checks and while the application processes payloads. Recovery and a started mutation still finish consistently, so this is not a hard-real-time guarantee. Eventfd remains milestone 7.

## Recovery and implementation handoff

Current topology validation rejects publication-bearing state, and current Task cleanup rejects receipts/nonzero delivery state. Complete delivery-aware validation, replay, and Task/Source cleanup before public send. Publication recovery settles prepared/committed batches before any observer uses their roots, ring entries, receipts, or borrowed bytes. Ending a producer claim never retracts its accepted Publications; receipt cleanup concerns deliveries issued to that Task as a consumer.

Receipt issuance, acknowledgment, release, close, replacement cleanup, and loss progress use reserved FixedState storage without append allocation, including when the publication log is full. All compact scans use retained committed definitions and fixed old identities, not pointers already cleared by an earlier replay pass. Preserve issuance counters across Source closure, never wrap counters silently, and reject stale tokens without progress.

Every mutating step includes crash cuts and injected I/O failure around prepare/commit/apply; later qualification composes them with wraparound, full logs, repeated interrupted recovery, ownership replacement, empty payloads, and maximum counters. Verify rejection in a quiescent recovered Session separately from repair of an earlier operation. Tests explain public usage separately from private corruption/fault fixtures.

The existing global mutation gate remains a correctness baseline for the small bounded topology. It can block behind a paused live owner. Borrowed immutable payload reads need no gate after pin establishment. No lock-free, wait-free, starvation-free, power-loss-durability, or hard-real-time promise is added. Step 4.11 measures allocation/commit contention alongside latency and throughput.

[Milestone 4 steps](milestones/milestone-4.md) · [Mapped format](mapped-format.md) · [Concurrency/recovery](concurrency.md) · [Public API](api.md)
