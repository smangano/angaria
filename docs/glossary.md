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
| **Logical representation** | A Field's meaning and value constraints beyond its storage bytes: for example, a Boolean permits only zero and one. The [logical Field contract](logical-fields.md) describes its required metadata, implemented in step 3.3. |
| **Storage base** | The underlying integer or character representation and fixed byte width used by a Field. Logical metadata can constrain it without changing an existing base code's meaning. |
| **Closed enumeration** | A named mapping of distinct numeric values to distinct labels; numbers outside the mapping are invalid. Changing the mapping requires a new immutable Field identity. |
| **Fixed ASCII** | An exact number of bytes in `00..7f`, including any explicitly supplied control or padding bytes. It is distinct from variable-length UTF-8 text. |
| **ABI profile** | An Application Binary Interface profile identifying the native OS/CPU and shared-memory access requirements for live attachment. Portable metadata decoding can inspect a foreign profile; using it live requires a qualified compatible native adapter. |
| **RAII** | Resource Acquisition Is Initialization: C++ ownership in which an object's lifetime controls a resource, and its destructor releases that resource. File and mapping owners use this to manage native handles. |
| **File descriptor** | A process-local integer handle used by native calls to refer to an open file. A borrowed descriptor does not transfer ownership to the caller. |
| **Device/inode identity** | The native device and inode numbers identifying an underlying file. Different pathnames or hard links can identify the same file and must share its process-local mapping bookkeeping. |
| **Alias** | Another handle or pathname referring to the same underlying device/inode identity. Aliases share mapping bookkeeping, while access permissions and byte limits remain per handle. |
| **Hard link** | An additional pathname for the same underlying file. A symbolic link instead names another path; the native file API rejects symbolic links in the final pathname component. |
| **Mapped region** | An owned virtual-memory mapping of a file range. It retains the backing file state; its exposed byte spans borrow the region's lifetime. |
| **View** | A borrowed byte span referring to mapped storage. It does not own or extend the mapping's lifetime. |
| **Page** | The host operating system's unit of virtual-memory mapping. Its byte size is queried at runtime and need not equal the superblock's 4,096 bytes. |
| **Shared mapping** | A file mapping whose updates are visible through other mappings of the same file. Visibility alone does not provide atomic updates, thread synchronization, or power-loss durability. |
| **Atomic word** | A naturally aligned eight-byte value accessed indivisibly through the qualified native adapter. All accesses to such a live word use atomic accessors; publication ordering and storage durability are separate properties. |
| **Ordering fence** | A compiler/hardware barrier ordering memory operations around a synchronization boundary. It does not flush file contents for power-loss durability. |
| **Open file description** | The kernel state behind an opened file descriptor. Duplicated or fork-inherited descriptors share that state and its `flock` lock; separate opens have independent descriptions. |
| **Mapping lease** | Process-local bookkeeping protecting a live mapping's exposed range against resize through another handle for the same underlying file. It does not protect against external truncation. |
| **Positional I/O** | Reading or writing at an explicit file byte offset without advancing a shared file-position cursor. |
| **Fork** | The operating-system operation that creates a child process with inherited process state. Inherited Angaria owners do not become child-owned resources. |
| **Exec** | The operating-system operation that replaces the program running in a process. It removes inherited memory mappings and closes descriptors marked close-on-exec. |

`SuperblockBytes` represents the complete serialized 4,096-byte control area. `SuperblockConfiguration` contains only the process-local immutable configuration values returned by the metadata decoder. It excludes mutable roots and journal state. Successful metadata decoding does not establish a recovered, usable Session.

The [native storage contract](native-storage.md) defines file/mapping ownership, range checks, leases, and their process boundaries.

## Ownership and recovery

