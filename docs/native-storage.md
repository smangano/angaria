# Native file and mapping support

Step 2.2 implements the private [native storage API](../src/native_file.h) used by the future format-2 backend. `NativeFile` owns a native file descriptor, and `MappedRegion` owns a shared memory mapping. Both use move-only [RAII](glossary.md#encoding-and-native-resources): moving transfers ownership, and destruction releases the owned resource without throwing.

A **file descriptor** is the process-local integer handle returned by the operating system when a file opens. A **mapped region** makes a file byte range accessible through virtual memory. Its returned **view** is a borrowed C++ byte span, not an owner. The [glossary](glossary.md#encoding-and-native-resources) defines the related terms, including inode identity, host pages, positional I/O, and mapping leases.

These primitives do not create a usable Session, acquire the session mutation gate, interpret the superblock, or recover a journal. Step 2.3 adds native ownership/synchronization qualification, and step 2.4 adds journal transactions and recovery. Resize and write operations here are ordinary native I/O rather than committed Angaria publications.

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

This initial private layer does not implement general fork safety. After a multithreaded fork, the child must exec or call `_exit` before using this layer or cleaning up inherited C++ owners. Its inherited mutex or allocator state may have belonged to a thread that no longer exists. Mapping cleanup avoids locking the copied per-inode mutex in a child, but that does not make arbitrary post-fork C++ cleanup safe. Step 2.3 integrates the ownership/controller policy before usable public Session attachment is introduced.

## Failures and validation scope

| Result | Meaning here |
| --- | --- |
| `InvalidArgument` | Empty/null-containing path, unknown access mode, unsupported byte limit, zero-length mapping, or unrepresentable range. |
| `InvalidState` | Empty/moved-from owner or a write, writable mapping, or resize attempted through read-only access. |
| `WrongOwner` | The native owner or factory belongs to another process after fork. |
| `Conflict` | Exclusive creation found an existing path. |
| `CapacityExceeded` | Requested resize exceeds the handle's or native format's byte limit. |
| `OwnershipBusy` | The resize would invalidate a live leased range. |
| `IoError` | Native I/O/mapping failure, invalid file type/identity/size, unexpected EOF, or a requested range beyond the current file. |

Errors include a diagnostic path and message. Destruction releases descriptors and mappings without throwing. It does not delete the underlying file, perform journal rollback, or provide a synchronous durability flush.

The [native storage tests](../test/test_native_file.cpp) cover ownership transfer, file validation, read-only behavior, range boundaries, shared mappings, resize protection through aliases, fork/exec policy, and descriptor cleanup. A [startup regression](../test/native_storage_startup.cpp) also exercises file and mapping creation before `main`. These checks exercise the native storage primitives. Atomic/profile qualification, crash-recoverable logical transactions, and usable format-2 Session behavior remain the later steps' acceptance criteria.

[Milestone 2](milestones/milestone-2.md) · [Mapped format](mapped-format.md) · [Concurrency](concurrency.md)
