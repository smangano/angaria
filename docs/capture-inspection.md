# Capture and generic message inspection

Milestone 3, step 3.5, framework version `0.3.5`, adds [capture stream helpers](../include/angaria/capture.h), an owning [message inspector](../include/angaria/inspection.h), and the `angaria_inspect` command. The [twelfth primer program](../examples/primer/12_capture.cpp) records Quote v1, Quote v2, Order, and Heartbeat. The generic inspector decodes them from retained Registry definitions, without including that application's message types.

A **capture** is an ordinary binary stream containing consecutive [canonical message records](message-record.md). There is no additional file header, index, or schema serialization format: each entry is the existing 80-byte header followed by its declared payload. An empty stream is a valid empty capture. Native Session formats 2.0/2.1, record encoding version 1, and required features are unchanged.

Capture order and duplicate publication IDs are preserved. They do not establish queue order, admission, delivery, or acknowledgment. Capture writes use ordinary streams; they are separate from Angaria's native journal and [direct Queue publication](direct-publication.md).

## Retain the source catalog

The **inspection catalog** is an owning copy of the source Session's Field and message-schema definitions. Message type/version pairs remain Session-scoped, and schema versions and Field meanings remain immutable. A capture identifies that context but does not embed its catalog. Keep the native Session directory with the capture so its retained definitions remain available.

`MessageInspector::from_session(session, limits)` captures the Session identity and one consistent Registry snapshot. It retains only Field/schema definitions and revision values, with no Session, Task claim, Registry handle, or live topology. Inspection remains usable after every originating handle closes. Definitions added afterward require a new inspector; it never silently refreshes or substitutes the latest schema version.

`from_snapshot(schemaSession, snapshot, limits)` accepts independently retained Session context and owning definitions supplied by the caller. It validates Field/schema identities, names, logical metadata, exact schema uniqueness, type/name families, and Field references. Unrelated Task/topology data is not retained. This validates catalog structure, not its claimed origin; caller-built snapshots are not authenticated.

Opening a native catalog through `Session::attach` can validate/recover and write its Session file, and requires a supported native ABI/profile. The capture itself is opened read-only by the CLI. This is not a physically read-only or foreign-profile native Session-file reader; those tools retain their later milestone scope. Once the inspector owns its catalog, reading/rendering captures requires no Session access.

## Capture streams and ownership

`MessageCaptureWriter::create(output, maxRecordBytes, maxCaptureBytes)` and `MessageCaptureReader::create(input, maxRecordBytes, maxCaptureBytes)` borrow ordinary C++ streams. Keep the stream alive and exclusively controlled for the wrapper's lifetime. These move-only wrappers do not open, truncate, seek, flush, or close files, change exception masks, coordinate writers, follow a live writer, or tail incomplete files.

`maxRecordBytes` must permit at least the 80-byte header. It bounds each complete record, including header and payload. `maxCaptureBytes` bounds all complete records starting at the borrowed stream's current position; zero permits an empty capture. Neither limit reserves its maximum amount of memory. Native size/arithmetic limits are checked separately.

The writer accepts either encoded record bytes or a header plus payload. It validates framing and remaining capture space before output, so invalid input or insufficient capacity leaves the stream and counters unchanged and permits a corrected retry. It recognizes all five stable payload formats structurally; it does not validate their application contents.

A stream failure can leave a partial output prefix. It returns `IoError` and makes the writer terminal; later appends return `InvalidState`. Capture appends are not journaled transactions, and the wrapper does not roll back ordinary I/O. The caller owns final flushing/closing and any failure reported then. Counters track complete records accepted by the stream, not durable storage.

The reader's `read_next()` returns an owning `MessageRecord`, clean end-of-stream as an empty optional, or an error. It reads one fixed header, checks its announced total against record/native/remaining-capture bounds, then allocates and reads exactly that payload. Payload contents remain opaque until inspection.

| Input condition | Result |
| --- | --- |
| No bytes at the next record boundary | Clean EOF; repeated calls remain EOF. |
| Only part of the next 80-byte header | `InvalidArgument`, truncated header. |
| Complete header but short payload | `InvalidArgument`, truncated payload. |
| Invalid header, unknown format, or unsupported encoding | The corresponding record-codec diagnostic. |
| Record or remaining capture budget exceeded | `CapacityExceeded`, before payload allocation/read. |
| Actual stream failure other than ordinary EOF | `IoError`. |

Any read error makes the reader terminal; subsequent reads return `InvalidState`. There is no resynchronization in the middle of damaged input. A completed valid prefix may already have been returned before a later error.

Reader/writer counters count only successfully completed records and their bytes. On a partial I/O error the underlying stream may have advanced, but the counters remain at that record's start. Offsets are capture-relative from the position at wrapper creation, without requiring a seekable stream. Diagnostics identify the zero-based record index and starting byte offset, such as `capture.records[2]@192`.

