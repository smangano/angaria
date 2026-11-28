# Angaria terminology

These definitions describe the terms used in Angaria's storage and recovery design. The [domain-object definitions](milestones/milestone-1.md#domain-objects) cover Registry, Task, Queue, both Queue Set kinds, Message, Publication, and Delivery. Introduce specialized terms with a definition or a link to their canonical definition.

## Independent messaging protocol

Framework **0.4.39** admits this protocol through the public API. The [public recorded runtime](public-runtime-admission.md) describes the supported profile and its limits; the [protocol design](independent-messaging-protocol.md) explains the synchronization. Ordinary established messaging does not acquire the Session administrative gate. Earlier Session-message-journal implementations are historical, rather than an alternate runtime.

| Term | Meaning in the current protocol |
| --- | --- |
| **Producer stream** | One Task-owned append log shared by its outbound Queues, with independent allocation/commit state. |
| **Committed tail** | The atomic end offset of that stream's immutable accepted prefix. Its publication accepts a message; reserved file length and ring hints do not. |
| **Replay cut** | A fixed acquired committed boundary and record count, bound to one producer file identity. Iteration excludes later acceptances. A Session archive captures a vector of these per-producer cuts; it does not create a single global instant or recover exact consumer execution order. See [public replay](public-runtime-admission.md#replay-export-and-inspection). |
| **Producer owner file** | Native per-producer incarnation storage bound to the log file's identity. Its counter advances before new IDs are exposed, including after an owner accepted nothing. It is separate from immutable message records. |
| **Publication capacity** | The configured maximum accepted publication-record count in the bounded stream. It bounds lookup storage and acknowledged destination receipt reservations. |
| **Logical credit** | A monotonic per-Queue consumed-sequence watermark limiting producer admission independently of descriptor publication and application acknowledgment. |
| **Processing acknowledgment** | The application's explicit completion of an acknowledged Delivery. Consumer-owned progress commits it separately from message acceptance and transport credit. Neither acknowledgment nor returned credit reclaims retained log capacity. |
| **Descriptor stamp** | An atomic Queue sequence published after a ring slot's reference words. It identifies a delivery hint; the producer log's committed tail remains acceptance authority. |
| **Connection generation** | A nonzero immutable identity for a fixed connection setup, checked at open and in matching descriptor hints. It is separate from producer/consumer Task incarnations. |
| **QueueCreditFull** | A private nonblocking send result: the new ID remains unaccepted because the Queue's logical credit is full. The caller may retry the same ID after consumption; no Error allocation or processing acknowledgment is required. |
| **Admitted connection** | An Active Queue and its Source whose ownership, policies, schemas, sealed catalog and native resources meet the implemented operational profile. Metadata activation alone does not admit every described policy combination. |
| **Sealed catalog / catalog seal** | The validated Registry/schema/topology snapshot and its persisted final Registry revision. The first valid endpoint opening or publication-ID issuance bootstraps resources and commits this seal. Later definition/topology mutation is rejected; replacing a Task changes its incarnation, not the catalog. Read-only capture requires an existing seal. |
| **Source operation image** | A reserved consumer-owned description of one operation's absolute Source, selected Queue and receipt after-state. An atomic selector commits it; interrupted installation is replayed locally. |
| **Receipt ledger** | Reserved consumer-owned rows retaining each accepted destination's latest issuance and acknowledgment metadata in acknowledged mode. It is separate from the immutable message log and optional execution-event trace. See the [acknowledged progress file](recorded-acknowledged-progress.md#progress-file-10). |
| **Source operation generation / selector** | The nonwrapping counter identifying a consumer-owned progress operation, independent of Task incarnation and connection generation. Publishing the selector commits that operation; generation parity selects one of two fixed operation images. |
| **Applied generation** | The committed Source operation whose after-state has finished installation into the working/receipt rows. A lagging value requires replay of the selected operation. It does not certify hint publication; attachment/settlement also reconciles credit and acknowledgment hints. |

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
| **Arena** | Administrative storage for variable-sized allocations beyond the Session's fixed reservation, including definitions, transaction manifests and topology reservations. Current message records, connection descriptors and processing receipts live in separate producer/connection/progress files. |
| **Committed prefix** | A sequence of committed records ending at its owning stream's authoritative tail. The administrative Session arena and each producer log have separate tails and recovery rules; a Session scan does not establish message acceptance. |
| **Logical offset** | An unsigned 64-bit byte address within the session's storage layout. Two-plane storage uses file offsets; segmented storage translates the address into its owning extent. Zero denotes a null reference. |
| **Root** | A top-level stored reference or control value identifying current state, such as the latest manifest reference, Registry revision, or allocation tail. A root is not necessarily a pointer or offset. |
| **Extent** | A contiguous file allocation representing one logical address interval in segmented storage. It has its own header and can occupy multiple allocation quanta. |
| **Segment allocation quantum** | The configured `segment_bytes` unit used to size new extents. It is an allocation increment, rather than a maximum message size. |

## Encoding and native resources

| Term | Meaning in Angaria |
| --- | --- |
| **Codec** | Code that encodes process-local values into specified bytes and decodes those bytes into values. The superblock codec also validates immutable metadata and section geometry. It does not serialize native C++ object layouts. |
| **Required capability** | A bit in the immutable superblock required-feature mask. Live binaries must understand the required format, capabilities and synchronization revision before mutable access. Current public Sessions use format **3.0**, required-feature mask **0x7** and synchronization revision **2**; earlier formats are rejected. |
| **Message record** | A portable 80-byte header plus application payload, defined by the [message-record contract](message-record.md). It carries a complete publication identity and exact schema context without native offsets or destination receipt state. Encoding it does not admit a publication. |
| **Message / MessageView / Delivery** | Message owns its header, payload bytes, and bound decoding context. MessageView owns the same MessageHeader value and borrows bytes. Delivery combines a MessageView with its active acknowledgment/release token. Copying a message preserves its entire header but does not retain the delivery obligation; see the [API contract](api.md#messages-and-receive-events). |
| **Log archive** | An owning public view of accepted publications within captured per-producer cuts and their exact schema catalog. `LogArchive::capture_live` does not attach a Task, seal setup, acknowledge processing or capture receipts/execution traces. |
| **Capture** | An ordinary binary stream of consecutive canonical message records, with no additional header or embedded definitions. A Log archive can export this flat format; retain the matching catalog for schema-aware inspection. [Capture helpers](capture-inspection.md) bound and validate framing; their I/O does not establish Queue admission, delivery, or durability. |
| **Inspection catalog** | An owning copy of a source Session's Field and message-schema definitions used by the [generic inspector](capture-inspection.md#retain-the-source-catalog). It retains exact versions independently of native handles and requires the separately retained source Session identity. |
| **Opaque payload** | Bytes preserved without interpreting their contents. A message record can frame a known payload format without implementing its serializer or validating that format's application syntax. |
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
| **View** | A borrowed byte span referring to retained storage. It does not own or extend its public borrow. In acknowledgment-free receive, the next receive invocation ends the preceding MessageView borrow, including an empty attempt; an owning Message copy survives that boundary. |
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
| **Mutation gate / administrative gate** | The process-local mutex and interprocess file lock serializing Registry setup, Task attachment bookkeeping, administrative recovery and admission. Established ID issuance, send/status, receive and processing progress do not acquire it. Producer and Source ownership instead protect their own state. |
| **Mutation guard** | A move-only, thread-affine owner of one mutation-gate acquisition. It retains its controller and releases the file lock before the local mutex when destroyed. |
| **Thread-affine** | Requiring acquisition, use, moves, and destruction on the same thread. This applies to mutation guards because they own a thread's mutex lock. |
| **Session controller** | Process-local shared ownership of a session file, coordination resources, mutation mutex, and Task claim bookkeeping. Opening this private resource layer does not establish recovered public Session state. |
| **Task lease** | The internal shared lifetime owner of a Task's native claim lock and optional parent lease. A lease alone does not establish a persisted Task incarnation. |
| **Provisional Task claim file** | A stable coordination file created for an unused Task ID after registration preflight but before its journal intent. An aborted attempt may leave it for a later registration to reuse. Its existence neither registers nor attaches a Task, and it is never unlinked or replaced by Angaria. |
| **Schema family** | The immutable schema versions sharing one logical message type and name. Each exact type/version pair remains a distinct identity, and a new version preserves earlier definitions and ordered Field lists. |
| **Draft entity** | A registered Queue or Queue Set whose definition can still be completed before activation. It has no sending/receiving endpoint or allocated ring. Nonzero ownership and membership references must already resolve. |
| **Activation** | The one-way metadata transition from Draft to Active after ownership, membership, policy and administrative resource checks. Operational admission subsequently validates the supported complete graph and creates its producer, transport and optional progress files before sealing. |
| **Endpoint handle** | A move-only public QueueWriter or SourceQueueSet retaining its logical Task claim and admitted resources. Publishing Queue Set execution remains a later milestone. |
| **Topology revision** | The version of a fixed routing definition, separate from a Task incarnation or connection generation. An admitted Queue connection uses its Queue activation's Registry revision; its producer log binds the whole sealed catalog revision. Publishing metadata has its own destination-snapshot counter. |
| **Operation journal** | The reusable 2,048-byte administrative area inside the Session superblock describing an administrative operation's intent, commit state, and inline recovery commands or append-manifest reference. It does not commit current message acceptance or acknowledgments. |
| **Operation generation** | The journal counter distinguishing accepted operations, including fixed-state updates that do not append records. A plan proposes the last completed generation plus one; an unaccepted rolled-back intent does not advance the stored counter. |
| **Journal recovery** | Finishing a committed administrative operation or rolling back an unaccepted Prepared operation. It is separate from producer-log recovery, consumer-owned progress repair and application message replay. See the [failure qualification](native-qualification.md). |
| **Manifest** | An append-transaction record identifying the operation's new records and redo commands. It lives in the arena, separately from the reusable journal. |
| **Append batch** | One contiguous group of new arena records followed by its manifest. The initial allocator rounds each record to 64 bytes, and one journal commit establishes acceptance for the whole batch. |
| **Append allocation plan** | Size-only accounting for every record, immutable/mutable boundary, envelope, manifest reference, redo frame, and padding in an append batch. It checks the complete byte ceiling before payload images are constructed; it does not reserve space or commit acceptance. |
| **Transaction plan** | A process-local proposal containing new records, absolute redo commands, and the snapshot on which they were based. The native journal rejects a stale snapshot before writing a new intent. |
| **Redo** | Reapplying a committed operation's absolute after-values so interrupted work completes without repeating its logical effect. The administrative journal and consumer progress images have distinct owners and commit points. |
| **Redo target allowlist** | The byte-level rules permitting changes only to declared mutable words. It excludes immutable metadata/payload, reserved bytes, the journal, and final roots owned by the journal engine; it does not validate application or Registry semantics. |
| **Fault checkpoint** | A private test boundary after an actual store or file operation where a worker can pause for termination or simulate an I/O failure. It is not an application callback. |
| **Commit point** | The authoritative publication of an operation: the producer committed tail for acceptance, the consumer selector for processing progress, or the administrative journal's Committed state for administrative changes. Recovery may finish derived effects afterward. None alone promises power-loss durability. |
| **Delivery pin** | The Source's outstanding acknowledged Delivery, represented in consumer progress by its exact issuance/token. Repeated receive retains it; acknowledgment completes it, while release or close permits redelivery. Retained log bytes remain available independently of ring reuse. |
| **Acknowledged prefix** | The contiguous leading Queue sequence whose deliveries have committed processing acknowledgments. The producer-visible atomic hint is a conservative committed lower bound and can lag until consumer recovery reconciles it. |
| **Reserved progress** | Consumer-owned processing state, two operation images and receipt rows allocated before messaging. Receive, acknowledgment, release and replacement repair need no message append or Session journal space, including when the producer log is full. See [acknowledged progress](recorded-acknowledged-progress.md). |

Historical Session-gated documents also use **terminal receipt**, **Delivery gap** and **DeferredClose** for their former allowed-loss and cleanup engine. That engine has been retired; allowed loss and its gap behavior are not admitted by the current public profile.

The [concurrency and recovery contract](concurrency.md) specifies these protocols. The [mapped format](mapped-format.md) specifies their byte representations.
