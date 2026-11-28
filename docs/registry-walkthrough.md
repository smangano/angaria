# A Registry shared by independent programs

This walkthrough completes milestone 2's public Registry example. It uses the [eighth primer program](../examples/primer/08_registry.cpp), the [primer TOML file](../examples/primer/configuration.toml), and an [automated walkthrough](../test/registry_walkthrough.py). Start with the [programmer primer](primer.md) if Sessions, Tasks, or Queue Sets are new to you.

Several processes will add definitions to the same Session. Another process will inspect one consistent Registry revision, including earlier definitions after the topology changes. A producer process will hold its Task claim, exit, and be replaced. These operations configure and inspect communication; this milestone does not send messages.

## Run the complete check

Build from a checkout containing step 2.11:

```sh
cmake -S . -B build
cmake --build build --parallel
```

Run the automated example in a new temporary Session:

```sh
python3 test/registry_walkthrough.py build/angaria_primer_08_registry examples/primer/configuration.toml
```

The script launches independent executables, waits for explicit readiness messages, releases both registrars, and checks the Registry they leave behind. It also checks ownership contention, replaces a terminated owner, and compares current and historical routing. Its temporary files are removed when it finishes.

To keep the Session for your own inspection, choose a new child directory:

```sh
registry_workspace=$(mktemp -d "${TMPDIR:-/tmp}/angaria-registry.XXXXXX")
python3 test/registry_walkthrough.py build/angaria_primer_08_registry examples/primer/configuration.toml --session "$registry_workspace/session"
./build/angaria_primer_08_registry inspect "$registry_workspace/session"
```

The script refuses an existing Session path and preserves a retained Session after success or failure. It copies the supplied TOML into its own working directory and changes only the new Session location. The original configuration file is unchanged.

CTest runs this check as `registry_walkthrough`. It complements the native tests that interrupt actual journal writes; the public example itself has no private fault-injection hooks.

## Follow the individual processes

Use a different workspace from the automated example so the initial Session does not already exist:

```sh
registry_workspace=$(mktemp -d "${TMPDIR:-/tmp}/angaria-registry-manual.XXXXXX")
cp examples/primer/configuration.toml "$registry_workspace/registry.toml"
./build/angaria_primer_08_registry init "$registry_workspace/registry.toml"
session_directory="$registry_workspace/primer-session"
printf '%s\n' "$session_directory"
./build/angaria_primer_08_registry inspect "$session_directory"
```

`init` creates the TOML-defined connection and registers two offline configuration roles, `RegistrarA` and `RegistrarB`. The initial catalog has five Tasks, one Queue, one Source Queue Set, one Publishing Queue Set, two Fields, and one message schema. It keeps the TOML file's small capacity limits.

The inspector's first line begins with `SNAPSHOT revision=... current=...`. The following lines enumerate the Tasks, Fields, message-schema versions, Source memberships, Queues, and Publishing destinations. IDs identify relationships within this Session; their numeric values are not application constants.

Copy the printed absolute Session path into two additional terminals. Run commands from the repository root, setting the same variable in each terminal:

```sh
session_directory=/paste/the/printed/path/here
```

In terminal A:

```sh
./build/angaria_primer_08_registry register "$session_directory" A --wait
```

In terminal B:

```sh
./build/angaria_primer_08_registry register "$session_directory" B --wait
```

Each prints `READY registrar=A` or `READY registrar=B`. Both have attached to their own logical Task. Press Enter in each terminal to let both programs continue.

Each registrar adds its own Field, message schema, and Queue. Both new Queues belong to `Feed` and join `ConsumerInputs`; configuration Tasks can prepare connections for offline owners. Queue registration updates the Queue and the Source's inverse membership in one committed revision. Each new Queue is then activated.

Both processes also register the same `Shared` Task and the same `Quote` schema version 2. Exact registrations return the same existing identities. Finally, they propose different documentation for one `Contested` Task. One definition succeeds and the other receives `Conflict`; scheduling determines which registrar wins. A conflict never silently edits the winning definition.

After both exit, inspect again:

```sh
./build/angaria_primer_08_registry inspect "$session_directory"
```

The catalog now has seven Tasks, three Queues, one Source, one Publishing set, four Fields, and four message-schema versions. `Quote` versions 1 and 2 are both present. `ConsumerInputs` contains all three Queues. Running either registrar again leaves the Registry revision unchanged because its successful registrations and activations are exact retries.