## Inspect exact definitions

`MessageInspector::inspect(recordBytes)` validates one complete record under its byte limit. The overload accepting `MessageRecordView` revalidates semantic header/size information. Both require the catalog's independently retained Session identity and the exact message type/version. Missing schemas return `NotFound`, another Session returns `SchemaConflict`, and a recognized nonbinary payload returns `UnsupportedPolicy` rather than pretending to decode its values.

The inspector resolves ordered/repeated Fields and compiles the selected `BinaryPayloadCodec` for each call. It decodes the existing scalar/logical representations, including signed/unsigned widths, IEEE floats, booleans, bit fields, closed enums, and fixed ASCII. It needs no application-specific C++ structures or enum declarations. A same-sized v2 with different Field order never changes v1 interpretation.

Inspection returns one owning ASCII text string, ending in a newline. The header line includes the message name, full Session identity, exact schema, producer Task/incarnation/sequence, initial publication UTC nanoseconds, and flags. Field lines preserve order and identify names, representations, and values. For example:

```text
message "Quote" session=00000000000000010000000000000002 type=1 version=1 producer=1 incarnation=1 sequence=1 time_ns=101 flags=0x80000001
  [0] "PriceTicks" int64 = -2
  [1] "Quantity" uint64 = 7
```

All metadata names, enum labels, and character data are quoted/escaped. Printable ASCII is preserved; quotes and backslashes are escaped, and other bytes appear as `\xhh`, including UTF-8 bytes and embedded ASCII controls. This keeps names and values from injecting terminal control sequences or extra lines. Fixed ASCII padding remains visible inside quotes. Bytes/int8 print numerically rather than as characters; bit fields include their width, and enums include the registered label plus underlying number. Float formatting uses the classic locale and sufficient precision; negative zero, infinities, and NaN bits remain distinguishable.

## Inspection limits

Use named members to override the defaults in `InspectionLimits`:

| Member | Default | Bound |
| --- | ---: | --- |
| `_maxRecordBytes` | 1,048,576 | One complete header plus payload. Must be at least 80. |
| `_maxCatalogBytes` | 16,777,216 | Encoded Field/schema definition-byte estimate, and separately the expanded selected Field layout before codec copies. Zero permits an empty catalog. |
| `_maxTextBytes` | 4,194,304 | One rendered record, including escaping, metadata names, and final newline. Must be positive. |

The catalog estimate includes Field fixed bodies, logical attributes, labels, names/documentation, schema fixed bodies, and ordered Field IDs. Borrowed size preflight precedes additional catalog copies. Repeated large enum definitions are charged per Field occurrence before compiling the selected layout. Text bounds are checked incrementally before appending or escaping, since a tiny enum payload can name a large label. Bounds describe encoded/catalog/text sizes, not exact process RSS; memory allocation can still return `CapacityExceeded`. `from_session` first acquires the full snapshot through the ordinary native Session API; the inspector catalog limit does not bound that earlier allocation.

An inspector is immutable during inspection and retains its original catalog. Copies own independent definitions; failed copy assignment preserves the prior inspector, and moved-from inspectors return `InvalidState`.

## Command-line inspection

```sh
./build/angaria_inspect <session-directory> <capture-file>
```

Optional named limits are `--max-record-bytes`, `--max-capture-bytes`, `--max-catalog-bytes`, `--max-text-bytes`, and `--max-output-bytes`. Defaults are the inspector limits above, 64 MiB total capture bytes, and 16 MiB aggregate rendered output. Values must be unsigned decimal integers. `--help` explains the native catalog access and bounds.

The CLI closes its native Session handle after obtaining the owning catalog and before reading the capture. It prints only rendered records to stdout, checks the aggregate output budget before each whole record, and returns nonzero with a contextual diagnostic on failure. A prior valid prefix may already be printed. Framing, inspection, or budget rejection prints none of the current record; an actual stdout write/flush failure can leave partial output. Unknown/truncated records are reported with their index and capture-relative byte offset rather than skipped.

Diagnostics on stderr escape control/non-ASCII bytes as `\xhh` and literal backslashes as `\\` in paths, messages, and delegated exception text. Each failure has one diagnostic line, so a malformed value paired with a control-bearing Field name cannot add terminal commands or forged error lines. The library's structured `Error` retains its original text; escaping is applied when the CLI displays it.

The [primer's capture lesson](primer.md#12-capture-and-inspect-multiple-message-types) shows creation and inspection as separate commands after the creator exits. Step 3.6's [process exchange lesson](process-exchange.md) qualifies retained catalogs across schema additions and producer replacement. Native Queue transport, generalized log replay, catalog export/converters, and foreign/physically-read-only native-file tools retain their roadmap scope.

[Message records](message-record.md) · [Payloads](message-payload.md) · [Milestone 3 plan](milestones/milestone-3.md)