| Term | Meaning in Angaria |
| --- | --- |
| **Task claim** | An exclusive attachment to a logical Task, retained by its Task handle and dependent endpoints or actor-bound Registry views. OS ownership and a recorded incarnation distinguish a live owner from an earlier attachment. |
| **Registry snapshot** | An owning copy of every entity definition at one committed Registry revision, captured under one mutation gate. It includes topology states and relationships, but not historical Task attachment diagnostics. |
| **Historical cut** | The final Registry definitions visible at a chosen committed revision. All changes within that revision's atomic batch appear together; later registrations and revisions are absent. |
| **Bootstrap** | Initializing a new session's storage, creator Task, metadata, and starting state before making the session available for use. |
| **Incarnation** | The generation of a Task attachment. Reattachment advances it so identities and tokens from an older claim remain distinguishable. |
| **Session bootstrap** | Creation of the initial Session definition, creator Task registration, and creator claim before the initialized session directory becomes visible at the requested path. The Session flags word records completion. |
| **Staging directory** | An exclusively created temporary sibling of the requested session directory. Creation initializes it completely, then atomically renames it to the requested path without replacing an existing target. An unpublished directory left by a killed creator is not an attached Session. |
| **Mutation gate** | The combination of a process-local mutex and an interprocess file lock that serializes mutable session access in the initial protocol. |
| **Mutation guard** | A move-only, thread-affine owner of one mutation-gate acquisition. It retains its controller and releases the file lock before the local mutex when destroyed. |
| **Thread-affine** | Requiring acquisition, use, moves, and destruction on the same thread. This applies to mutation guards because they own a thread's mutex lock. |
| **Session controller** | Process-local shared ownership of a session file, coordination resources, mutation mutex, and Task claim bookkeeping. Opening this private resource layer does not establish recovered public Session state. |
| **Task lease** | The internal shared lifetime owner of a Task's native claim lock and optional parent lease. A lease alone does not establish a persisted Task incarnation. |
| **Provisional Task claim file** | A stable coordination file created for an unused Task ID after registration preflight but before its journal intent. An aborted attempt may leave it for a later registration to reuse. Its existence neither registers nor attaches a Task, and it is never unlinked or replaced by Angaria. |
| **Schema family** | The immutable schema versions sharing one logical message type and name. Each exact type/version pair remains a distinct identity, and a new version preserves earlier definitions and ordered Field lists. |
| **Draft entity** | A registered Queue or Queue Set whose definition can still be completed before activation. It has no sending/receiving endpoint or allocated ring. Nonzero ownership and membership references must already resolve. |
| **Activation** | The one-way transition from Draft to Active after complete ownership, membership, policy, and resource checks. A Queue's Active definition and initial ring allocation commit together. Active status makes an identity/ownership handle openable; payload operations follow their own milestones. |
| **Endpoint identity handle** | A move-only Queue writer, Source Queue Set, or Publishing Queue Set handle that retains its logical Task claim and exposes a stable identity. Step 2.9 implements opening and identity checks; delivery methods are added later. |
| **Topology revision** | A nonzero counter identifying a Publishing Queue Set's destination snapshot, starting at 1. It is separate from the Registry revision, which identifies a committed definition update across the Session. Source membership rows use their Source definition's Registry revision. |
| **Operation journal** | The reusable 2,048-byte area inside the superblock describing the current operation's intent, commit state, and inline recovery commands or append-manifest reference. |
| **Operation generation** | The journal counter distinguishing accepted operations, including fixed-state updates that do not append records. A plan proposes the last completed generation plus one; an unaccepted rolled-back intent does not advance the stored counter. |
| **Manifest** | An append-transaction record identifying the operation's new records and redo commands. It lives in the arena, separately from the reusable journal. |
| **Append batch** | One contiguous group of new arena records followed by its manifest. The initial allocator rounds each record to 64 bytes, and one journal commit establishes acceptance for the whole batch. |
| **Transaction plan** | A process-local proposal containing new records, absolute redo commands, and the snapshot on which they were based. The native journal rejects a stale snapshot before writing a new intent. |
| **Redo** | Reapplying a committed operation's intended changes so interrupted work completes. The commands are repeatable without duplicating logical effects such as delivery or sequence issuance. |
| **Redo target allowlist** | The byte-level rules permitting changes only to declared mutable words. It excludes immutable metadata/payload, reserved bytes, the journal, and final roots owned by the journal engine; it does not validate application or Registry semantics. |
| **Fault checkpoint** | A private test boundary after an actual store or file operation where a worker can pause for termination or simulate an I/O failure. It is not an application callback. |
| **Commit point** | The journal's transition to Committed, which establishes the operation's logical acceptance. Recovery can finish its remaining effects afterward. This does not itself promise power-loss durability. |
| **Delivery pin** | The retention protection for a currently issued delivery and its borrowed payload. It lasts until acknowledgment, release, Source handle closure, or claim termination, and survives ring overwrite. |

The [concurrency and recovery contract](concurrency.md) specifies these protocols. The [mapped format](mapped-format.md) specifies their byte representations.