## Inspect a consistent revision

The core API call is:

```cpp
auto registry = session.registry();
auto snapshot_result = registry.snapshot();
if (const auto* error = std::get_if<angaria::Error>(&snapshot_result))
{
  std::cerr << error->_path << ": " << error->_message << '\n';
  return 1;
}
auto snapshot = std::get<angaria::RegistrySnapshot>(std::move(snapshot_result));
```

A **snapshot** is an owning copy of the complete Registry at one committed revision. It contains ordinary C++ strings and vectors. Later updates do not change an earlier snapshot object. One snapshot is captured under the Session's mutation gate, so its Queue assignments and Source member lists describe the same committed state.

Its `_revision` names the captured definitions. `_currentRevision` gives the current committed Registry revision observed by that call. Without an argument, they are equal. An explicit revision asks for earlier retained definitions:

```cpp
auto earlier_result = registry.snapshot(saved_revision);
if (const auto* error = std::get_if<angaria::Error>(&earlier_result))
{
  std::cerr << error->_path << ": " << error->_message << '\n';
  return 1;
}
auto earlier = std::get<angaria::RegistrySnapshot>(std::move(earlier_result));
```

The historical copy includes the last committed state of every entity that existed at that revision. An atomic update is visible as a whole. Entities registered later are absent. Schema versions remain separate entries. Snapshot lists use numeric identity order, with message schemas ordered by type and version; Source and Publishing membership lists preserve their recorded order.

Revision zero is invalid. A revision beyond the current committed revision returns `NotFound`. Task attachment, PID information, and restart incarnations change independently of Registry revisions, so snapshots do not claim to reconstruct those past runtime values. The program labels its separate live status query `CURRENT` and omits that query when printing a historical snapshot.

The Session's Registry view rejects a mutation with `ReadOnlyRegistry`. The example checks that result and prints `READ_ONLY ReadOnlyRegistry`. Session attachment can still validate and recover the stored state; this read-only view is not a separate read-only file-open API.

## Replace an owner

In terminal A, open `Feed`'s writer and Publishing set:

```sh
./build/angaria_primer_08_registry owner "$session_directory" --hold
```

On a fresh walkthrough this prints `READY owner incarnation=1`. The Task variable keeps its claim alive while the program waits for Enter. This metadata lesson opens no messaging endpoint, so it does not seal the catalog before the later routing changes. The configured multiple-input Source and Publishing set describe later runtime features.

In terminal C, attempt another owner:

```sh
./build/angaria_primer_08_registry owner "$session_directory"
```

It fails with `OwnershipBusy`. Press Ctrl+C in terminal A to end that program, then retry terminal C's command. The replacement can attach, prints the next incarnation, and exits. The registered Task identity stays the same. Ending a process releases its native claim lock; the stored PID is diagnostic information.

The automated version terminates and reaps only a child process that it created, then verifies replacement. Native fault tests separately qualify interrupted Registry writes and repeated recovery.

## Compare earlier and current routing

Inspect the Registry and save the `revision` number in its first line. Then change the Publishing set through its owner:

```sh
./build/angaria_primer_08_registry inspect "$session_directory"
./build/angaria_primer_08_registry route "$session_directory"
./build/angaria_primer_08_registry inspect "$session_directory"
```

`route` attaches `Feed` and changes `FeedUpdates` from its original destination to `ExtraA` and `ExtraB`, in that order. The current snapshot shows two destinations and topology generation 2. It leaves the original Queue and all Source memberships intact.

To inspect the state before this change, replace `REVISION` below with the saved integer:

```sh
./build/angaria_primer_08_registry inspect "$session_directory" REVISION
```

That historical snapshot still shows the earlier Publishing destination. Its `_currentRevision` reports that newer definitions exist. You can also inspect the revision printed at initialization to see the Registry before the two registrars added their definitions.

After all application processes exit, the inspector still discovers the saved Registry. In a healthy, settled Session these inspections leave the log unchanged. If an earlier writer was interrupted, ordinary Session attachment may first complete recovery.

Milestone 2 qualifies Registry registration, inspection, ownership, and recovery on the tested native profiles. Milestone 3 adds [message encoding](message-payload.md), [canonical records](message-record.md), and [capture exchange with generic inspection](process-exchange.md). Native message delivery, Queue Set scheduling, waiting alternatives, and general replay tools follow their own roadmap milestones.
