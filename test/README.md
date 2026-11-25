# Reading the tests

For a guided introduction before reading test fixtures, use the [programmer primer](../docs/primer.md) and [runnable examples](../examples/primer/). CTest's `primer_examples` checks the first seven lessons, including process ownership and TOML. The eighth lesson runs separately as `registry_walkthrough`, covering concurrent registrars, historical snapshots, owner replacement, and offline Registry inspection.

The ninth lesson runs as `payload_example` and shows schema-driven binary encoding/decoding. Its [codec tests](test_payload.cpp) use independent golden bytes, integer/IEEE boundaries, exact schema versions, malformed inputs, payload bounds, and independent codec lifetime. These payload APIs use the current Registry definitions and remain separate from the legacy Field-template serialization tests.

Start with [test_runtime_session.cpp](test_runtime_session.cpp) for Session creation/lifetimes, [test_task_registration.cpp](test_task_registration.cpp) for logical Task registration, and [test_schema_registry.cpp](test_schema_registry.cpp) for Field and versioned schema definitions. Their test comments describe the application scenario, the relevant lifetime or failure rule, and any private setup used to reach that state. Test helpers at the beginning manage temporary files, independent processes, and deliberate corruption; they are test infrastructure rather than application initialization code.

A useful reading order is:

1. `CreationRecordsCreatorEffectiveOptionsAndFrameworkMetadata`: create a Session, use its returned creator Task, and inspect options and Registry metadata.
2. `RegistersOfflineParticipantsBeforeTheyAttach` and `ExactRegistrationIsAnUnchangedLookupIncludingTheCreator`: register logical Tasks through the creator's Registry, inspect attribution, and retry without creating another identity.
3. `ActorRegistryRetainsItsTaskWhileReadOnlyViewsDoNot`: see which views retain a Task claim and when another owner can attach.
4. `PublicationIdsPersistWithinAClaimAndResetOnlyWithANewIncarnation`: issue IDs and observe the identity boundary at Task replacement. This does not send a publication.
5. `ThreadTasksRequireAndRetainTheirParentAndRejectOtherThreads`: register children through the creator's public Registry, then understand parent retention and attaching-thread affinity.
6. `RegistersScalarFieldsWithFullWidthApplicationIdentities` and `RetainsExactVersionsOrderedRepetitionsAndEmptyControlSchemas`: register fixed-width Fields and exact schema versions without confusing IDs, versions, and index slots. `LargeFieldListsPreserveEveryFullWidthReference` demonstrates the count/identity widths independently.
7. [Topology activation](test_topology_activation.cpp) and [endpoint lifetimes](test_endpoint_runtime.cpp): complete drafts, activate Source/Queue/Publishing resources in order, and understand which handles retain ownership. These examples stop at identity handles, before payload delivery.
8. The concurrent registration, coordinated replacement/status, termination, full-log, and malformed-state scenarios: understand recovery and rejection guarantees after learning normal use.

Build instructions are in the [project README](../README.md). Run just the public Session and registration examples with:

```sh
ctest --test-dir build -R '^(RuntimeSessionTest|TaskRegistrationTest|SchemaRegistryTest|TopologyRegistryTest|TopologyActivationTest|EndpointRuntimeTest)\.' --output-on-failure
```

The rest of the suite explains the supporting contracts:

| Tests | What readers should learn |
| --- | --- |
| [Task registration](test_task_registration.cpp), [process/recovery scenarios](test_task_registration_recovery.cpp) | Logical registration versus native attachment, exact retry, attribution/history, concurrent registrars, stable claim resources, and interrupted outcomes. Fault hooks and sparse-record fixtures are test-only setup. |
| [Field/schema registration](test_schema_registry.cpp), [process/recovery scenarios](test_schema_recovery.cpp), [portable definitions](test_schema_metadata.cpp) | Full-width application IDs, fixed scalar sizes, immutable version families, ordered Field lists, exact byte budgets, and retained history. Format recognition does not implement payload encoding or unsupported Field representations. |
| [Draft topology](test_topology_registry.cpp), [process/recovery scenarios](test_topology_recovery.cpp), [portable definitions](test_topology_metadata.cpp) | Register and inspect Queues and both Queue Set kinds, resolve policies, preserve typed shared identities, and commit Source membership atomically. These fixtures focus on Draft metadata; the following suites cover activation and endpoints. |
| [Registry snapshots](test_registry_snapshot.cpp), [Task history validation](test_task_catalog_validation.cpp), [mixed-process qualification](test_registry_qualification.cpp), [walkthrough](registry_walkthrough.py) | Capture owning catalogs at one current or historical revision, preserve atomic relationships, distinguish current claim diagnostics, and combine all entity kinds with concurrency, restart, interrupted updates, and offline inspection. Raw corruption and journal callbacks appear only in qualification fixtures/workers. |
| [Activation and updates](test_topology_activation.cpp), [endpoint ownership](test_endpoint_runtime.cpp), [process/recovery scenarios](test_activation_recovery.cpp) | Complete draft topology, activate resources atomically, preserve history and active ownership, and retain Task claims through endpoint identity handles. These tests do not send or receive payloads. |
| [API values](test_api_contract.cpp), [identities](test_identities.cpp), [Fields](test_field.cpp) | Descriptor defaults and capacity setters, distinct IDs, result alternatives, and basic Field serialization. Value construction alone does not exercise delivery or endpoint operations. |
| [TOML example](test_configuration.cpp), [file configuration](test_file_configuration.cpp) | Parse documented syntax through pinned cpptoml, create the equivalent persistent Registry, reject typed/semantic/policy/capacity mistakes, and check compatible attachment without applying configuration updates or claiming Tasks. |
| [Layout](test_mapped_format.cpp), [superblock](test_mapped_superblock.cpp), [Session/Task bodies](test_session_metadata.cpp), [record lookup](test_runtime_access.cpp) | Capacity accounting, explicit portable encoding, and reuse of validated record boundaries within an operation. Hand-authored byte fixtures check format compatibility independently of production encoders. |
| [Files/mappings](test_native_file.cpp), [atomics](test_native_atomic.cpp), [controller](test_session_controller.cpp) | Private ownership, aliasing, synchronization, and native process boundaries beneath the Session API. |
| [Journal encoding](test_journal_format.cpp), [allowed targets](test_journal_validation.cpp), [transactions/recovery](test_mutation_journal.cpp), [bulk redo](test_journal_bulk_redo.cpp) | Acceptance, complete-stream validation, rollback, repeatable replay, and uncertain outcomes. Artificial byte changes and fault checkpoints are controlled test techniques. |
| [Prototype Session tests](test_session.cpp), [queue CLI check](queue_demo.cmake), [restart walkthrough](restart_demo.py) | The earlier v0 producer/consumer prototype. Its APIs and delivery behavior are separate from the format-2 implementation milestones. |
| [Protocol model](protocol_model.py) | Logical scenarios for the full planned protocol. Model success does not mean every modeled runtime feature is implemented or native behavior is qualified. |

The `native_*_worker.cpp` programs run operations in independent exec processes so tests cannot pass solely because handles share process-local state. The startup/shutdown programs cover resource owners outside ordinary test-body lifetimes. Native crash tests exercise process termination on a running local filesystem; the [native storage contract](../docs/native-storage.md) defines their scope, and the [glossary](../docs/glossary.md) defines the storage and recovery terms.
