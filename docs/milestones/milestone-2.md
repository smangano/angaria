# Milestone 2 Shared persistent Registry

Milestone 1's architecture and policy contracts reached `main` through [PR #7](https://github.com/smangano/angaria/pull/7). Milestone 2 implements the shared persistent Registry against those reviewed contracts. Steps 2.1 through 2.5 are integrated into `milestone-2`; step 2.6 adds public Task registration. Its framework version is `0.2.5` under the [per-PR versioning policy](../../ROADMAP.md#versioning).

Completion requires independent processes to register and discover Tasks, Fields, immutable message schemas, Queues, Source Queue Sets, and Publishing Queue Sets; recover interrupted updates; and inspect the persisted Registry after application shutdown.

The staging branch `milestone-2` starts from the merged `main`. Each focused step uses `milestone-2-<s>` and a PR targeting `milestone-2`. After all steps are reviewed and merged, a milestone PR integrates `milestone-2` into `main`.

## Review steps

| Step | Branch | Scope | Completion criterion |
| --- | --- | --- | --- |
| 2.1 | `milestone-2-1` | Checked fixed-section planning, format-2 [superblock](../mapped-format.md#superblock) encoding/decoding, and metadata compatibility checks. | Golden-byte and malformed-input tests validate the 4,096-byte superblock prefix and section directory without native struct serialization. Decoding remains separate from live session validation. |
| 2.2 | `milestone-2-2` | Native file and mapping [RAII](../glossary.md#encoding-and-native-resources), checked ranges, file-size validation, and controlled resize. | File/mapping lifetimes and failure paths work on supported hosts; truncated files and invalid mapping ranges fail before access. |
| 2.3 | `milestone-2-3` | Shared session controller, mutation gate, per-Task claim primitives, and native atomic/profile qualification. | Independent processes and threads obey claim/gate exclusion; unsupported profiles fail before mutation. Supported hosts pass native ordering, descriptor-lifetime, and process-termination checks. |
| 2.4 | `milestone-2-4` | Reserved operation journal, append manifests, redo/rollback, and native fault injection. | Interrupt actual stores and I/O at prepare, commit, apply, and recovery boundaries. Recovery is repeatable, preserves committed work, and discards only uncommitted work. |
| 2.5 | `milestone-2-5` | Session bootstrap/create/attach, creator Task, effective options, global metadata, and fixed-state claim/incarnation/issuance operations. | A created session records its creator and settings; an independent process can attach compatibly. Claim replacement and issuance persist without append allocation, including at `LogFull`. |
| 2.6 | `milestone-2-6` | Task registration and lookup, stable IDs/name indexes, idempotency, conflicts, capacities, and historical revisions. | Processes discover consistent Task identities and metadata; exact re-registration is idempotent, and conflicting/exhausted updates preserve state. |
| 2.7 | `milestone-2-7` | Field definitions and immutable versioned message schemas. | Registration preserves full 32-bit Field IDs, ordered Field lists, exact type/version identities, metadata, and retained definitions across new schema versions. |
| 2.8 | `milestone-2-8` | Draft Queues and both Queue Set kinds, typed lookup/indexes, ownership, and membership registration. | Processes discover every entity kind and consistent relationships. Queue Set IDs share their bounded namespace while typed lookups preserve their kinds. |
| 2.9 | `milestone-2-9` | Draft adoption, topology updates, activation/resource checks, and endpoint identity/ownership handles where dependencies permit. | Updates commit one consistent topology revision, preserve history, and enforce active ownership rules. Queue activation reserves its ring geometry; unavailable resources cannot produce an active entity. |
| 2.10 | `milestone-2-10` | cpptoml TOML loading, semantic preflight, common programmatic validation, and file-based Session factories. | The trading configuration creates the equivalent Registry through the common path. Invalid values/references/policies/capacities fail with key-path diagnostics before initial updates are applied; supplied attachment settings match recorded effective configuration. |
| 2.11 | `milestone-2-11` | Independent-process qualification, read-only Registry inspection, and a Registry walkthrough. | Demonstrate registration/discovery of every entity kind, concurrent updates, restart and interrupted-update recovery, historical inspection, and a reproducible walkthrough on supported profiles. |

Each PR implements its listed behavior and adds checks appropriate to that change. Later runtime methods remain declarations until their implementation step; these increments do not substitute placeholder behavior for unfinished operations.

## Step 2.1 Binary foundation

The [fixed-layout planner](../../include/angaria/detail/mapped_format.h) calculates all thirteen sections, their checked byte counts and offsets, and the arena start from effective `CapacityLimits`. The existing reservation helpers use that same plan so byte accounting has one source of truth.

The **superblock** is the 4,096-byte session control area at the beginning of `session.ang`; its [definition and layout](../mapped-format.md#superblock) specify its contents. A **codec** converts between process-local values and encoded bytes. The internal [superblock codec](../../src/mapped_superblock.h) implements three operations:

- `encode_initial_superblock()` produces a provisional 4,096-byte superblock prefix containing the immutable configuration and planned section directory, with initial control/journal values. It does not allocate the remaining sections or create/publish a session file.
- `decode_superblock()` reads immutable configuration and validates the directory against its capacity-derived layout. It accepts exactly format 2.0 and rejects truncated, malformed, or inconsistent metadata before forming references into other storage.
- `check_live_compatibility()` compares decoded metadata with caller-supplied ABI/protocol, required-feature, storage-support, and optional capacity-ceiling capabilities. It does not qualify the caller's native adapter.

The codec writes explicit little-endian integers and returns process-local values. It preserves opaque ABI/profile and feature metadata for portable inspection, including foreign native profiles and unknown optional feature bits. Live compatibility is a separate check; decoding bytes is not permission to mutate them.

Mutable roots, live state, and the reusable operation journal are deliberately outside this metadata decoder's validation. Their bytes may reflect a pending transaction or a previously used Idle journal. Later attachment must acquire the proper gate, recover when required, and validate resulting state before exposing usable Session or Registry handles.

Step 2.1 tests use an independently authored byte fixture, round trips, and invalid cases for format markers, lengths, capacities, section geometry, overflow, reserved fields, and compatibility metadata. They also verify that mutable root/journal bytes do not turn this portable decoder into an incomplete recovery routine. Native file I/O, mappings, atomics, gate ownership, and journal repair follow in steps 2.2 through 2.5.

## Step 2.2 Native file and mapping ownership

The [native storage contract](../native-storage.md) specifies the implemented `NativeFile` and `MappedRegion` primitives. They own native descriptors and mappings through move-only RAII, check file type/length and requested ranges, support read-only/shared writable views, and perform explicit bounded resize. The [glossary](../glossary.md#encoding-and-native-resources) defines descriptors, mapped regions/views, pages, positional I/O, and mapping leases.

Process-local lease state is shared by device/inode identity across independent opens and hard links. It prevents resizing a file below the end of any live exposed mapping, while growth leaves existing view lengths unchanged. A mapping retains its file state after the original file wrapper closes.

This resource layer does not acquire the session mutation gate, synchronize access through raw mapped spans, validate/recover session contents, or prevent arbitrary external truncation. Its initial owners and initialized factory state are process-bound; a child inheriting that state requires exec before factory use. Multithreaded fork requires exec or `_exit` before library use or inherited-owner cleanup. Steps 2.3 and 2.4 add the native controller/ownership qualification and journal integration before public Session behavior is exposed.

## Step 2.3 Native coordination and atomic access

The private [session controller](../../src/session_controller.h) coalesces process-local handles by session/file identity, owns the local mutex and stable coordination descriptors, and issues move-only mutation guards and shared Task leases. The [native storage contract](../native-storage.md#shared-controller-and-mutation-gate) defines their lifetimes, alias checks, lock order, nonblocking probes, and fork/exec boundary. Lock descriptors inherited through `fork` are closed in the child without unlocking the parent's shared open description.

The [atomic adapter](../../src/native_atomic.h) checks native profile prerequisites, validates aligned eight-byte ranges, and supplies acquire/release word access, control publication with a following full fence, and gate-boundary fences. Independent-process tests exercise publication of ordinary body bytes before a control word and a following file-growth effect, and termination during repeated eight-byte stores. These bounded checks qualify observed native behavior on the tested host; they do not prove all hardware schedules, certify remote filesystems, or promise power-loss durability.

The controller opens existing coordination resources and validates immutable superblock compatibility before mutation. It does not itself recover a journal, install a persisted Task incarnation, validate Registry Task-parent relationships, or expose a usable public Session. The separate step 2.4 journal implements transactions/recovery; step 2.5 integrates both into Session bootstrap and attachment.

## Step 2.4 Native journal transactions and recovery

The portable [journal codecs](../../src/journal_format.h) encode and validate the reserved header, all redo command frames, arena envelopes, and append manifests. The private [mutation journal](../../src/mutation_journal.h) executes inline FixedState updates and manifest-backed Append batches in two-plane storage under an existing mutation guard. Its [native contract](../native-storage.md#journal-transactions-and-recovery) defines snapshots, stale-plan rejection, byte accounting, acceptance, and repeated recovery after process termination.

The engine validates complete redo streams against declared mutable word ranges before commit and before replay. It owns the final allocation/accounting, Registry-revision, manifest, and completed-generation root assignments. An append includes every record and its manifest in the byte-limit check; fixed-state updates use only the reserved journal and can proceed at `LogFull`. The initial implementation scans retained committed history to check framing and manifest chains, without reclamation or a performance claim.

The [portable tests](../../test/test_journal_format.cpp) use independent byte fixtures and malformed inputs. The [native tests](../../test/test_mutation_journal.cpp), with an [independent process worker](../../test/native_journal_worker.cpp), exercise actual write/file-growth boundaries, repeated recovery, partial redo/final assignments, and I/O failure handling. These checks cover process termination on a running coherent local filesystem, not power-loss durability or every possible hardware schedule.

This increment implements byte framing, allocation/recovery, and allowed write targets. Registered Task/schema identities, live actor claims, and definition/reference contents require the Session/Registry/domain validators. Step 2.5 adds the Task-only ReleaseClaim executor; ResetBudgets, LoseRange, and delivery/receipt cleanup remain for their owning domain implementations. Symbol-value records, SymbolIndex/StringPool updates, and segmented storage remain unsupported.

## Step 2.5 Public Session and Task lifetimes

`Session::create()` records a format-2 Session definition and its creator Process Task in one bootstrap Append, installs the creator's first claim and Session readiness through FixedState, then publishes the completed directory with an atomic rename that refuses to replace an existing path. It initializes only an exclusively created sibling directory, removes that directory on ordinary failure, and preserves an existing target. A process killed before publication can leave an unpublished `.angaria-create-*` directory; attachment never treats it as a published Session or repairs another creator's staging directory.

`Session::attach()` validates native compatibility, acquires the shared mutation gate, recovers pending journal work, and validates Session metadata, Task indexes/definitions, and Task state before returning a handle. Metadata includes the original creator, UTC creation time, effective OS user, framework version from `VERSION`, and optional application version. The recorded directory remains diagnostic; `options()._directory` reports the actual canonical attachment directory after relocation. Session attachment itself claims no Task.

The implemented Registry read paths are `find_task`, `describe_task`, Task `metadata`, and `task_status`, initially exposing the creator registration. Step 2.6 adds general Task registration below. Task-bound Registry views retain their Task claim; Session-bound views retain the Session. Thread Tasks require and retain their live local parent Process Task and enforce attaching-thread affinity. Process Tasks support coordinated calls from application threads.

Attachment advances the incarnation and restart count, resets claim-local publication issuance, and records native process/thread IDs and timestamps. `next_publication_id()` persists each checked sequence through FixedState. Native locks determine liveness; stale PID/attached bits are diagnostic. Last-owner destruction attempts fixed-state detachment before releasing its native lease; if that update fails, the next attachment repairs it. Replacing a Process Task also detaches thread claims tagged with its ending incarnation, through a compact ReleaseClaim command whose size is independent of the number of children. Neither operation appends records or grows the file, including at `LogFull`.

This native increment accepts two-plane storage with strict selection, spin waiting, recovery from log, and all-destination acceptance. Known policies whose behavior is not implemented return UnsupportedPolicy before creation; malformed values return InvalidConfiguration. TOML factories remain declarations until step 2.10. Queue endpoints, publication delivery/status, and scheduling remain later work. The Task-only ReleaseClaim executor rejects delivery state until its receipt/Source cleanup is implemented.

The [Session tests](../../test/test_runtime_session.cpp) use independent exec workers for attachment, ownership, termination/replacement, competing creation, and full-log cleanup of 24 child Tasks. The [metadata codec tests](../../test/test_session_metadata.cpp) check independent byte fixtures and malformed bodies; journal tests interrupt every cleanup/attachment redo word and repeat recovery. These are process-termination guarantees on the qualified local filesystem, under the existing power-loss and fork/exec boundaries.

## Step 2.6 Task registration and retained revisions

`Registry::register_task(TaskDescriptor)` now registers logical Process and Thread Tasks through a live actor-bound view. Session-bound views remain read-only. Shared descriptor validation checks UTF-8 names/documentation and parent shape; runtime validation checks the acting native/persisted claim and resolves a Thread's parent to a registered Process Task. The parent may be offline, and registration does not attach the target or populate its native diagnostics.

Exact re-registration compares name, kind, parent, and documentation and returns the existing ID before capacity, counter, or append checks. It preserves creator/time/revision and all bytes, including at LogFull. Differing descriptors under an existing name return Conflict. New registrations select unused IDs and empty allocation slots independently, append one immutable definition and manifest, install one index entry, and advance the Registry revision once. Older records and attribution remain retained across later registrations and actor restart. Task descriptors have no implicit update API; historical revisions here refer to retained Registry states and immutable definitions, not documentation edits through re-registration.

Complete definition/manifest/redo/padding budgeting precedes stable claim-file creation, which precedes journal preparation. This makes committed registrations attachable after a writer dies immediately after commit. An unaccepted attempt may leave a [provisional claim file](../glossary.md#ownership-and-recovery); later registration reuses its unpublished ID and stable inode without unlinking or replacing the file. Recovery settles pending work before lookup or retry, and exact registration can resolve an uncertain result without duplicating a committed identity. The [native contract](../native-storage.md#public-task-registration) defines these boundaries.

The [public registration tests](../../test/test_task_registration.cpp) explain normal registration/attachment, idempotency/conflicts, parent rules, Thread actors, capacities, full-log and counter exhaustion, sparse ID/slot geometry, immutable history, and coordination failures. The [independent-process tests](../../test/test_task_registration_recovery.cpp) use real registrars and a [worker](../../test/native_registration_worker.cpp) to demonstrate discovery, concurrent allocation, claim-file preparation, process termination at journal boundaries, repeated interrupted recovery, and I/O outcomes. Existing thread-lifetime examples now use public registration. Field/schema registration is the next step, 2.7; TOML factories remain step 2.10.

## Registry implementation boundaries

The [domain and policy contracts](milestone-1.md), [configuration/lifecycle contract](../configuration.md), [public API](../api.md), [mapped representation](../mapped-format.md), and [concurrency/recovery protocol](../concurrency.md) govern every step. The initial implementation uses the reviewed blocking mutation gate, per-Task lifetime claims, and one authoritative journal commit point.

Session bootstrap must create and attribute its initial creator Task before general registration calls can act on that session. Programmatic and file-based creation must reserve enough Task capacity for that creator and preserve the same validation and metadata rules.

Registration persists immutable definitions and their revisions in the arena, with current indexes and live state in the reserved prefix. Runtime additions use remaining capacity. New schema versions and topology revisions retain the definitions needed for historical inspection. Registry strings and Field lists consume arena bytes independently of the reserved application string pool.

Activation must satisfy the recorded resource contract. Step 2.9 reserves and initializes a queue's ring allocation when two-plane storage requires it, while message sending, receiving, ring overwrite/recovery, and Queue Set scheduling behavior remain work for milestones 3 through 6. The TOML loader must reject storage/policy combinations that the current runtime does not support; recognizing a wire code is not implementation support.

Step 2.11's native tests extend the milestone 1 logical model with real processes, files, mappings, claims, and interrupted writes. Native support is advertised only for profiles that pass their qualification checks. Power-loss durability and the later eventfd/segmented-storage implementations retain their separate scope.

[Project roadmap](../../ROADMAP.md) · [Milestone 1](milestone-1.md)
