# Process exchange and retained schema qualification

Milestone 3, step 3.6, framework version `0.3.6`, qualifies the [canonical message record](message-record.md) and [capture/inspection APIs](capture-inspection.md) through independent processes. The [thirteenth primer program](../examples/primer/13_process_exchange.cpp) is an application example; its [Python walkthrough](../test/process_exchange.py) coordinates processes and checks their observable results. Neither needs private native storage APIs.

## Run the walkthrough

```sh
python3 test/process_exchange.py build/angaria_primer_13_process_exchange build/angaria_inspect
```

The default run owns a temporary workspace. To keep its Session and completed captures, use a new path whose parent already exists:

```sh
python3 test/process_exchange.py build/angaria_primer_13_process_exchange \
    build/angaria_inspect --workspace build/process-exchange
./build/angaria_inspect build/process-exchange/session build/process-exchange/new/capture.angcap
```

The compiled program is available when `BUILD_TESTING=OFF`; the walkthrough needs Python 3.9 or later. The normal CTest entry is `process_exchange` and runs on all four native CI profiles.

## Application commands and artifacts

| Command | Behavior |
| --- | --- |
| `init <new-workspace>` | Exclusively creates the workspace and its native `session` directory. Registers Setup, Feed, PriceTicks, Quantity, Quote v1, and empty Heartbeat. Reports the source Session identity independently of record bytes. |
| `retain <workspace>` | Owns the initial Field/schema catalog, closes native handles, reports readiness, and waits for a line on stdin. Then inspects the first/restart v1 captures and verifies that the new v2 capture returns `NotFound`. Start it before `upgrade`. |
| `produce <workspace> first [--hold]` | Writes v1 price `-2`, quantity `7`, then Heartbeat into `first/capture.angcap`. |
| `upgrade <workspace>` | Registers Quote v2 with Quantity before PriceTicks. Both Quote versions remain registered and occupy sixteen payload bytes. |
| `produce <workspace> new [--hold]` | Writes v2 quantity `8`, price `-3`, then Heartbeat into `new/capture.angcap`. |
| `produce <workspace> restart [--hold]` | Writes v1 price `-4`, quantity `9`, then Heartbeat into `restart/capture.angcap`. |

Prefix each command with `./build/angaria_primer_13_process_exchange`. `--hold` keeps the producer's Task claim alive after its capture has been flushed and closed; a line on stdin ends it normally. The walkthrough kills the first held producer at this completed-file boundary. It uses explicit readiness handshakes, bounded waits, and cleanup of its own children rather than timing sleeps.

Workspace and phase directories are created exclusively. Reusing them returns `Conflict` and preserves existing files. Each phase contains a 96-byte Quote record and an 80-byte Heartbeat record, for 176 bytes. These are ordinary captures, without an extra file header or embedded catalog. Keep their native Session directory to interpret them later.

## Catalogs, versions, and replacement identities

The retained reader constructs `MessageInspector::from_session` before another process registers v2. The inspector owns only definitions and source Session identity. It retains no native handle or Task claim. Its successful v1 reads and expected v2 `NotFound` establish that a newer same-sized layout neither rewrites old definitions nor refreshes an existing inspector implicitly.

Feed keeps Task ID 2. In a fresh walkthrough, its three independent attachments have incarnations 1, 2, and 3, and each issues publication sequences 1 and 2. Inspection preserves those original identities after claim replacement. Sequence reuse in a different incarnation does not identify the same publication. The script checks exact rendered headers and ordered values against the identity reported by initialization.

After the application and retained reader exit, separately launched `angaria_inspect` processes attach to the healthy native catalog, close their handles, and render all captures. Their fresh catalogs contain v1 and v2. Earlier capture bytes remain unchanged throughout version additions, replacement, and repeated inspection.

## Compatibility and qualification

This step changes framework version only. Native mapped formats 2.0/2.1, required features, TOML versions 1/2, synchronization protocol, and portable record encoding version 1 retain their existing contracts.

