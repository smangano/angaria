# Shared helpers and the consolidation audit

The original consolidation in framework **0.4.4** followed milestone 4.3. This map is updated for **0.4.39**, whose [public recorded runtime](public-runtime-admission.md) uses producer-owned logs and consumer-owned progress. The retired Session-gated message engine and its delivery/receipt helpers are no longer current implementation choices.

The audit covered tracked C++ sources and headers in `src`, `include`, `tools`, `examples`, and `test`. Function-body comparisons and searches for byte packing, zero checks, size arithmetic, result handling, identity keys, and process I/O were followed by review of their actual contracts. Untracked duplicate file copies are outside the build and this change.

## Production helper map

Use the related header below before introducing another local implementation. These are implementation details, not a new supported application API. Templates and `constexpr` functions are defined in headers; ordinary shared functions are explicitly `inline`. Inlining does not require a compiler-specific forced-inline attribute.

| Header | Related responsibilities |
| --- | --- |
| [byte_operations.h](../include/angaria/detail/byte_operations.h) | Typed and variable-width little-endian reads/writes, byte/word zero checks, signed interpretation of stored bits. Operates on bounded local copies; callers establish range/width preconditions. |
| [checked_arithmetic.h](../include/angaria/detail/checked_arithmetic.h) | Overflow-safe addition, multiplication, alignment, range containment, and host-size predicates. No native-format ceiling or error policy. |
| [error_support.h](../include/angaria/detail/error_support.h) | Result unwrapping with the caller's exception type, pure-codec failures/requirements, and ErrorCode names. |
| [wire_codes.h](../include/angaria/detail/wire_codes.h) | Explicit stored-code tables and optional encode/decode lookup for policies, payload formats, Fields, and reasons. C++ enum ordinals do not determine the wire representation. |
| [field_representation.h](../include/angaria/detail/field_representation.h) | Integer signedness/width and scalar byte widths. Logical validation and feature admission stay with their owners. |
| [identity_helpers.h](../include/angaria/detail/identity_helpers.h) | Schema keys and the application Field-ID prefix, independent of Registry existence. |
| [record_layout.h](../include/angaria/detail/record_layout.h) | Common native-record envelope/body/allocation geometry. Format constants remain in [mapped_format.h](../include/angaria/detail/mapped_format.h), which preserves existing arithmetic names through using declarations. |
| [coordination_descriptor.h](../src/coordination_descriptor.h), [native_claim.h](../src/native_claim.h) | Shared fork-aware process tokens and fork-registered coordination descriptors, EINTR-safe native locking, and exclusive lifetime ownership reused by Session administration, producer ownership and consumer progress. Claims use descriptors distinct from mapped data ownership. |
| [native_atomic.h](../src/native_atomic.h), [native_file.h](../src/native_file.h) | Qualified live atomic access, including load-only bindings for read-only mappings; native file access and setup-only physical reservation. Reservation is separate from ordinary growth and used before mappings are published. |
| [append_layout.h](../include/angaria/detail/append_layout.h) | Allocation-free administrative append geometry, including aligned bodies, manifest references, redo bytes and final log limits. Shared by journal execution and topology/configuration preflight. Producer records use their own format geometry. |
| [definition_metadata.h](../src/definition_metadata.h) | Shared Registry-definition registration checks, body bounds, and canonical name/doc slices. |
| [registry_access.h](../src/registry_access.h) | Registry/Task access ordering, required registration actors, and typed entity lookup mechanics. Diagnostics remain selected by the operation. |
| [message_retention.h](../src/message_retention.h) | Exact schema/Field decoding dependencies and private MessageView binding. It owns neither payload bytes nor delivery pins; copying retains decoding dependencies independently of runtime handles. |
| [payload_runtime.h](../src/payload_runtime.h) | Construct an exact Binary codec from validated schema/Field definitions or administrative access. It does not supply Task authority; admitted endpoints cache their checked codecs. |
| [public_runtime_storage.h](../src/public_runtime_storage.h) | Construct the authoritative catalog from validated definitions, check the operational profile, bootstrap fixed producer/connection/progress resources and commit the catalog seal under the administrative gate. It also rejects definition mutation after sealing. |
| [public_runtime.h](../src/public_runtime.h) | Bind and cache a Task's shared producer log and sealed resources, with selected Queue/Source preflight. Established messaging reuses those retained owners without opening the administrative gate. |
| [source_runtime_state.h](../src/source_runtime_state.h) | Source lifecycle reservations, retained Task ownership, acknowledged/unacknowledged consumer binding and Source-local serialization for receive/progress/close. |
| [journal_atomic.h](../src/journal_atomic.h) | Bounded copies of live administrative words through qualified native atomic access. Administrative journal validation and recovery do not read current producer messages or consumer receipts. |
| [mapped_read_window.h](../src/mapped_read_window.h) | Bounded operation-local mapping reuse for immutable copies and native atomic word loads, shared by journal scans and runtime metadata reads. Callers own gate/range/validation policy; no borrowed spans escape. |
| [journal_validation.h](../src/journal_validation.h) | Validate administrative record ranges and redo target permissions before any write; resolve exact record boundaries by binary search. Format 3 rejects the old Session Publication/receipt record kinds and cleanup compacts. |
| [producer_log_format.h](../src/producer_log_format.h) | Format-3 producer headers, cross-log references and Publication encode-into/borrowed-decode representations. Reuses byte/arithmetic/wire helpers; immutable codecs do not access live atomic words. |
| [producer_log.h](../src/producer_log.h) | Producer-owned ID issuance, accepted append/status, recovery and per-producer committed replay cuts. One retained owner is shared by a public Task's writers; immutable records remain acceptance authority. |
| [recorded_queue.h](../src/recorded_queue.h) | Fixed connection options, exact cached schema bindings, descriptor hints and logical transport credit. Provides both consumer modes over retained producer messages, independently of the Session administrative journal. |
| [recorded_progress.h](../src/recorded_progress.h) | Reserved consumer-owned operation images, receipt rows and selected-operation repair for optional processing acknowledgments. Credit return and the producer-visible committed acknowledgment lower bound remain distinct. |
| [text_escaping.h](../include/angaria/detail/text_escaping.h) | Printable ASCII/hex escaping, quote policy, and bounded diagnostic streaming. |
| [native_file_state.h](../src/native_file_state.h) | Native file state and conversion of `stat` device/inode identity. Descriptor ownership and coordination remain separate. |

