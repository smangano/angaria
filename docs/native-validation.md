# Core delivery history validation

Milestone 4, step 4.3, framework version **0.4.3**, validates the core native delivery reader profile. A [Publication](native-transport.md#storage-and-acceptance) retains one message and its destination; its receipt records delivery progress. A Source pin identifies the one delivery currently issued through that Source.

The public Session factories now create format **2.2/`0x7`** for [direct publication](direct-publication.md) in step 4.6. Step 4.4 adds private [committed payload mappings and append planning](native-storage.md#committed-payload-storage-and-append-planning); step 4.5 adds [reserved progress and claim cleanup](native-progress.md). Step 4.7 now exposes [public Source delivery](native-receive.md) with bound payload views and exact progress tokens. Private tests construct **2.2/`0x7`** records through the journal to qualify malformed histories and reader boundaries independently of the public sender. Their offline profile edits are deliberate fixtures, not supported upgrades or application setup.

## Admission before recovery

Readers retain the 2.0 journal and 2.1 basic-Field profiles and recognize core 2.2 with its required native-delivery bit. Known minor/feature combinations must match the [capability table](native-transport.md#native-compatibility-and-required-capabilities). Unknown required bits, unsupported minors, foreign native ABI/protocol profiles, and allowed-loss requirements reject attachment before journal recovery. Structural metadata can recognize 2.2/`0xf`; native attachment still rejects its loss capability until step 4.8 implements loss replay and cleanup.

The admission tests leave real Prepared and Committed journal operations pending, then try incompatible headers. Rejection preserves the Session and coordination files byte for byte. A supported-profile control proves that recovery would otherwise modify those fixtures. This tests process-recovery ordering, not power-loss durability.

## History and current state

Session attachment, topology reads, and current/historical Registry snapshots validate delivery history after journal recovery while holding the mutation gate. Publication inspection reads a fixed header, destination metadata, and alignment padding; it retains the payload's checked offset/length without reading or copying application bytes. Its I/O and temporary metadata storage do not grow with payload size. Immutable metadata uses ordinary reads; receipt and ring words are copied through the native atomic adapter. The validator never starts delivery or writes progress. In a quiescent recovered Session, validation succeeds or rejects without changing its bytes. Recovery of a previously interrupted operation can legitimately repair state first.

| Evidence | Required relationship |
| --- | --- |
| Acceptance batch | One Publication, a Publication transaction subtype, unchanged Registry revision, and matching producer Task/incarnation in its final manifest. Registry transactions cannot hide Publications; retry transactions remain unsupported. |
| Publication identity | A registered producer, positive issued sequence, unique Task/incarnation/sequence tuple, and no backward producer incarnation in append order. The current incarnation is bounded by its current issued-ID counter. Earlier incarnations retain their accepted records without using that newer counter. |
| Schema and topology | The exact immutable registered schema matches type/version and existed by acceptance. The effective Queue and Source were Active, ownership matches the producer, and the original topology revision names the Queue definition effective at acceptance. Later documentation revisions do not alter that context. |
| Queue history | Accepted Queue sequences start at 1 and are contiguous. Each predecessor is exactly the preceding destination row. Latest link, last producer incarnation, and retained-history floor agree with this complete history. |
| Receipt and Source | Every accepted destination has a valid Core receipt. Captured consumer tags name its Source owner and a retained incarnation/issuance. An Issued receipt has exactly one matching current Source pin, and an idle Source cannot retain a pin. |
| Queue progress | Acknowledgments form a contiguous prefix. Any nonterminal receipt with recorded dequeue history, including released Pending, must be the oldest nonterminal delivery. The terminal watermark, oldest nonterminal link, and last acknowledgment timestamp agree with receipts. |
| Ring | Exactly the latest window is cached, with full-width generations and exact Queue destination/Publication references. Overwritten entries do not remove retained history. Slot and allocation padding remain canonical. |

Publication IDs may be accepted in a different order from their issuance: a producer can issue IDs 1 and 2, then send 2 before 1. Queue sequences describe acceptance order; publication sequences distinguish requests. Validation enforces uniqueness and issued bounds without confusing those two orders.

The core profile supports direct acceptance only. A native Publishing acceptance or Retry requires later capability work, even though the private record codecs can represent broader snapshots. Multiple Active Queues in one Source remain valid Registry topology; fresh Source selection currently supports at most one Active Queue in step 4.7; an existing pin remains usable during membership growth.

## Core progress words

The [fixed runtime layouts](mapped-format.md#preallocated-live-state) retain their existing sizes. Under the core delivery capability their previously reserved progress fields have these precise meanings:

- A Queue with no accepted messages has zero progress except its ring reference/capacity. With messages, its retained-history floor is sequence 1 and the first destination link. No history is retired; Task retirement floors remain zero.
- Queue flags at row offset 88 use bit 0 for presence of the last acknowledgment timestamp at offset 80. Other bits are zero. The value is the timestamp of the last acknowledgment in Queue order, which may be zero, negative, or earlier by wall clock than another acknowledgment.
- Source flags at row offset 18 use bit 0 for InFlight. A pinned slot contains the exact Queue, sequence, Publication, receipt, dequeue time, and delivery link matching its Issued receipt. An idle Source has zero slot fields at offsets 16–63.
- Source offsets 0 and 8 retain its issuance counter and consumer incarnation. Close/cleanup must preserve the last issuance context. A newer incarnation can reset its counter; receipts from older incarnations are not bounded by that newer counter. A retained receipt cannot name a later incarnation than the Source context, and same-incarnation receipts cannot exceed its issuance counter.

Strict/Spin scheduling, notification fields, and remaining reserved words stay zero. The presence flag makes epoch-zero acknowledgment time valid; UTC values are never used to infer operation order. Receipt presence follows its [own bit definitions](native-transport.md#receipt-fields-and-presence).

An Issued receipt can remain valid after its process dies: recorded claim identity and a persisted pin do not prove a live OS owner. An Issued Thread Task must also match its recorded parent Process incarnation, attachment flag, and PID; historical released/acknowledged receipts can outlive that parent claim. Step 4.5's replacement cleanup releases the exact old issuance and permits Task attachment to valid core publication-bearing state. The reader itself still performs no progress mutation.

## Tests and next steps

The [delivery validation tests](../test/test_delivery_validation.cpp) start with real Registry definitions, then install private journal records. They cover ring wrap, old incarnations, accepted-ID order, historical snapshots, pins, released-receipt ordering, acknowledgment clocks, and deliberately inconsistent references/roots. A private read observer rejects any metadata request overlapping a large payload before allocation/I/O, while separate corruption fixtures still check its alignment padding. The [capability tests](../test/test_delivery_compatibility.cpp) qualify rejection before recovery. The [test reading guide](../test/README.md) separates these fixtures from runnable public API examples.

Step 4.4 implements private mapped payload storage and complete append planning. Step 4.5 extends progress replay and Task/Source cleanup. Public direct sending and basic Source delivery are implemented in steps 4.6–4.7; allowed loss remains step 4.8. Application Binary payload admission belongs to the sending step; these history checks establish stored identity, reference, and progress relationships.
