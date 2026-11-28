# Session format version 1

> Historical implementation note: this page describes the superseded runtime or its original qualification. Current format 3.0 and public messaging are documented in [public runtime admission](public-runtime-admission.md) and [the current format](mapped-format.md). Historical source and measurements remain pinned to their recorded revisions.

This document describes the runnable v0 prototype. The [mapped layout contract](mapped-format.md) specifies the proposed format 2.0 for the Registry and Queue Set milestones; current tools do not create or attach to that format.

A session directory contains `session.ang` and a live queue in `pending/`. Endpoint attachment also creates stable `producer.lock` and `consumer.lock` files and a state file for each role. The queue holds two offsets into the log. The log has a configurable byte cap, including its header, with a default of 1 MiB. Queue capacity and log capacity are independent.

## Sending and receiving

Send and receive operations take an exclusive advisory lock on the log for the duration of the operation. The inspector opens the log read-only and takes a shared lock. Create the session before starting the endpoints and keep the files on a local filesystem.

A send checks the queue first and the log second. Queue-full and log-full results leave both unchanged. To accept a send, Angaria writes the record body, writes the commit marker, then creates a directory named with the record's decimal byte offset under `pending/`. The directory entry publishes the record to the receiver. A process can die after committing a record but before publishing it. A publication error can leave the same state. In either case the inspector shows the record and the receiver skips it. If a call is interrupted after publication, its result may be unknown to the caller.

The consumer reads the record at the lowest pending offset and keeps that entry until explicit acknowledgment. Repeated receives return the same in-flight delivery. Acknowledging its sequence number atomically removes the pending directory entry and frees a queue slot. Acknowledgments for another sequence or with no in-flight delivery fail. Closing or losing a consumer before acknowledgment leaves the message available to its replacement. Death after the entry is removed leaves the next message pending.

`remove` claims both roles before discarding the live queue and preserves the log and role files. The commit marker provides process-crash recovery on a running system. Power-loss durability is a separate guarantee.

## Endpoint ownership and recovery

The producer has task ID `1` and the consumer has task ID `2`. An endpoint holds an exclusive nonblocking `flock` on its role file until destruction or process exit. A competing claim throws `RoleBusy`. Producer and consumer roles can coexist and restart independently. Lock files retain the same inode for the life of the session.

A replacement validates the log header, all complete records, and every published offset before use. It preserves committed messages and pending deliveries. A partial final log record is repaired by the next accepted send. Committed records that have no pending entry remain available to the inspector. Version 1 sessions created before role ownership was added acquire role files on first attachment.

After validation, attachment advances the role's incarnation number. Each role state file contains one ASCII line with the format `ANGARIA_ROLE_1 TASK_ID INCARNATION PID`. The incarnation starts at `1`. The PID records the latest attaching process for diagnostics. Angaria writes a replacement `.state.tmp` file completely and renames it over the prior state while holding the role lock. An interrupted temporary write is overwritten by the next attachment. A failed attachment can leave a gap in incarnation numbers if it dies after the state replacement.

Role descriptors have close-on-exec set. Fork handlers close inherited role descriptors in the child, and inherited endpoint objects reject operations. A child can attach its own endpoints when the corresponding role is available. Applications that fork while another thread is inside an Angaria operation should exec before using Angaria in the child.

A waiting consumer checks the queue on attachment and on every receive. While waiting for a message, it releases the operation lock and polls at intervals of at most 10 ms, up to the requested timeout. Publication therefore requires no wakeup notification. An unacknowledged delivery remains pending across consumer restarts. Applications with external side effects must tolerate repeated processing after a crash before acknowledgment.

## File header

Offsets are in bytes. Multibyte integers are little-endian. Signed values use two's complement.

| Offset | Size | Value |
| ---: | ---: | --- |
| 0 | 8 | Magic bytes `ANGARIA\0` |
| 8 | 2 | Format version, currently `1` |
| 10 | 2 | Total header size |
| 12 | 8 | Maximum log size in bytes |
| 20 | 2 | Schema ID, nonzero |
| 22 | 1 | Field kind, `1` for signed 32-bit integer |
| 23 | 1 | Queue name length |
| 24 | 1 | Message name length |
| 25 | 1 | Field name length |
| 26 | Variable | Queue, message, and field names in that order |

Each name has 1 to 63 ASCII letters, digits, underscores, or dashes and has no terminator. Version 1 supports one immutable schema per session. The writer uses schema ID `1` for `Reading` with an integer field named `value`. The inspector obtains the schema and all three names from the file.

## Message records

Records follow the header with no padding. Each starts at `header_size + 30 × (sequence − 1)`.

| Offset in record | Size | Value |
| ---: | ---: | --- |
| 0 | 4 | Total record length, `30` |
| 4 | 8 | Sequence number, starting at `1` |
| 12 | 8 | Signed UTC nanoseconds since the Unix epoch |
| 20 | 2 | Schema ID from the header |
| 22 | 4 | Signed integer payload |
| 26 | 4 | Commit marker bytes `CMIT` |

The inspector skips a final record shorter than 30 bytes and reports malformed full-length records as errors. A subsequent send that passes both capacity checks overwrites a partial tail from its beginning, keeping all committed records and their sequence numbers intact.