The public detail directory is used where production code, tools, and examples need the same small header. Helpers tied to private runtime types remain in `src`. This prevents application examples from depending on the private Session implementation.

## Tests and examples

Test support remains separate from production encoding so expected bytes cannot silently inherit a production encoder bug.

| Header | Related responsibilities |
| --- | --- |
| [result_helpers.h](../test/result_helpers.h), [result_assertions.h](../test/result_assertions.h) | Successful Result extraction and common error assertions. Assertions are separate so independent exec workers do not acquire a GoogleTest dependency. Scenario-specific path checks stay local. |
| [wire_fixture.h](../test/wire_fixture.h) | Independent test integer/text/byte construction. Each suite still spells out its expected fields, codes, offsets, and values. |
| [journal_fixture.h](../test/journal_fixture.h) | Shared hand-authored journal drafts used by the parent qualification tests and their exec worker. These are deliberately incomplete domain fixtures, not Session initialization examples. |
| [native_log_budget.h](../test/native_log_budget.h) | Fill the remaining administrative arena with a valid retained Queue documentation revision for exact-budget tests. It does not falsify the configured ceiling or bound producer message retention. |
| [native_fixture.h](../test/native_fixture.h) | Private native observation, snapshots, claim-name capture, and deliberate atomic corruption setup. |
| [process_support.h](../test/process_support.h) | Test pipe reports, continuation, bounded reads/waits, SIGPIPE handling, and status/descriptor mechanics. Worker ownership stays in the fixture. |
| [native_process.h](../test/native_process.h) | RAII ownership of a separately executed qualification worker and its bidirectional pipes. Reuses bounded process-support reads/waits and SIGPIPE handling; scenario protocols remain in each test/worker. |
| [producer_log_fixture.h](../test/producer_log_fixture.h) | Prepared private producer storage paths, bounded reservations and opaque message requests shared by crash tests, their independent worker and the append-cost study. Not public Session setup. |
| [recorded_queue_fixture.h](../test/recorded_queue_fixture.h) | Frozen private point-to-point schema/topology context, prepared payloads and typed send-result checks shared by mapped delivery/process qualification and the isolated hot-path check. Public primer programs do not use it. |
| [recorded_acknowledged_fixture.h](../test/recorded_acknowledged_fixture.h), [recorded_queue_independence_fixture.h](../test/recorded_queue_independence_fixture.h) | Private consumer-progress fault setup and disjoint-connection process qualification. Native corruption/checkpoints remain separate from application configuration. |
| [public_runtime_fixture.h](../test/public_runtime_fixture.h) | Explicit public Session/Registry/schema/topology setup shared by admission and public performance tests. Messaging uses public Task/endpoints, without fabricated catalog references or private transport opening. |
| [allocation_capture.h](../test/allocation_capture.h), [performance_samples.h](../test/performance_samples.h) | Allocation replacement/capture for isolated performance executables, plus odd-sized sample medians and raw JSON sample arrays. Include allocation replacements in exactly one translation unit per executable. |

