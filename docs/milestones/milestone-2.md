# Milestone 2 Shared persistent Registry

Milestone 1's architecture and policy contracts reached `main` through [PR #7](https://github.com/smangano/angaria/pull/7). Milestone 2 implements the shared persistent Registry against those reviewed contracts. Work is under implementation, beginning with the binary foundation in step 2.1.

Completion requires independent processes to register and discover Tasks, Fields, immutable message schemas, Queues, Source Queue Sets, and Publishing Queue Sets; recover interrupted updates; and inspect the persisted Registry after application shutdown.

The staging branch `milestone-2` starts from the merged `main`. Each focused step uses `milestone-2-<s>` and a PR targeting `milestone-2`. After all steps are reviewed and merged, a milestone PR integrates `milestone-2` into `main`.

## Review steps

| Step | Branch | Scope | Completion criterion |
| --- | --- | --- | --- |
| 2.1 | `milestone-2-1` | Checked fixed-section planning, format-2 superblock encoding/decoding, and metadata compatibility checks. | Golden-byte and malformed-input tests validate the 4,096-byte prefix and section directory without native struct serialization. Decoding remains separate from live session validation. |
| 2.2 | `milestone-2-2` | Native file and mapping RAII, checked ranges, file-size validation, and controlled resize. | File/mapping lifetimes and failure paths work on supported hosts; truncated files and invalid mapping ranges fail before access. |
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

The internal [superblock codec](../../src/mapped_superblock.h) implements three operations:

- `encode_initial_superblock()` produces a provisional 4,096-byte prefix containing the immutable configuration and planned section directory. It does not allocate the remaining sections or create/publish a session file.
- `decode_superblock()` reads immutable configuration and validates the directory against its capacity-derived layout. It accepts exactly format 2.0 and rejects truncated, malformed, or inconsistent metadata before forming references into other storage.
- `check_live_compatibility()` compares decoded metadata with caller-supplied ABI/protocol, required-feature, storage-support, and optional capacity-ceiling capabilities. It does not qualify the caller's native adapter.

The codec writes explicit little-endian integers and returns process-local values. It preserves opaque ABI/profile and feature metadata for portable inspection, including foreign native profiles and unknown optional feature bits. Live compatibility is a separate check; decoding bytes is not permission to mutate them.

Mutable roots, live state, and the reusable operation journal are deliberately outside this metadata decoder's validation. Their bytes may reflect a pending transaction or a previously used Idle journal. Later attachment must acquire the proper gate, recover when required, and validate resulting state before exposing usable Session or Registry handles.

Step 2.1 tests use an independently authored byte fixture, round trips, and invalid cases for format markers, lengths, capacities, section geometry, overflow, reserved fields, and compatibility metadata. They also verify that mutable root/journal bytes do not turn this portable decoder into an incomplete recovery routine. Native file I/O, mappings, atomics, gate ownership, and journal repair follow in steps 2.2 through 2.5.

## Registry implementation boundaries

The [domain and policy contracts](milestone-1.md), [configuration/lifecycle contract](../configuration.md), [public API](../api.md), [mapped representation](../mapped-format.md), and [concurrency/recovery protocol](../concurrency.md) govern every step. The initial implementation uses the reviewed blocking mutation gate, per-Task lifetime claims, and one authoritative journal commit point.

Session bootstrap must create and attribute its initial creator Task before general registration calls can act on that session. Programmatic and file-based creation must reserve enough Task capacity for that creator and preserve the same validation and metadata rules.

Registration persists immutable definitions and their revisions in the arena, with current indexes and live state in the reserved prefix. Runtime additions use remaining capacity. New schema versions and topology revisions retain the definitions needed for historical inspection. Registry strings and Field lists consume arena bytes independently of the reserved application string pool.

Activation must satisfy the recorded resource contract. Step 2.9 reserves and initializes a queue's ring allocation when two-plane storage requires it, while message sending, receiving, ring overwrite/recovery, and Queue Set scheduling behavior remain work for milestones 3 through 6. The TOML loader must reject storage/policy combinations that the current runtime does not support; recognizing a wire code is not implementation support.

Step 2.11's native tests extend the milestone 1 logical model with real processes, files, mappings, claims, and interrupted writes. Native support is advertised only for profiles that pass their qualification checks. Power-loss durability and the later eventfd/segmented-storage implementations retain their separate scope.

[Project roadmap](../../ROADMAP.md) · [Milestone 1](milestone-1.md)
