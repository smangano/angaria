# Sharing checked schema preparation with payload codecs

Milestone 4 step **4.21**, framework **0.4.23**, continues [definition reuse](performance-definition-reuse.md). Send and receive now share fresh schema/Field preparation with their payload codec rather than loading the same Fields and Tasks again. Public APIs, stored formats, native protocols, headers and view lifetimes are unchanged. Integration into `main` remains held for measured improvements and qualification.

## What changes

A **payload codec** interprets application bytes using an exact message schema and its Field definitions. Resolving that schema already loads and validates Tasks, Fields and message schemas. Previously codec construction then loaded Tasks and Fields again under the same unchanged operation guard.

The private [LoadedSchemaContext](../src/runtime_state.h) owns the selected `LoadedSchema` and complete checked Field catalog. `runtime_schema_context` loads Tasks once, Fields once and schemas using those checked definitions, then selects the requested schema. Generic `runtime_schema` delegates to this fresh loader and returns the owning selected schema, preserving its invalid-ID and missing-schema behavior.

The [typed codec helper](../src/payload_runtime.h) accepts that selected schema and its checked Fields. It performs no live reads and builds an owning `BinaryPayloadCodec`; no input span or Task authority is retained. Its generic RuntimeAccess overload remains available and freshly loads Fields for callers that supply no prepared catalog.

Fresh Binary send uses the context at the original message-validation point, after any Source reconciliation. Other send formats still resolve the schema without constructing a Binary codec. Receive loads the context at the original post-mapping point, checks the exact retained schema reference and then constructs the codec. An existing local pin repeat still preserves its original mapping and dependencies.

## Risk assessment

The [performance/correctness guidance](../AGENTS.md#performance-and-correctness) supports removing a repeated Field/Task load when the same complete definitions are already checked under an unchanged mutation guard. Each endpoint preparation creates a fresh context. No pre-issuance topology is reused after issuance, no cross-operation cache is introduced, and no native ownership or mutation authority is attached to the copied context.

Receive keeps its existing order: commit issuance, read Publication metadata, run `before_mapping`, map the payload, load fresh schema context, verify the exact schema reference, construct the codec, run `before_binding`, bind the message and transfer mapping ownership. All fallible preparation remains after issuance, and failures still perform exact release cleanup or preserve discoverable uncertain state. This change does not add a trusted metadata-mapping overload or move schema construction across those hooks.

Tasks, Fields and schemas retain their full policy, index completeness, uniqueness, creator attribution, registration revision and version-chain checks during context loading. Codec construction retains the existing snapshot-based dependency and payload checks. Generic loaders remain fresh for corruption qualification. Immutable owning definitions can support codec construction after their live Session closes; they do not establish permission to mutate that Session or infer current Task ownership.

There is no Session mutation between the fresh context load and typed codec construction in endpoint callers. Unsupported raw writes bypassing the gate and arbitrary memory corruption during that short interval remain outside the coordination protocol. Later operations revalidate storage. MessageHeader copying, payload mapping ownership, close/deadline fences, token identity and recovery guarantees retain their existing behavior.

## Qualification and measurements

Focused tests cover fresh context definition reads, generic/typed codec equivalence and owned metadata lifetime. Public receive read-observer qualification distinguishes complete-history validation from message preparation and checks that the second preparation Field load disappears. Existing Binary/non-Binary admission, schema/Field corruption, mapping/binding failure cleanup, pin repeat, owning copies, deadlines, close, allowed loss, recovery and exhaustion tests remain relevant. The primer still uses the same public API.

The unchanged diagnostic workload compares preserved 0.4.22 executables with normal and enabled 0.4.23 builds. Normal builds supply timing comparisons; enabled controls explain Task slots, allocations and native work. Fresh Sessions, history/capacity/payload/Field controls, serial/backlog/thread/process workloads, raw outcomes and source/executable hashes are retained. Adjacent alternating pairs complement sequential matrices. Plots show observed ranges, not confidence intervals or production-tail guarantees. CI has no timing thresholds.

The [result directory](../benchmarks/results/milestone-4-payload-preparation/README.md) records measurements and plots. Complete prefix/delivery validation, repeated selected-Publication metadata reads, remaining identity/successor searches, allocation/copy costs, file checks/growth and idle polling remain candidates. Full-history scanning and the Session-wide mutation gate still limit scaling.