The runnable primer intentionally spells out public configuration and result checks in each lesson. Its former generic result/configuration wrappers and shared presentation header have been removed. Application-specific functions may propagate an `Error` through `Result<T>` or `OperationResult`; output-only formatting uses the standard library locally. This keeps each teaching program self-contained without depending on Angaria's private helpers.

[ipc_comparison_support.h](../benchmarks/ipc_comparison_support.h) groups argument parsing, bounded process coordination, matched workload data and result reporting for the current transport comparisons. These are research-tool helpers, separate from explicit primer examples and independent wire fixtures. [performance_diagnostics.h](../src/performance_diagnostics.h) groups the private, optional collector, phase/counter vocabulary and instrumentation macros; normal builds compile those observations away.

The former `transport_support.h`, delivery-history/word/access helpers, Publication storage fixtures and compact claim/loss helpers belonged to the retired Session-gated engine. Historical studies retain their pinned source revisions; those helpers are not dependencies to reintroduce into current messaging.

## Intentional local adapters

Some similarly named functions have different responsibilities. Their shared mechanics now live in the headers above; small adapters remain where they bind those mechanics to a contract:

- Format limits and diagnostics differ. A journal addition can enforce `max_log_bytes`, while inspection uses its own budgets. A `host_size` wrapper still selects the operation's error code/path. Catch blocks retain their different allocation, I/O, and unexpected-failure policies.
- Definition bodies permit eight-byte logical alignment, while Publications require 64-byte native record alignment. Session strings have a different fixed prefix and diagnostics. Sharing a range predicate must not erase those distinctions.
- Atomic adapters and replay reads validate different ownership, recovered ranges, and target permissions. Ordinary byte helpers never replace live atomic loads/stores or their fences.
- NativeFile and coordination descriptors differ in inode/mapping leases, fork registration and unlock/close behavior. A generic descriptor owner would obscure these differences.
- Quoted inspection text escapes quotes; diagnostic text leaves printable quotes intact. Shared escaping retains that policy choice, each caller's byte budget, and its line boundaries.
- Test process constructors and command lines describe different scenarios. Fixture methods remain thin adapters for their owned PID/descriptors and diagnostics. The controller's process-group/reaped state and the Session worker's per-poll timeout remain distinct from aggregate-deadline readers.
- Raw wire fixtures, Task/schema/topology profiles, and expected values stay explicit in their suites. Coincidentally equal fixture capacities can evolve independently; they are not production capacity defaults.
- Compile-only interface-review examples may use presentation shortcuts described in their own review contract. Runnable primer programs show public Result checks directly and do not share generic unwrappers with those proposals.

The shared-helper tests cover unaligned and variable-width bytes, signed boundaries, empty/nonzero ranges, arithmetic overflow, move-only Results, wire-code/Field semantics, and escaping across streaming boundaries. Existing format, ownership, recovery, and primer tests remain the behavioral baseline. All shared headers are also compiled independently to check their includes.

[diagnostic_totals.h](../benchmarks/diagnostic_totals.h) provides overflow-checked diagnostic Sample accumulation. [ipc_operation_diagnostics.h](../benchmarks/ipc_operation_diagnostics.h) supplies IPC-specific capture lifetimes and serialization; ordinary builds compile those captures away. The original stall and Session-gate studies are historical evidence; current public measurements must exercise admitted endpoints.
