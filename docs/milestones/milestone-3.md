# Milestone 3 General messages and basic inspection

Milestone 2 reached `main` through [PR #23](https://github.com/smangano/angaria/pull/23), release `0.2.14`. Milestone 3 builds general messages from its persistent Registry and immutable schema versions. Staging branch `milestone-3` starts at that merged commit; each focused implementation step uses `milestone-3-<s>` and a PR targeting the staging branch. The completed milestone reaches `main` through a separate integration PR.

The first step uses framework version **`0.3.1`**. Build numbering restarts within each milestone under the [versioning policy](../../ROADMAP.md#versioning). Each step updates the [programmer primer](../primer.md), runnable examples, contracts, and appropriate tests.

## Review steps

| Step | Branch | Scope | Completion criterion |
| --- | --- | --- | --- |
| 3.1 | `milestone-3-1` | Compile exact registered schemas into owning, bounded binary scalar codecs. | Independent golden bytes verify packed little-endian encoding, signed/unsigned bounds, IEEE floating-point bits, ordered/repeated Fields, empty schemas, errors, and retained exact versions. The ninth primer program encodes and decodes quotes. |
| 3.2 | `milestone-3-2` | Describe basic logical Fields: booleans, bit fields, enumerations, and fixed character data. | Review their metadata, TOML representation, bounds, and required compatibility/feature rules before extending persisted definitions. Preserve existing base codes and earlier schema meaning. |
| 3.3 | `milestone-3-3` | Encode/decode the new basic Field representations and provide convenient bounded message construction. | Exact types and representation constraints are enforced; invalid values preserve caller-visible state. Generic inspection can distinguish the logical representations using registered metadata. |
| 3.4 | `milestone-3-4` | Implement canonical generic message headers and record codecs. | Checked bytes carry exact schema identity, payload format/size, producer/publication identity and sequence, enqueue time, and user flags. Destination-specific dequeue/acknowledgment metadata follows the existing receipt contract and is never confused with immutable payload data. |
| 3.5 | `milestone-3-5` | Capture and inspect several application-defined message types through retained Registry definitions. | An inspector decodes canonical message bytes without application-specific C++ types. It selects exact schema versions and rejects malformed/truncated/unknown records with precise diagnostics. |
| 3.6 | `milestone-3-6` | Qualify general-message exchange and retained-version inspection across processes. | Producer/reader examples exchange canonical message records through ordinary process/file I/O, survive schema-version additions, and remain inspectable after applications exit. Complete native-profile CI, compatibility checks, and the primer milestone review. |

Later step boundaries can be split further if a representation or format change needs its own review. Keep each PR concrete and reviewable; do not introduce working-looking substitutes for unfinished APIs.

## Step 3.1 Binary scalar payloads

`BinaryPayloadCodec` resolves one exact schema from a consistent Registry snapshot and owns its ordered Field definitions. Applications supply a payload byte cap at construction. No Session, Task, claim, or Registry handle is retained. A new schema version leaves an existing codec's meaning unchanged.

The initial codec covers the already registered native scalar bases: char, byte, signed/unsigned 16-/32-/64-bit integers, and 32-/64-bit IEEE floating point. It writes explicit little-endian bytes in schema order, including repeated Fields, without C++ struct padding or embedded type tags. Empty schemas produce empty payloads. Value counts, exact C++ alternatives, declared scalar widths, and encoded lengths are checked. Unsupported representations return errors until implemented in their assigned steps.

The [payload format](../message-payload.md) specifies the bytes and ownership/error rules. The [ninth primer example](../../examples/primer/09_payload.cpp) demonstrates v1/v2 quotes and independent codec lifetime; tests use hand-authored byte fixtures in addition to round trips.

## Implementation boundaries

The [public API](../api.md), [mapped-format contract](../mapped-format.md), and [concurrency/receipt contract](../concurrency.md) remain authoritative. Payload encoding does not itself create a publication, change Queue progress, or record a delivery. Native send/receive and Queue Set behavior remain with milestones 4–6. Ordinary process/file I/O in the message examples exercises canonical record bytes without adding another Angaria transport or reviving the legacy v0 queue design.

Step 3.1 leaves mapped format 2.0, configuration version 1, existing Field base codes, and the native policy-support boundary unchanged. New basic representations must explicitly review their metadata and compatibility requirements in step 3.2. Canonical message capture inspection belongs to steps 3.5–3.6. General tooling, physically read-only or foreign-profile native Session-file readers, replay, converters, and advanced pooled/compound Fields retain their milestone 9–10 scope.

[Project roadmap](../../ROADMAP.md) · [Completed milestone 2](milestone-2.md)
