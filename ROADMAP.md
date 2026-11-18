# Angaria roadmap

Angaria is a C++ library for inspectable local IPC. The first release targets Linux and demonstrates one producer and one consumer exchanging a typed message through a named queue. After both processes exit, a read-only tool can decode the session file using the schema recorded in that file.

## 1. Establish a build and demo

Implemented.

- Add a CMake build and Linux CI. Fix the prototype headers that the demo uses and verify they compile from a fresh checkout.
- Provide a small producer and consumer with one bounded FIFO and one registered message type. Document the commands to create a session and run both endpoints.
- Keep v0 to one producer, one consumer, and one in-flight delivery.

## 2. Define the queue and session file

Implemented.

- Give the live queue a fixed capacity and the append-only session log its own byte cap. Return distinct queue-full and log-full results. A rejected send leaves the queue and log unchanged.
- A successful send commits a record for inspection and publishes it to the live queue. If the producer dies between those steps, a committed record can be visible in the inspector without ever being delivered. Recovery preserves published entries and leaves committed but unpublished records visible in the inspector.
- Define a versioned file format with explicit byte order, fixed-width encodings, record lengths, and a way to detect a partial final record. Keep each registered schema immutable for the life of the session. Store a schema identifier in each message record.
- Keep message records immutable after publication. Store delivery progress separately from the message so it can later be tracked per destination.
- Add an inspector that shows queue name, sequence number, timestamp, message type, and decoded fields. Test inspection after normal shutdown and producer termination. Power-loss durability requires a separate contract.

## 3. Support independent endpoint restart

Implemented.

- Give each role a stable task ID and a separate exclusive Linux file lock scoped to the session. Return a role-busy result to a competing claimant. Use the PID for diagnostics and advance the role's incarnation number on each successful attachment.
- Keep the lock files stable for the lifetime of the session and prevent child processes from retaining role lock descriptors. A replacement validates the session and recovers its side of the queue before using it.
- Preserve already-published entries when replacing the producer. Make the consumer recheck queue state on attach and after a bounded wait so a crash between publication and notification cannot strand an entry.
- Advance the consumer cursor only on explicit acknowledgment. If the consumer exits before acknowledging a delivery, its replacement receives that delivery again. Applications with external side effects must tolerate duplicates if a crash occurs after the effect and before acknowledgment.
- Test simultaneous claims for one role, independent producer and consumer restarts, and termination at the log commit, queue publication, notification, and acknowledgment boundaries.

## 4. Release the demonstration

Implemented.

From a fresh checkout, a reader can build the examples, send a typed message, replace either endpoint while the other stays up, and verify that an unacknowledged delivery is retried. After both processes exit, the reader can inspect the session and decode the recorded fields.

The [automated walkthrough](test/restart_demo.py) exercises this sequence through the demo executable. CTest and Linux CI run it as `restart_demo`. The [README](README.md) includes build and walkthrough commands for a fresh checkout.

## After v0

Use the demo to evaluate explicit replay, priority queue sets, fan-out, schema evolution, additional storage policies, and latency optimizations. Define delivery and acknowledgment per destination before adding fan-out.

[Original design wiki](https://github.com/smangano/angaria/wiki/Angaria)
