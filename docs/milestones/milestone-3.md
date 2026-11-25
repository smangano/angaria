# Milestone 3 General messages and basic inspection

Milestone 2 reached `main` through [PR #23](https://github.com/smangano/angaria/pull/23), release `0.2.14`. Milestone 3 builds general messages from its persistent Registry and immutable schema versions. Staging branch `milestone-3` starts at that merged commit; each focused implementation step uses `milestone-3-<s>` and a PR targeting the staging branch. Integration release **`0.3.7`** brings the completed milestone into `main` through a separate PR.

The first step uses framework version **`0.3.1`**. Build numbering restarts within each milestone under the [versioning policy](../../ROADMAP.md#versioning). Each step updates the [programmer primer](../primer.md), runnable examples, contracts, and appropriate tests.

## Review steps

| Step | Branch | Scope | Completion criterion |
| --- | --- | --- | --- |
| 3.1 | `milestone-3-1` | Compile exact registered schemas into owning, bounded binary scalar codecs. | Independent golden bytes verify packed little-endian encoding, signed/unsigned bounds, IEEE floating-point bits, ordered/repeated Fields, empty schemas, errors, and retained exact versions. The ninth primer program encodes and decodes quotes. |
| 3.2 | `milestone-3-2` | Describe basic logical Fields: booleans, bit fields, enumerations, and fixed ASCII data. | Review their metadata, TOML representation, bounds, and required compatibility/feature rules before extending persisted definitions. Preserve existing base codes and earlier schema meaning; identify the missing signed 8-bit base. |
| 3.3 | `milestone-3-3` | Implement the reviewed metadata/configuration/profile extensions, encode/decode basic logical Fields and int8, and provide convenient bounded message construction. | Exact types and representation constraints are enforced; invalid values preserve caller-visible state. Generic inspection can distinguish the logical representations using registered metadata. Prior scalar definitions and format-2.0 Sessions retain their meaning. |
| 3.4 | `milestone-3-4` | Implement canonical generic message headers and record codecs. | Checked bytes carry exact schema identity, payload format/size, producer/publication identity and sequence, initial publication time, and user flags. Destination-specific acceptance/enqueue, dequeue, and acknowledgment metadata follows the existing receipt contract and is never confused with immutable payload data. |
| 3.5 | `milestone-3-5` | Capture and inspect several application-defined message types through retained Registry definitions. | An inspector decodes canonical message bytes without application-specific C++ types. It selects exact schema versions and rejects malformed/truncated/unknown records with precise diagnostics. |
| 3.6 | `milestone-3-6` | Qualify general-message exchange and retained-version inspection across processes. | Producer/reader examples exchange canonical message records through ordinary process/file I/O, survive schema-version additions, and remain inspectable after applications exit. Complete native-profile CI, compatibility checks, and the primer milestone review. |

Later step boundaries can be split further if a representation or format change needs its own review. Keep each PR concrete and reviewable; do not introduce working-looking substitutes for unfinished APIs.

## Step 3.1 Binary scalar payloads

`BinaryPayloadCodec` resolves one exact schema from a consistent Registry snapshot and owns its ordered Field definitions. Applications supply a payload byte cap at construction. No Session, Task, claim, or Registry handle is retained. A new schema version leaves an existing codec's meaning unchanged.

The initial codec covers the already registered native scalar bases: char, byte, signed/unsigned 16-/32-/64-bit integers, and 32-/64-bit IEEE floating point. It writes explicit little-endian bytes in schema order, including repeated Fields, without C++ struct padding or embedded type tags. Empty schemas produce empty payloads. Value counts, exact C++ alternatives, declared scalar widths, and encoded lengths are checked. Unsupported representations return errors until implemented in their assigned steps.

The [payload format](../message-payload.md) specifies the bytes and ownership/error rules. The [ninth primer example](../../examples/primer/09_payload.cpp) demonstrates v1/v2 quotes and independent codec lifetime; tests use hand-authored byte fixtures in addition to round trips.

## Step 3.2 Logical Field contract

Step 3.1 is merged into `milestone-3` through [PR #24](https://github.com/smangano/angaria/pull/24). Step 3.2 uses framework version **`0.3.2`** and is the design review before implementation.

The [logical Field contract](../logical-fields.md) defines one-byte booleans, 8/16/32/64-bit Fields, closed user-defined enumerations, and exact fixed ASCII sequences. It specifies owning immutable metadata over existing storage bases, canonical enum mappings, complete numeric ranges in TOML, byte/value bounds, and allocation/error rules. New code 26 will add the Wiki's missing signed 8-bit scalar while preserving codes `0..25`.

Step 3.2 changed no runtime behavior. Step 3.3 implements the specified format-2.1/required-feature boundary and configuration version 2 with explicit support for existing 2.0 Sessions and version-1 scalar TOML.

## Step 3.3 Logical Field implementation

Step 3.2 merged through [PR #25](https://github.com/smangano/angaria/pull/25). Step 3.3 uses framework version **`0.3.3`**. Owning logical metadata now survives registration, canonical retries, snapshots, handle closure, and native crash recovery. The codec enforces exact scalar/logical alternatives and supports initializer-list payload construction plus transactional `encode_into` for caller-owned buffers.

The [tenth primer lesson](../primer.md#10-encode-logical-fields-and-preserve-an-output-buffer) demonstrates TOML version 2, closed enum labels, exact ASCII padding, independent codec lifetime, golden bytes, and unchanged caller output after invalid input. Tests preserve the earlier scalar bytes and 2.0 Session behavior while qualifying required feature negotiation and immutable metadata bounds.

## Step 3.4 Canonical message records

Step 3.3 merged through [PR #26](https://github.com/smangano/angaria/pull/26). Step 3.4 uses framework version **`0.3.4`** and implements [portable message headers and record codecs](../message-record.md). Each record carries full publication identity, exact schema, format/size, initial publication time, and user flags. Bounded header/whole-record decoding distinguishes owning and borrowed results, rejects malformed framing, and preserves caller output on failure.

Typed binary decoding also checks independently supplied source Session context and exact schema version, including same-sized version and cross-Session mismatches. Destination acceptance/enqueue, dequeue, and acknowledgment remain receipt metadata. The [eleventh primer lesson](../primer.md#11-wrap-a-payload-in-a-message-record) demonstrates real ID issuance and Registry definitions without implying publication acceptance. Native mapped formats, feature masks, and Publication layout remain unchanged.

## Step 3.5 Capture and generic inspection

Step 3.4 merged through [PR #27](https://github.com/smangano/angaria/pull/27). Step 3.5 uses framework version **`0.3.5`** and adds [bounded capture streams and generic inspection](../capture-inspection.md). Captures concatenate the existing canonical records. The inspector owns one source Session's Field/schema catalog and renders exact binary scalar/logical versions without application-specific C++ types. It remains usable after originating handles close; later schema additions require a new catalog.

Separate record, aggregate capture, retained/expanded catalog, per-record text, and CLI aggregate-output limits bound work. Tests cover malformed/truncated/unknown records, source Session mismatches, stream errors and completed-record counters, escaping, scalar/logical values, repeated definitions, and owning catalog lifetime. The [twelfth primer lesson](../primer.md#12-capture-and-inspect-multiple-message-types) records multiple types and same-sized Quote versions, then runs the generic CLI after the application exits. Native catalog attachment retains ordinary recovery/profile requirements; native formats and record encoding are unchanged. Step 3.6 extends process/version/restart qualification.

## Step 3.6 Process exchange and milestone qualification

Step 3.5 merged through [PR #28](https://github.com/smangano/angaria/pull/28). Step 3.6 uses framework version **`0.3.6`**. The [thirteenth primer program](../../examples/primer/13_process_exchange.cpp) and [orchestrated walkthrough](../../test/process_exchange.py) exchange completed canonical capture files between independent applications. An owning inspector captured before Quote v2 keeps interpreting v1 after the same-sized, reordered v2 is registered. It rejects v2 as absent rather than substituting a newer layout. Cold generic inspectors decode both versions after all applications exit.

The walkthrough terminates a producer only after it has closed a complete capture, then verifies that replacement producers use new incarnations with their publication sequences reset. Earlier bytes and identities remain intact. Readiness handshakes and bounded waits coordinate processes; tests do not rely on sleeps. Exclusive workspace/phase directories protect earlier artifacts from accidental overwrite.

The [compatibility matrix](../process-exchange.md#compatibility-and-qualification) covers native 2.0 scalar inspection, 2.1 logical definitions, required-feature/native-profile rejection, and canonical record encoding. All thirteen primer programs compile with tests disabled. This completes the milestone's implementation steps. Native formats, required features, TOML versions, and message-record encoding are unchanged by this step.

## Completion and integration

Step 3.6 merged through [PR #29](https://github.com/smangano/angaria/pull/29). Integration release **`0.3.7`** combines the six reviewed steps without changing their runtime behavior or introducing another implementation step. The completed scope includes scalar/logical payloads, exact immutable schemas, canonical headers/records, bounded capture streams, generic inspection, and independent-process qualification.

The completion review covers all thirteen [primer lessons](../primer.md), their tests-disabled build, the [native compatibility matrix](../process-exchange.md#compatibility-and-qualification), and the full 498-test suite on Linux and macOS for x86_64 and ARM64. The integration PR records validation for its exact commit. Framework integration versioning does not change native formats 2.0/2.1, TOML versions 1/2, record encoding version 1, or synchronization protocol.

Native Queue payload operations begin in milestone 4. Source/Publishing Queue Set delivery behavior, advanced Fields, additional serializers, replay, catalog export, and foreign-profile/physically-read-only native tooling remain assigned to their later roadmap milestones.

## Implementation boundaries

The [public API](../api.md), [mapped-format contract](../mapped-format.md), and [concurrency/receipt contract](../concurrency.md) remain authoritative. Payload encoding does not itself create a publication, change Queue progress, or record a delivery. Native send/receive and Queue Set behavior remain with milestones 4–6. Ordinary process/file I/O in the message examples exercises canonical record bytes without adding another Angaria transport or reviving the legacy v0 queue design.

Steps 3.1–3.2 retained mapped format 2.0 and configuration version 1. Step 3.3 implements the reviewed 2.1/required-feature boundary and TOML version 2 while preserving existing base codes and earlier schema meaning. Canonical message capture inspection belongs to steps 3.5–3.6. General tooling, physically read-only or foreign-profile native Session-file readers, replay, converters, and advanced pooled/compound Fields retain their milestone 9–10 scope.

[Project roadmap](../../ROADMAP.md) · [Completed milestone 2](milestone-2.md)
