# Native file and mapping support

Steps 2.2 and 2.3 implement the private native resources used by the future format-2 backend. In the [file/mapping API](../src/native_file.h), `NativeFile` owns a native file descriptor, and `MappedRegion` owns a shared memory mapping. Both use move-only [RAII](glossary.md#encoding-and-native-resources): moving transfers ownership, and destruction releases the owned resource without throwing. The [controller](../src/session_controller.h) adds shared process-local ownership, the mutation gate, and Task lifetime claims; the [atomic adapter](../src/native_atomic.h) supplies native word access and ordering.

A **file descriptor** is the process-local integer handle returned by the operating system when a file opens. A **mapped region** makes a file byte range accessible through virtual memory. Its returned **view** is a borrowed C++ byte span, not an owner. The [glossary](glossary.md#encoding-and-native-resources) defines the related terms, including inode identity, host pages, positional I/O, and mapping leases.

The file/mapping owners alone do not acquire the session mutation gate or interpret session contents. The controller validates immutable superblock metadata and native compatibility, but does not recover the journal or create a usable public Session. Step 2.4 adds journal transactions and recovery. Resize and write operations here are ordinary native I/O rather than committed Angaria publications.

## Files and access

| Operation | Behavior |
| --- | --- |
| `NativeFile::create_exclusive(path, max_bytes)` | Creates an empty regular file with exclusive creation. An existing path returns Conflict without truncation. Growth is a separate `resize()` operation. |
| `NativeFile::open_existing(path, access, max_bytes)` | Opens an existing regular file in explicit ReadOnly or ReadWrite mode. It never creates or truncates the file. |
| `size()` | Checks the descriptor's current file identity and length. The file must remain regular and within its supported byte limit. |
| `identity()` | Returns the opened file's device/inode identity. An inode identifies the underlying file independently of its pathname. |
| `native_handle()` | Returns a borrowed descriptor. Keep its owner alive while using it; callers must not close, replace, duplicate over, or resize that descriptor outside this API. |
| `read_exact(offset, output)` | Reads the complete requested range with positional I/O, handling short reads and interrupted calls. An unexpected end of file (EOF) is an error. |
| `write_exact(offset, input)` | Writes the complete requested range with positional I/O. It does not grow the file implicitly. |

The positive per-handle byte limit must fit the format's signed 64-bit range and the host's file-offset type. File ranges are checked with overflow-safe arithmetic before native I/O. Empty read/write spans are allowed at the end of the file; ranges beyond the current file length fail. A read or write that fails after I/O begins can have transferred part of its range. Higher layers need the journal protocol when atomic logical updates are required.

Descriptors are close-on-exec, so a successful [exec](glossary.md#encoding-and-native-resources) does not inherit them. The final pathname component cannot be a symbolic link. Nonregular files, including directories and named pipes (FIFOs), are rejected; nonblocking open prevents waiting on a pipe peer during validation. Read/write permissions and byte limits remain specific to each handle, even when another handle opens the same underlying file with different access. [Native open semantics](https://man7.org/linux/man-pages/man2/open.2.html)

## Mapping ranges and lifetimes

`map_read_only(offset, length)` and `map_read_write(offset, length)` require a nonempty range contained in the current file. A writable mapping also requires a ReadWrite file handle. `MappedRegion::bytes()` returns a read-only view; `writable_bytes()` returns a writable view only for a writable mapping.

The API accepts arbitrary byte offsets. It queries the host **page size**, rounds the native mapping offset down to a page boundary, checks the enlarged backing length against host addressable limits, and exposes only the originally requested span. This avoids assuming that the host uses 4,096-byte pages. Mappings request shared read-only or read/write protection, do not request execution, and let the OS choose the virtual address. The OS mapping requirements are documented by [Linux mmap](https://man7.org/linux/man-pages/man2/mmap.2.html) and [Apple mmap](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/mmap.2.html).

A mapping retains the originating file state, including its descriptor and range protection, after the `NativeFile` wrapper is destroyed or moved away. Its spans remain valid until the mapping's owning state is destroyed or replaced, subject to the coordination rules below. Moving a region transfers that lifetime without changing the mapped address. Move assignment releases the destination's previous mapping; spans borrowed from that previous mapping expire. Empty or moved-from owners reject access.

## Controlled resize and aliases

A **mapping lease** records the end of a live mapping's exposed byte range. Every handle for the same device/inode shares one process-local mutex and lease collection. This includes independent opens, alternate pathnames, and hard links. A mapping retains that collection through its backing file state.

Range validation, creation of a mapping and its lease, and resizing all use the same per-inode mutex. `resize(new_size)` permits growth, exact-size no-ops, and shrinking that leaves every live exposed range intact. Shrinking below the greatest leased end returns OwnershipBusy before `ftruncate`. Closing or replacing a mapping releases its lease. Growing the file does not enlarge existing spans; map a new range to access added storage.

Later journal rollback must release any mapping created for an uncommitted append whose exposed range extends beyond the length being restored. Existing prefix mappings wholly below that boundary can remain alive during the shrink.

The native resize primitive sets the exact file length and discards a removed suffix. Newly extended file bytes read as zero. This is file-length management, not proof of physical disk reservation or power-loss durability. [Native truncate semantics](https://man7.org/linux/man-pages/man2/truncate.2.html)

The mutex protects resource bookkeeping and the wrapper's I/O/resize operations. It does **not** synchronize direct reads and writes through returned mapped spans. Callers must coordinate shared data access separately.

Lease protection is process-local. Another process or code bypassing this API can truncate a file after it was checked. A previous size check cannot make retained spans safe against that race; do not access a span after uncoordinated truncation can have invalidated its range. Access beyond the shortened file can fault. The later mutation-gate and retention protocols coordinate cooperating participants; they cannot protect against arbitrary external truncation.

## Process ownership and fork

Files and mappings belong to the process that created their owners. Operations check that process identity before acquiring any inherited mutex. Inherited handles return WrongOwner. The process-local inode registry initializes on first use, including before `main`; once initialized, its factory calls also return WrongOwner in a fork child until the child execs a new program. A child whose parent never initialized native storage has no inherited registry to reject. A successful exec starts fresh storage state and removes inherited mappings.

This private layer does not implement general fork safety. After a multithreaded fork, the child must exec or call `_exit` before using this layer or cleaning up inherited C++ owners. The same cleanup restriction applies when a quiescent fork inherits an active mutation guard: its copied locked mutex cannot become child-owned. Inherited mutex or allocator state may have belonged to a thread that no longer exists. Mapping cleanup avoids locking the copied per-inode mutex in a child, but that does not make arbitrary post-fork C++ cleanup safe. Usable public Session attachment follows journal recovery and bootstrap in steps 2.4 and 2.5.

## Shared controller and mutation gate

`SessionController::open_existing(directory)` opens an existing `session.ang`, `mutation.lock`, and `claims` directory. It decodes immutable [superblock](mapped-format.md#superblock) metadata, checks that the file contains its complete fixed-section prefix within the recorded byte ceiling, and compares its required features, storage mapping, synchronization version, and ABI profile with the native adapter. It does not create missing coordination resources or repair session contents. Exclusive creation of the session directory and its initial resources belongs to step 2.5.

Controllers are shared within a process by session identity and underlying file identity. Directory aliases resolve to the same controller. A live session copy or a conflicting set of coordination resources is rejected instead of silently introducing an independent mutation gate for the same session. Coordination files must remain stable; they are never unlinked or replaced by these primitives.

A **mutation guard** owns one acquisition of the [mutation gate](glossary.md#ownership-and-recovery). `acquire_mutation()` takes the controller's local mutex before exclusive `flock` on `mutation.lock`; `try_acquire_mutation()` attempts both without waiting for gate ownership and returns OwnershipBusy on contention. Nested acquisition on the owning thread also returns OwnershipBusy. The local mutex excludes threads even though they share a file-lock descriptor. Native ordering fences surround the protected work. Normal guard destruction releases the file lock before the local mutex, including when an exception unwinds the operation.

The guard retains its controller and is move-only. It is **thread-affine**: acquisition, use, moves, and destruction must stay on the acquiring thread. Destroying a live guard on another thread, or an unrecoverable native unlock failure, calls `std::terminate` rather than continuing with invalid lock ownership. `file(guard)` supplies a borrowed file owner for later journal operations; the guard must belong to that controller and remain alive for the entire use. The resource API does not make arbitrary callbacks, mapped-span access, or unlocked file use safe.

## Task lifetime claims

`create_task_claim_file(guard, task)` creates or validates a stable `claims/task-<id>.lock` under the mutation gate. It is a resource primitive; it does not register a Task or persist an incarnation. `try_claim_task(task, parent)` opens that existing file through a distinct open description and attempts an exclusive nonblocking lock. A process-local claim table also rejects duplicate ownership. A different Task has an independent lifetime claim.

The returned shared `TaskLease` retains its controller. Its last owner releases the claim, so later Task handles, endpoints, and actor-bound Registry views can share its lifetime. A supplied parent lease is retained as well and must belong to the same controller and a different Task. Validating registered Task kinds and parent relationships, installing incarnations, and cleaning up predecessor state follow in the Session/Registry steps.

The lock order remains Task claim, local session mutex, then session file lock. `probe_task_claim(task)` returns whether a claim is busy; its native lock attempt never waits, including during recovery under a mutation guard. A free probe is only an observation, not a reservation. Claim attempts and probes return OwnershipBusy if their local claim bookkeeping is contended. Claim files remain present after their owners close. Nonblocking ownership attempts do not promise bounded filesystem I/O, allocation, or execution latency.

Controller lock descriptors are close-on-exec. Registered `fork` handlers close the child's inherited lock descriptors **without** issuing `LOCK_UN`, which would also unlock the parent's shared open description. This both preserves a live parent's exclusion and prevents a surviving child from retaining a dead parent's locks. Inherited controller/lease operations and factories bound to initialized parent state return WrongOwner. These handlers do not remove the multithreaded-fork restriction above. [Native fork-handler scope](https://man7.org/linux/man-pages/man3/pthread_atfork.3.html)

The descriptor registry has process lifetime because registered fork handlers cannot be removed and global owners can release their resources during static destruction. This retains the bookkeeping mutex and callback state until process exit; individual descriptors still close when their owners are destroyed.

## Native atomic qualification

`qualify_native_profile()` checks compiler/OS/CPU prerequisites: a recognized Linux or macOS x86-64/ARM64 profile, little-endian representation, 64-bit addresses and file offsets, and always-lock-free eight-byte compiler atomics. The result supplies the native `LiveCompatibility` used by the controller. It does not test an arbitrary filesystem or certify another build's environment. Qualification tests must run on the actual target; the four profile identifiers remain separate platform targets.

An **atomic word** is an indivisibly accessed, naturally aligned eight-byte value. `AtomicWord::bind(bytes, offset)` validates the complete range and eight-byte address alignment before returning a borrowed accessor. The caller retains the mapping and uses atomic access exclusively for that word. `load_acquire()` and `store_release()` provide the ordering required by record readiness. `publish_control()` uses a sequentially consistent store followed by a full compiler/hardware fence, ordering both the preceding body and subsequent state or resource effects. `native_gate_fence()` provides the gate-boundary ordering. See the [native ordering contract](concurrency.md#live-profiles-and-atomic-words) and [GCC atomic builtins](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html).

These are explicit native ABI operations over coherent mapped storage, not portable serialization of C++ objects. Unsupported prerequisites and incompatible recorded profiles fail before session mutation. Successful atomic access does not flush storage or provide power-loss durability.

## Failures and validation scope

| Result | Meaning here |
| --- | --- |
| `InvalidArgument` | Invalid path, access mode, byte limit, range/alignment, Task ID, or supplied parent lease. |
| `InvalidState` | Empty/moved-from owner or guard, or a write, writable mapping, or resize attempted through read-only access. |
| `WrongOwner` | Inherited process ownership, a guard used on another thread, or a guard belonging to another controller. |
| `Conflict` | Exclusive creation found an existing path, or a live session identity/coordination namespace conflicts with another controller. |
| `CapacityExceeded` | Requested resize exceeds the handle's or native format's byte limit. |
| `OwnershipBusy` | Resize would invalidate a mapping, a gate/Task is already owned, or local claim bookkeeping is contended. |
| `UnsupportedPolicy` | Recorded segmented storage has no live implementation yet. |
| `IoError` | Native I/O failure, invalid/replaced resources or metadata, unsupported native compatibility, unexpected EOF, or a range beyond the file. |

Errors include a diagnostic path and message. Destruction releases descriptors and mappings without throwing. It does not delete the underlying file, perform journal rollback, or provide a synchronous durability flush.

The [file/mapping tests](../test/test_native_file.cpp) cover ownership transfer, file validation, read-only behavior, range boundaries, shared mappings, resize protection through aliases, fork/exec policy, and descriptor cleanup. A [startup regression](../test/native_storage_startup.cpp) also exercises file and mapping creation before `main`. The [controller tests](../test/test_session_controller.cpp) and [atomic tests](../test/test_native_atomic.cpp) exercise native exclusion and ordering with threads and independent processes. Crash-recoverable logical transactions and usable format-2 Session behavior remain the later steps' acceptance criteria.

[Milestone 2](milestones/milestone-2.md) · [Mapped format](mapped-format.md) · [Concurrency](concurrency.md)
