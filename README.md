# Angaria

Angaria is a C++ inter-process messaging framework in development around a shared persistent Registry, Source Queue Sets, Publishing Queue Sets, and selectable policies. The [roadmap](ROADMAP.md) records the agreed architecture and ten milestones based on the [original design wiki](https://github.com/smangano/angaria/wiki/Angaria). Milestone 1 is complete, and [milestone 2](docs/milestones/milestone-2.md) is under implementation.

The current v0 prototype exchanges an integer message through a bounded filesystem queue, supports independent endpoint restart and explicit acknowledgment, and records messages in an inspectable session file. The commands below run that prototype.

## Build and test

On Ubuntu 24.04, install a C++20 compiler, CMake, Boost headers, GoogleTest, and Python 3.9 or later for the automated walkthrough.

```sh
sudo apt-get install build-essential cmake libboost-dev libgtest-dev python3
git clone https://github.com/smangano/angaria.git
cd angaria
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Configuration contract

The [configuration and lifecycle contract](docs/configuration.md) defines the proposed TOML schema for the Registry implementation. The [trading example](examples/trading.toml) exercises process and thread Tasks, both Queue Set types, and the delivery policies.

CTest parses this example with pinned cpptoml revision `fededad7169e538ca47e11a9ee9251bc361a9a65`. CMake fetches those headers when testing is enabled. A local checkout of that revision can be supplied for an offline build.

```sh
cmake -S . -B build -DFETCHCONTENT_SOURCE_DIR_CPPTOML=/absolute/path/to/cpptoml
```

Milestone 2 integrates configuration loading and semantic validation with the Registry.

## Public API contract

The [public C++ API contract](docs/api.md) describes the identities, descriptors, results, and endpoint interfaces under milestone 1. The [trading API example](examples/trading_api.cpp) is compiled with each build as `angaria_api_contract`. Runtime definitions follow the Registry, message, and Queue Set milestones.

The [mapped layout and compatibility contract](docs/mapped-format.md) defines the proposed format 2.0, capacity accounting, and migration rules. Its checked reservation arithmetic is tested; format-2 file I/O and synchronization follow in the implementation milestones.

The [concurrency and recovery contract](docs/concurrency.md) specifies the blocking mutation gate, Task claims, journal commit/replay, full-log delivery progress, and notification protocol. CTest runs a bounded crash-recovery reference model; native format-2 runtime qualification follows in the implementation milestones.

Step 2.1 implements the format-2 backend foundation: a shared checked section planner and a portable [superblock codec](src/mapped_superblock.h), with byte-fixture, malformed-input, and compatibility tests. It encodes a provisional prefix and validates immutable metadata; it does not create a usable Session or perform native file/mapping/journal recovery. Those operations follow in the [milestone 2 steps](docs/milestones/milestone-2.md).

## Run the restart walkthrough

The walkthrough starts a producer and consumer, kills and replaces the producer while the consumer stays attached, then kills a consumer before acknowledgment and verifies redelivery to its replacement. It acknowledges three messages and inspects their log after both endpoints exit and the live queue is removed.

```sh
python3 test/restart_demo.py build/angaria_queue_demo --session build/restart-demo
./build/angaria_queue_demo inspect build/restart-demo/session.ang
```

Use a new session directory each time. Omitting `--session` runs the same scenario in a temporary directory and cleans it up afterward. CTest runs it as `restart_demo`, and Linux CI runs it with the rest of the suite from a fresh checkout.

The demo uses one producer, one consumer, one integer message schema, and one delivery in flight. The log provides recovery from process termination on a running system. Power-loss durability requires a separate contract.

## Run the queue demo

Create a session and send a `Reading` with a signed 32-bit value from the producer terminal.

```sh
./build/angaria_queue_demo create demo
./build/angaria_queue_demo send demo 42
```

In another terminal in the same checkout, receive the message. Type `ack` and press Enter after processing it to acknowledge delivery. Leaving the process before acknowledging keeps the message available for a replacement consumer.

```sh
./build/angaria_queue_demo receive demo 1000
# Reading sequence=1 value=42
ack
```

The optional timeout is in milliseconds. The default is an immediate check. For a script that processes and acknowledges one message, pipe the acknowledgment into the command.

```sh
printf 'ack\n' | ./build/angaria_queue_demo receive demo
```

To keep the producer attached while restarting consumers, run the interactive producer. Enter one signed integer per line and use Ctrl-D to detach.

```sh
./build/angaria_queue_demo produce demo
```

Inspect the log after both endpoints exit. The inspector reads the recorded schema and shows the queue, sequence, timestamp, type, and value.

```sh
./build/angaria_queue_demo inspect demo/session.ang
```

Remove the live queue when you are done. This preserves the log for inspection or archiving.

```sh
./build/angaria_queue_demo remove demo
```

The FIFO holds two messages. The session log defaults to 1 MiB. Pass a byte count after `create demo` to set a different cap. A send exits with status 3 if the queue is full or status 4 if the log is full. Acknowledgment frees a queue slot. Each role has one owner, and a competing attachment exits with status 5. Each successful attachment advances that role's incarnation number. See the [session format](docs/session-format.md) for the file layout and process-crash behavior.

## C++ endpoints

Keep a `Producer` and `Consumer` object alive for as long as each role is attached. Each object belongs to the process that created it and is used by one thread. Only one delivery can be in flight.

```cpp
#include "angaria/session.h"

#include <chrono>

void process_reading(const std::filesystem::path& session)
{
    angaria::Consumer consumer(session);
    const auto delivery = consumer.receive_reading(std::chrono::seconds{1});
    if (delivery)
    {
        // Apply application work before acknowledging.
        consumer.acknowledge(delivery->_sequence);
    }
}
```

Repeated receive calls return the current delivery until it is acknowledged. Closing or losing the consumer before acknowledgment lets its replacement receive the same message. An external side effect can be repeated if a consumer completes it and then dies before acknowledgment, so handlers must tolerate duplicates.
