# Angaria terminology

These definitions describe the terms used in Angaria's storage and recovery design. The [domain-object definitions](milestones/milestone-1.md#domain-objects) cover Registry, Task, Queue, both Queue Set kinds, Message, Publication, and Delivery. Introduce specialized terms with a definition or a link to their canonical definition.

## Session storage

| Term | Meaning in Angaria |
| --- | --- |
| **Superblock** | The fixed 4,096-byte session control area at byte offset zero of `session.ang`. It contains configuration and compatibility metadata, the section directory, mutable control fields, and the operation journal. The [superblock definition and layout](mapped-format.md#superblock) specify its contents. |
| **Fixed section** | A table or pool with capacity and location recorded when a session is created. Nonempty sections reserve storage after the superblock; zero-capacity sections allocate no bytes and have offset zero. Section contents can change even though their reserved sizes stay fixed. |
| **Section directory** | The thirteen descriptors inside the superblock that identify each fixed section's kind, entry stride, logical offset, entry count, and byte length. The descriptors locate the sections; they do not contain the section data. |
| **Canonical layout** | The section order, sizes, and offsets prescribed by the format version and recorded capacities. The decoder requires this layout, rather than accepting arbitrary nonoverlapping section arrangements. |
| **Stride** | The byte width of one entry in a section. A section's byte length is its entry count multiplied by its stride. |
| **Fixed reservation** | The superblock, all fixed sections, and their alignment padding. Its size depends on the configured capacities, and the arena begins after it. |
| **Superblock prefix** | The first 4,096 bytes of the session file. Step 2.1's encoder produces this prefix provisionally, before file allocation, bootstrap, or session publication. For the reserved region that includes the fixed sections too, use **fixed reservation**. |
| **Arena** | Storage for variable-sized allocations logically beyond the fixed reservation, including definitions, transaction manifests, publications, receipts, and ring storage. Records can include mutable fields; the arena is not wholly immutable. |
| **Logical offset** | An unsigned 64-bit byte address within the session's storage layout. Two-plane storage uses file offsets; segmented storage translates the address into its owning extent. Zero denotes a null reference. |
| **Root** | A top-level stored reference or control value identifying current state, such as the latest manifest reference, Registry revision, or allocation tail. A root is not necessarily a pointer or offset. |
| **Extent** | A contiguous file allocation representing one logical address interval in segmented storage. It has its own header and can occupy multiple allocation quanta. |
| **Segment allocation quantum** | The configured `segment_bytes` unit used to size new extents. It is an allocation increment, rather than a maximum message size. |

## Encoding and native resources

| Term | Meaning in Angaria |
| --- | --- |
| **Codec** | Code that encodes process-local values into specified bytes and decodes those bytes into values. The superblock codec also validates immutable metadata and section geometry. It does not serialize native C++ object layouts. |
| **Little-endian** | Integer encoding in which the least significant byte is stored first, at the lowest byte offset. |
| **ABI profile** | An Application Binary Interface profile identifying the native OS/CPU and shared-memory access requirements for live attachment. Portable metadata decoding can inspect a foreign profile; using it live requires a qualified compatible native adapter. |
| **RAII** | Resource Acquisition Is Initialization: C++ ownership in which an object's lifetime controls a resource, and its destructor releases that resource. File and mapping owners use this to manage native handles. |

`SuperblockBytes` represents the complete serialized 4,096-byte control area. `SuperblockConfiguration` contains only the process-local immutable configuration values returned by the metadata decoder. It excludes mutable roots and journal state. Successful metadata decoding does not establish a recovered, usable Session.

## Ownership and recovery

| Term | Meaning in Angaria |
| --- | --- |
| **Task claim** | An exclusive attachment to a logical Task, retained by its Task handle and dependent endpoints or actor-bound Registry views. OS ownership and a recorded incarnation distinguish a live owner from an earlier attachment. |
| **Bootstrap** | Initializing a new session's storage, creator Task, metadata, and starting state before making the session available for use. |
| **Incarnation** | The generation of a Task attachment. Reattachment advances it so identities and tokens from an older claim remain distinguishable. |
| **Mutation gate** | The combination of a process-local mutex and an interprocess file lock that serializes mutable session access in the initial protocol. |
| **Operation journal** | The reusable 2,048-byte area inside the superblock describing the current operation's intent, commit state, and inline recovery commands or append-manifest reference. |
| **Manifest** | An append-transaction record identifying the operation's new records and redo commands. It lives in the arena, separately from the reusable journal. |
| **Redo** | Reapplying a committed operation's intended changes so interrupted work completes. The commands are repeatable without duplicating logical effects such as delivery or sequence issuance. |
| **Commit point** | The journal's transition to Committed, which establishes the operation's logical acceptance. Recovery can finish its remaining effects afterward. This does not itself promise power-loss durability. |
| **Delivery pin** | The retention protection for a currently issued delivery and its borrowed payload. It lasts until acknowledgment, release, Source handle closure, or claim termination, and survives ring overwrite. |

The [concurrency and recovery contract](concurrency.md) specifies these protocols. The [mapped format](mapped-format.md) specifies their byte representations.
