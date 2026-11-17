# Session format version 1

A session directory contains `session.ang` and a live queue in `pending/`. The queue holds two offsets into the log. The log has a configurable byte cap, including its header, with a default of 1 MiB. Queue capacity and log capacity are independent.

## Sending and receiving

Send and receive operations take an exclusive advisory lock on the log for the duration of the operation. The inspector opens the log read-only and takes a shared lock. Create the session before starting the endpoints and keep the files on a local filesystem.

A send checks the queue first and the log second. Queue-full and log-full results leave both unchanged. To accept a send, Angaria writes the record body, writes the commit marker, then creates a directory named with the record's decimal byte offset under `pending/`. The directory entry publishes the record to the receiver. A process can die after committing a record but before publishing it. A publication error can leave the same state. In either case the inspector shows the record and the receiver skips it. If a call is interrupted after publication, its result may be unknown to the caller.

The receiver reads the record at the lowest pending offset and removes that entry. Receiving leaves the log intact. `remove` discards the live queue and preserves `session.ang`. The queue does not yet track acknowledgments. The commit marker provides process-crash recovery on a running system. Power-loss durability is a separate guarantee.

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
