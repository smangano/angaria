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

In another terminal in the same checkout, receive the message.

```sh
./build/angaria_queue_demo receive demo
# Reading value=42
```

Remove the session when both endpoints are done.

```sh
./build/angaria_queue_demo remove demo
```

The demo uses a Boost.Interprocess queue with room for two messages. A full queue rejects a send, and an empty queue rejects a receive. A successful receive removes the message. The [roadmap](ROADMAP.md) describes the session log, inspector, restart, and acknowledgment milestones.