| Boundary | Qualification evidence |
| --- | --- |
| Retained 2.0 scalar definitions | `SchemaRegistryTest.LegacyScalarSessionStillAcceptsScalarsButRejectsNewRepresentations`; `InspectionRegistryTest.LegacyScalarCatalogInspectsCanonicalRecordsWithoutUpgradingItsProfile`. The latter attaches a clearly labeled private predecessor-profile fixture, uses real scalar records and replacement claims, and verifies inspection leaves the native profile at 2.0. |
| 2.1 logical metadata and required features | `MappedSuperblock.BasicFieldsProfileIsExplicitAndNegotiated`, `FormatAndBasicFieldFeatureCannotDisagree`; `SchemaRecoveryTest.LogicalMetadataSurvivesCrashCutsAndIndependentInspection`. |
| Unsupported minor/required feature before recovery | `SchemaRecoveryTest.UnsupportedProfileRejectsBeforeRecoveringAcceptedJournal` checks rejected attachment leaves the complete file unchanged before restoring a supported header and recovering. |
| Incompatible native ABI/protocol/features | `MappedSuperblock.LiveCompatibilityRequiresProfileProtocolAndRequiredFeatures`; `SessionControllerTest.IncompatibleProfilesAndRequiredFeaturesFailWithoutWrites`; `NativeAtomicTest.QualifiesOnlyTheActualNativeProfile`. |
| Portable record bytes and format dispatch | `MessageRecordTest.EncodesAndDecodesIndependentGoldenHeaderAndRecordBytes`, `EveryStablePayloadFormatPreservesOpaqueBytes`, and `RejectsMalformedMagicVersionsHeaderSizesFormatsAndReservedBytes`. Bytes are explicitly encoded and checked independently of C++ object layout. |
| Exact schema and independent Session context | `MessageInspection.ExactSchemaSelectionNeverFallsBackToNewestOrAnotherSession`; `MessageRecordTest.BinaryDecodingRequiresTheExactSchemaEvenWhenWidthsMatch` and `BinarySchemaContextIsAnIndependentSessionIdentity`; the new process walkthrough. |
| Process lifetime and teaching examples | CTest `process_exchange`, existing `capture_example` and `registry_walkthrough`, and the thirteen always-built primer programs. The [primer](primer.md) reviews the complete milestone-3 learning path. |

The [inspection tests](../test/test_inspection.cpp), [schema tests](../test/test_schema_registry.cpp), [recovery tests](../test/test_schema_recovery.cpp), [superblock tests](../test/test_mapped_superblock.cpp), [controller tests](../test/test_session_controller.cpp), [atomic tests](../test/test_native_atomic.cpp), and [record tests](../test/test_message_record.cpp) explain public scenarios separately from private compatibility/corruption fixtures. CI qualifies Linux and macOS on x86_64 and ARM64. These independent native runs do not establish cross-host live shared storage, foreign-profile native attachment, or migration support.

## Scope of the milestone

Applications exchange completed ordinary files after constructing records in memory. Captures preserve framing, values, exact schemas, and publication identities; they do not establish Queue admission, delivery, acknowledgment, log replay, or power-loss durability. Termination occurs only after a producer finishes its capture, so this scenario qualifies claim replacement rather than recovery of torn capture writes.

The native catalog still requires a qualified matching ABI and normal `Session::attach` recovery access. Physically read-only/foreign-profile native readers and catalog export remain later tooling. Native mapped Queue payload operations follow milestone 4; Source and Publishing Queue Set behavior follow milestones 5–6. Completion of step 3.6 is followed by a separate integration PR from `milestone-3` into `main`.

[Primer lesson 13](primer.md#13-exchange-captures-across-processes-and-schema-changes) · [Milestone 3 plan](milestones/milestone-3.md) · [Roadmap](../ROADMAP.md)
