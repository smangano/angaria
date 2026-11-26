# Shared helpers and the consolidation audit

Framework **0.4.4** is a focused cleanup after milestone 4.3. It consolidates repeated helper mechanics without changing the public API, stored layouts, wire codes, supported profiles, or roadmap step numbering. The next development step remains 4.4.

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
| [append_layout.h](../include/angaria/detail/append_layout.h) | Allocation-free complete append geometry, including separately aligned immutable/mutable bodies, manifest references, redo bytes, and final log limits. Added in step 4.4 and shared by journal execution and topology/configuration preflight. |
| [definition_metadata.h](../src/definition_metadata.h) | Shared Registry-definition registration checks, body bounds, and canonical name/doc slices. |
| [registry_access.h](../src/registry_access.h) | Registry/Task access ordering, required registration actors, and typed entity lookup mechanics. Diagnostics remain selected by the operation. |
| [delivery_words.h](../src/delivery_words.h), [delivery_access.h](../src/delivery_access.h) | Local atomic-word/receipt codec bridges and gate-bound atomic byte copies, shared by history validation, reserved progress, and claim cleanup. They do not replace the native atomic adapter with ordinary mapped-byte reads. |
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
| [publication_storage_fixture.h](../test/publication_storage_fixture.h) | Shared private committed Publication setup for payload mapping and rollback qualification. It isolates storage geometry from complete schema/topology/receipt semantics; it is not application setup. |
| [delivery_progress_fixture.h](../test/delivery_progress_fixture.h) | Full-domain private delivery setup, exact log exhaustion, and captured claim/replacement intents shared by progress, cleanup, and independent crash qualification. Public Registry bootstrap is followed by deliberately installed core records. |
| [native_fixture.h](../test/native_fixture.h) | Private native observation, snapshots, claim-name capture, and deliberate atomic corruption setup. |
| [process_support.h](../test/process_support.h) | Test pipe reports, continuation, bounded reads/waits, SIGPIPE handling, and status/descriptor mechanics. Worker ownership stays in the fixture. |
| [example_support.h](../examples/primer/example_support.h), [example_formatting.h](../examples/primer/example_formatting.h) | Primer failure/configuration conventions and related byte/Session-identity presentation. |

## Intentional local adapters

Some similarly named functions have different responsibilities. Their shared mechanics now live in the headers above; small adapters remain where they bind those mechanics to a contract:

- Format limits and diagnostics differ. A journal addition can enforce `max_log_bytes`, while inspection uses its own budgets. A `host_size` wrapper still selects the operation's error code/path. Catch blocks retain their different allocation, I/O, and unexpected-failure policies.
- Definition bodies permit eight-byte logical alignment, while Publications require 64-byte native record alignment. Session strings have a different fixed prefix and diagnostics. Sharing a range predicate must not erase those distinctions.
- Atomic adapters and replay reads validate different ownership, recovered ranges, and target permissions. Ordinary byte helpers never replace live atomic loads/stores or their fences.
- NativeFile, coordination descriptors, and prototype role locks differ in inode/mapping leases, fork registration, and unlock/close behavior. A generic descriptor owner would obscure these differences.
- Quoted inspection text escapes quotes; diagnostic text leaves printable quotes intact. Shared escaping retains that policy choice, each caller's byte budget, and its line boundaries.
- Test process constructors and command lines describe different scenarios. Fixture methods remain thin adapters for their owned PID/descriptors and diagnostics. The controller's process-group/reaped state and the Session worker's per-poll timeout remain distinct from aggregate-deadline readers.
- Raw wire fixtures, Task/schema/topology profiles, and expected values stay explicit in their suites. Coincidentally equal fixture capacities can evolve independently; they are not production capacity defaults.
- Primer and declaration-example Result wrappers format errors differently. They keep their respective presentation policies rather than changing example output during cleanup.

The shared-helper tests cover unaligned and variable-width bytes, signed boundaries, empty/nonzero ranges, arithmetic overflow, move-only Results, wire-code/Field semantics, and escaping across streaming boundaries. Existing format, ownership, recovery, and primer tests remain the behavioral baseline. All shared headers are also compiled independently to check their includes.
