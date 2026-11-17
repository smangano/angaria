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

Inspect the log after both endpoints exit. The inspector reads the recorded schema and shows the queue, sequence, timestamp, type, and value.

```sh
./build/angaria_queue_demo inspect demo/session.ang
```

Remove the live queue when you are done. This preserves the log for inspection or archiving.

```sh
./build/angaria_queue_demo remove demo
```

The FIFO holds two messages. The session log defaults to 1 MiB. Pass a byte count after `create demo` to set a different cap. A send exits with status 3 if the queue is full or status 4 if the log is full. A successful receive frees a queue slot and leaves the log intact. See the [session format](docs/session-format.md) for the file layout and process-crash behavior.
