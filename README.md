# Angaria

> **Development status:** Angaria is under active development and is incomplete. Its design, APIs, configuration, and persisted data formats may change incompatibly before the stable **1.0.0** release.

<p align="center">
  <a href="assets/logos/README.md"><img src="assets/logos/A1_original_web.png" alt="Angaria — C++ messaging library" width="360"></a>
</p>

Angaria is a C++ inter-process messaging framework in development around a shared persistent Registry, Source Queue Sets, Publishing Queue Sets, and selectable policies. The [roadmap](ROADMAP.md) records the agreed architecture and ten milestones based on the [original design wiki](https://github.com/smangano/angaria/wiki/Angaria). Milestones 1, [2](docs/milestones/milestone-2.md), and [3](docs/milestones/milestone-3.md) are complete. Milestone 3 adds general message construction, capture, and inspection through exact Registry definitions. [Milestone 4](docs/milestones/milestone-4.md) starts the native Queue transport contract review.

The framework version is recorded in [VERSION](VERSION), which CMake reads and copies into the build directory. Development versions use `0.<milestone>.<build>`, with a new build number for every PR and, from milestone 3, build numbering starting at `1` for each milestone. Completing the roadmap produces `1.0.0`; see the [versioning policy](ROADMAP.md#versioning).

The milestone-6 demo target is a [five-process trading simulation](ROADMAP.md#demonstration-application): market-data feed, strategy, risk gateway, exchange simulator, and audit consumer. Planning starts during milestones 2–3, with runnable portions introduced as mapped storage and Queue Sets become available.

The current v0 prototype exchanges an integer message through a bounded filesystem queue, supports independent endpoint restart and explicit acknowledgment, and records messages in an inspectable session file. The commands below run that prototype.

## Build and test

New to Angaria? Start with the [programmer primer](docs/primer.md). It introduces the core concepts in simple language and walks through thirteen runnable programs, from creating a Session to configuring fan-out, inspecting Registry history, and exchanging captured records across processes and producer replacements. It grows with each milestone.

On Ubuntu 24.04, install a C++20 compiler, CMake, Boost headers, GoogleTest, and Python 3.9 or later for the automated walkthrough.

```sh
sudo apt-get install build-essential cmake libboost-dev libgtest-dev python3
git clone https://github.com/smangano/angaria.git
cd angaria
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For usage examples, start with the [test reading guide](test/README.md). Test comments explain the demonstrated scenarios and distinguish public Session usage from private fixtures, recovery checks, the protocol model, and the earlier prototype.

On macOS, install the test dependencies with Homebrew and use its prefix when configuring:

```sh
brew install cmake boost googletest python
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure
```

Hosted CI automatically exercises Linux x86_64, Linux ARM64, and macOS ARM64. The slower macOS x86_64 qualification runs only when requested through its manual workflow. The [native qualification contract](docs/native-storage.md#registry-qualification-and-native-profiles) defines the coverage and [manual-run instructions](docs/native-storage.md#request-macos-x86_64-qualification).

## Configuration contract

The [configuration and lifecycle contract](docs/configuration.md) defines the implemented TOML schema. `Session::create_from_file` validates configuration and creates the initial Registry; `Session::attach_from_file` validates settings against an existing Session without applying registrations or claiming Tasks. The [trading example](examples/trading.toml) describes process and thread Tasks, both Queue Set types, scalar Fields, and an immutable message schema using the supported native policy profile. Start with the [primer's TOML lesson](docs/primer.md#7-move-setup-into-a-toml-file) for a smaller runnable example.

The loader uses pinned cpptoml revision `fededad7169e538ca47e11a9ee9251bc361a9a65`, targeting TOML v0.5.0 syntax. CMake fetches those headers for library builds, including builds with testing disabled. A local checkout of that revision can be supplied for an offline build.

```sh
cmake -S . -B build -DFETCHCONTENT_SOURCE_DIR_CPPTOML=/absolute/path/to/cpptoml
```

Relative Session directories resolve from the TOML file's parent directory. Creation refuses to replace an existing Session and publishes its directory only after setup succeeds. Delivery policies whose runtime behavior is not implemented still return `UnsupportedPolicy`.

## Public API contract

The [public C++ API contract](docs/api.md) describes the identities, descriptors, results, and endpoint interfaces under milestone 1. The [trading API example](examples/trading_api.cpp) is compiled with each build as `angaria_api_contract`. Runtime definitions follow the Registry, message, and Queue Set milestones.

The [mapped layout and compatibility contract](docs/mapped-format.md) defines retained format 2.0, creation format 2.1, the core 2.2 reader profile, capacity accounting, and migration rules. Public Session creation/attachment, registration and inspection of every Registry entity kind, topology updates and activation, Task claims, endpoint identity/ownership handles, and publication identity issuance are implemented. Message delivery and Queue Set scheduling follow their [roadmap milestones](ROADMAP.md).

The [concurrency and recovery contract](docs/concurrency.md) specifies the blocking mutation gate, Task claims, journal commit/replay, full-log delivery progress, and notification protocol. CTest runs both the bounded reference model and native storage/coordination/journal qualification tests. Publication, delivery, and notification behavior follow in their implementation milestones.

Step 2.1 implements the format-2 backend foundation: a shared checked section planner and a portable [superblock codec](src/mapped_superblock.h), with byte-fixture, malformed-input, and compatibility tests. The [superblock](docs/mapped-format.md#superblock) is the fixed 4,096-byte session control area at the beginning of `session.ang`. Its codec encodes a provisional superblock prefix and validates immutable metadata; it does not create a usable Session or perform native file/mapping/journal recovery. Native resources, journal recovery, and Session integration are separate increments in the [milestone 2 steps](docs/milestones/milestone-2.md). Related terms are defined in the [glossary](docs/glossary.md).

Step 2.2 adds private [native file and mapping owners](docs/native-storage.md): checked positional I/O, read-only/shared writable mappings, and bounded resize with process-local protection for live views across aliases. A mapping retains its backing file state. These storage primitives do not expose usable format-2 Sessions.

Step 2.3 adds the shared [session controller, mutation guards, Task leases, and native atomic adapter](docs/native-storage.md#shared-controller-and-mutation-gate). Native tests exercise thread/process exclusion, claim lifetimes and process termination, and mapped-memory publication ordering.

Step 2.4 adds the private [mutation journal](docs/native-storage.md#journal-transactions-and-recovery): inline fixed-state transactions, append batches with manifests, stale-plan rejection, and repeatable rollback/redo for two-plane storage. Tests terminate independent processes at real write/recovery boundaries and inject I/O failures.

Step 2.5 integrates [public Session creation and attachment](docs/native-storage.md#public-session-and-task-lifetimes), persisted options/metadata, the creator Process Task, Task inspection, and fixed-state attachment/incarnation/publication-ID operations. Creation publishes a completely initialized directory exclusively; attachment validates and recovers existing state. Claim replacement, including cleanup of a dead Process Task's thread Tasks, works with no append space.

Step 2.6 adds [Task registration](docs/native-storage.md#public-task-registration) through a live Task's Registry. Exact descriptors return the existing identity without writing; conflicting descriptors, exhausted capacities, and insufficient log space preserve existing registrations. Process and Thread Tasks can be registered before attachment, and each successful new registration commits one immutable definition and Registry revision. Tests cover concurrent registrars, independent discovery, and interrupted registration/recovery.

Step 2.7 adds [Field and immutable message-schema registration](docs/native-storage.md#fields-and-message-schemas). Field IDs retain their full 32-bit `0xEE` prefix, schema identities use an exact message type/version, and ordered Field lists preserve repetition without a 16-bit count limit. Earlier versions remain discoverable after a new version is registered. Step 3.1 adds scalar payload encoding below, and step 3.3 adds basic logical Fields. Step 2.10 integrates TOML loading below.

Step 2.8 adds [draft Queues and Queue Sets](docs/native-storage.md#draft-queues-and-queue-sets), shared typed Queue Set identities, ownership checks, and atomic Source membership registration. Independent-process tests exercise discovery, concurrent registration, and interrupted multi-record updates. Activation, ring allocation, and endpoints follow their roadmap steps.

Step 2.9 adds [adoption, topology changes, and activation](docs/native-storage.md#topology-changes-and-activation). Queue activation reserves a zeroed ring atomically with its Active definition. Endpoint handles expose identities and retain Task ownership; a Source opening is exclusive within that claim. Sending, receiving, acknowledgment, and publishing follow their implementation milestones.

Step 2.10 adds [TOML Session factories](docs/native-storage.md#toml-session-factories), complete semantic preflight, shared programmatic policy validation, initial registration/activation, and exact effective-setting checks on file-based attachment. The [primer](docs/primer.md#7-move-setup-into-a-toml-file) and automated examples demonstrate the public file API.

Step 2.11 adds [consistent current and historical Registry snapshots](docs/api.md#current-and-historical-registry-snapshots) and mixed independent-process qualification. The [Registry walkthrough](docs/registry-walkthrough.md) shows every entity kind, concurrent registration, ownership replacement, and inspection after application shutdown:

```sh
python3 test/registry_walkthrough.py build/angaria_primer_08_registry examples/primer/configuration.toml
```

The default run uses temporary storage. To retain a Session for manual inspection, pass `--session` with a new directory path whose parent already exists. It refuses to overwrite an existing path. This walkthrough demonstrates the Registry and endpoint identities. The ninth lesson adds payload encoding; delivery follows its roadmap milestones.

Step 3.1 adds the [binary scalar payload codec](docs/message-payload.md), with exact schema-version selection, explicit little-endian bytes, and owning codecs independent of Session/Task lifetime. The [ninth primer example](examples/primer/09_payload.cpp) encodes and decodes two Quote versions. Steps 3.4–3.5 add message records and capture inspection below.

Step 3.2 reviewed the [basic logical Field contract](docs/logical-fields.md). Step 3.3 implements booleans, 8/16/32/64-bit Fields, closed enumerations, fixed ASCII, and signed int8, including immutable Registry metadata and TOML version 2. New Sessions use format 2.1; existing 2.0 Sessions retain scalar behavior. The [tenth primer program](examples/primer/10_logical_fields.cpp) encodes logical values and demonstrates bounded caller-buffer construction with unchanged output on errors.

Step 3.4, framework version **`0.3.4`**, adds [canonical message records](docs/message-record.md): an 80-byte portable header plus payload, with complete publication identity, exact schema, initial publication time, and flags. The [eleventh primer program](examples/primer/11_message_record.cpp) demonstrates bounded framing, owning/borrowed decoding, and exact Session/schema checks. Native Session formats remain unchanged; Queue publication and delivery follow later milestones.

Step 3.5, framework version **`0.3.5`**, adds [bounded capture streams and generic inspection](docs/capture-inspection.md). The inspector uses retained Registry definitions to render exact scalar/logical schema versions without application-specific C++ types. The [twelfth primer program](examples/primer/12_capture.cpp) writes Quote versions, an Order, and an empty Heartbeat, then exits before the separate inspector runs:

```sh
./build/angaria_primer_12_capture build/capture-example
./build/angaria_inspect build/capture-example/session build/capture-example/capture.angcap
```

Use a new workspace directory. Keep its native Session catalog alongside the capture; attaching that catalog requires a supported native profile and can perform recovery. The CLI closes native handles before reading the capture. Record, capture, catalog, per-record text, and aggregate output limits bound inspection; ordinary capture I/O does not establish Queue acceptance or delivery.

Step 3.6, framework version **`0.3.6`**, adds [independent-process exchange qualification](docs/process-exchange.md) and the [thirteenth primer program](examples/primer/13_process_exchange.cpp). It keeps an old catalog alive across a same-sized schema addition, replaces a terminated producer after its capture is complete, and inspects every exact version after applications exit:

```sh
python3 test/process_exchange.py build/angaria_primer_13_process_exchange build/angaria_inspect
```

Pass `--workspace` with a new directory to keep the captures and native catalog for manual inspection. Framework version **`0.3.7`** integrates the completed milestone-3 staging branch into `main`. The [qualification contract](docs/process-exchange.md#compatibility-and-qualification) records native compatibility checks and the boundary between these ordinary files and future Queue delivery.

Milestone 4, step 4.3, framework version **`0.4.3`**, adds [Session and delivery-history validation](docs/native-validation.md) after the checked record codecs. Core format 2.2 readers validate exact schemas, acceptance transactions, Queue history, receipts, Source pins, and ring windows; incompatible capabilities fail before recovery. New Sessions still use format 2.1, and public native send/receive remains planned. The thirteen existing primer programs remain the runnable learning path.

Framework **`0.4.4`** consolidates repeated helpers after step 4.3. The [helper audit and header map](docs/helper-audit.md) records what is shared and why some operation-specific adapters remain. This cleanup adds no public delivery behavior or format changes.

Step **4.4**, framework **`0.4.5`**, adds [private committed payload mappings and complete append planning](docs/native-storage.md#committed-payload-storage-and-append-planning). Payload addresses remain stable across append growth and rollback of later uncommitted work. Full batch sizing includes receipts, the manifest, redo, and alignment before journal image construction. Public delivery and delivery-enabled Session creation remain with later steps.

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
