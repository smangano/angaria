# Angaria

Angaria is a C++ inter-process messaging framework in development. The first demo exchanges a typed message through a named, bounded queue. See the [roadmap](ROADMAP.md) and [original design wiki](https://github.com/smangano/angaria/wiki/Angaria).

## Build and test

On Ubuntu 24.04, install a C++20 compiler, CMake, Boost headers, and GoogleTest.

```sh
sudo apt-get install build-essential cmake libboost-dev libgtest-dev
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

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
