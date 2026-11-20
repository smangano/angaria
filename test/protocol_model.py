#!/usr/bin/env python3
"""Bounded reference-model checks for the proposed format-2 mutation protocol.

This models serialized logical operations and crash cuts, not native mmap,
atomic instructions, filesystem durability, flock, or eventfd qualification.
One arena unit represents one append allocation, not a byte-size calculation.
The production implementation must pass separate OS/process-crash tests.
Tuple materialization abstracts record readiness bits and packed atomic stores;
Ready versus already-Committed record flags are not separate model states.
"""

import copy
import itertools
import sys
import unittest
from dataclasses import dataclass
from enum import IntEnum


class Control(IntEnum):
    IDLE = 0
    PREPARED = 1
    COMMITTED = 2
    APPLIED = 3


class KnownRejection(Exception):
    pass


class RecoveryRequired(Exception):
    pass


class CorruptJournal(Exception):
    pass


@dataclass
class Intent:
    label: str
    base_tail: int
    base_state: dict
    records: tuple
    patches: tuple


def assign(state, path, value):
    target = state
    for component in path[:-1]:
        target = target[component]
    target[path[-1]] = copy.deepcopy(value)


class Model:
    """The gate excludes observers until recovery has completed.

    Redo patches use absolute values. The same committed intent can therefore
    be replayed after any number of process failures without incrementing a
    counter twice or creating another delivery. Two queues use either separate
    Source sets or one shared Source; selection-policy fairness is outside the
    model. Fixed-state dictionaries abstract compact redo commands such as
    loss-range progress and claim cleanup, not an allocation of redo records.
    """

    def __init__(self, log_capacity=8, allow_loss=False, shared_source=False):
        self.log_capacity = log_capacity
        self.arena = []
        self.control = Control.IDLE
        self.journal = None
        self.state = {
            "producer": {"incarnation": 1, "issued": 0},
            "consumer_incarnation": 1,
            "queues": {
                queue: {"last": 0, "history": [], "ring": [], "terminal": 0}
                for queue in (1, 2)
            },
            "issuance": {1: 0, 2: 0},
            "inflight": {1: None, 2: None},
            "source_open": {1: True, 2: True},
            "source_for_queue": {1: 1, 2: 1 if shared_source else 2},
            "publications": {},
            "receipts": {},
            "log_tail": 0,
            "allow_loss": allow_loss,
        }

    def require_clean(self):
        if self.control != Control.IDLE:
            raise RecoveryRequired("the gate must recover before exposing mutable state")

    def plan(self, label, desired, records=()):
        self.require_clean()
        patches = []
        for section in self.state:
            old, new = self.state[section], desired[section]
            if isinstance(old, dict):
                for key in new:
                    if key not in old or old[key] != new[key]:
                        patches.append(((section, key), copy.deepcopy(new[key])))
            elif old != new:
                patches.append(((section,), copy.deepcopy(new)))
        return Intent(label, len(self.arena), copy.deepcopy(self.state),
                      tuple(copy.deepcopy(records)), tuple(patches))

    def execute(self, intent):
        self.require_clean()
        if intent is None:
            return
        if intent.base_tail != len(self.arena):
            raise KnownRejection("stale plan")
        # This body is ignored if the writer dies before PREPARED is published.
        self.journal = copy.deepcopy(intent)
        yield "journal_body"
        self.control = Control.PREPARED
        yield "prepared"
        for index, record in enumerate(intent.records):
            self.arena.append(("incomplete_record", index))
            yield f"materialize_begin_{index}"
            self.arena[-1] = copy.deepcopy(record)
            yield f"materialize_{index}"
        self.control = Control.COMMITTED
        yield "committed"
        for index, (path, value) in enumerate(intent.patches):
            assign(self.state, path, value)
            yield f"apply_{index}"
        self.control = Control.APPLIED
        yield "applied"
        self.control = Control.IDLE
        yield "idle"

    def recover(self):
        if self.control == Control.IDLE:
            return
        if self.control not in (Control.PREPARED, Control.COMMITTED, Control.APPLIED):
            raise CorruptJournal("unknown control value")
        if self.journal is None or len(self.arena) < self.journal.base_tail:
            raise CorruptJournal("invalid journal tail")
        if self.control == Control.PREPARED:
            if self.state != self.journal.base_state:
                raise CorruptJournal("Prepared canonical state differs from base plan")
            if self.journal.records:
                del self.arena[self.journal.base_tail :]
                yield "abort_tail"
            elif len(self.arena) != self.journal.base_tail:
                raise CorruptJournal("FixedState unexpectedly changed append resources")
        else:
            expected = list(self.journal.records)
            if self.arena[self.journal.base_tail :] != expected:
                raise CorruptJournal("committed append was not completely materialized")
            for index, (path, value) in enumerate(self.journal.patches):
                assign(self.state, path, value)
                yield f"redo_{index}"
            self.control = Control.APPLIED
            yield "recovered_applied"
        self.control = Control.IDLE
        yield "recovered_idle"

    def finish(self, intent):
        list(self.execute(intent))

    def issue_id(self):
        self.require_clean()
        desired = copy.deepcopy(self.state)
        desired["producer"]["issued"] += 1
        return self.plan("issue_id", desired)

    def next_id(self):
        producer = self.state["producer"]
        return producer["incarnation"], producer["issued"]

    def publish(self, identity, destinations=(1, 2), unavailable=(), partial=False,
                payload="message", output_capacity=2, endpoint="updates"):
        self.require_clean()
        if identity[0] != self.state["producer"]["incarnation"]:
            raise KnownRejection("old incarnations cannot publish new messages")
        original = self.state["publications"].get(identity)
        required = len(original["snapshot"]) if original is not None else len(destinations)
        if output_capacity < required:
            raise KnownRejection("OutputBufferTooSmall")
        if original is not None:
            # destinations describes the endpoint's CURRENT topology. Repeating
            # the original endpoint/input resolves its original snapshot.
            if original["payload"] != payload or original["endpoint"] != endpoint:
                raise KnownRejection("Conflict")
            return None
        if not 0 < identity[1] <= self.state["producer"]["issued"]:
            raise KnownRejection("unissued identity")
        accepted = tuple(queue for queue in destinations if queue not in unavailable)
        # Unavailability models destination-specific resource/counter exhaustion,
        # never an offline consumer or a full overwriteable ring.
        if not accepted or (not partial and len(accepted) != len(destinations)):
            raise KnownRejection("destination cannot accept")
        if len(self.arena) >= self.log_capacity:
            raise KnownRejection("LogFull")
        desired = copy.deepcopy(self.state)
        desired["publications"][identity] = {
            "payload": payload, "snapshot": destinations, "accepted": {},
            "reserved_receipts": destinations, "endpoint": endpoint,
        }
        for queue in destinations:
            desired["receipts"][(identity, queue)] = {"state": "unused", "token": None}
        self.add_deliveries(desired, identity, accepted)
        desired["log_tail"] += 1
        record = ("publication", identity, payload, destinations, accepted)
        return self.plan("publish", desired, (record,))

    def retry(self, identity, requested=(1, 2), unavailable=()):
        self.require_clean()
        original = self.state["publications"].get(identity)
        if original is None:
            raise KnownRejection("HistoryUnavailable")
        if len(set(requested)) != len(requested) or not set(requested) <= set(original["snapshot"]):
            raise KnownRejection("invalid retry destinations")
        accepted = tuple(queue for queue in requested
                         if queue not in original["accepted"] and queue not in unavailable)
        if not accepted:
            return None
        if len(self.arena) >= self.log_capacity:
            raise KnownRejection("LogFull")
        desired = copy.deepcopy(self.state)
        self.add_deliveries(desired, identity, accepted)
        desired["log_tail"] += 1
        return self.plan("retry", desired, (("retry", identity, accepted),))

    @staticmethod
    def add_deliveries(desired, identity, accepted):
        for queue in accepted:
            slot = (identity, queue)
            if (slot not in desired["receipts"] or
                    desired["receipts"][slot]["state"] != "unused"):
                raise CorruptJournal("delivery lacks its original reserved receipt")
            state = desired["queues"][queue]
            state["last"] += 1
            sequence = state["last"]
            state["history"].append((sequence, identity))
            state["ring"] = (state["ring"] + [(sequence, identity)])[-2:]
            desired["publications"][identity]["accepted"][queue] = sequence
            desired["receipts"][slot] = {"state": "pending", "token": None}

    @staticmethod
    def receipt_key(state, queue, sequence):
        for candidate, identity in state["queues"][queue]["history"]:
            if candidate == sequence:
                return identity, queue
        raise CorruptJournal("delivery sequence has no retained history")

    def eligible_ring(self, queue):
        """Reject a stale/provisional cache entry without committed acceptance."""
        self.require_clean()
        return [
            (sequence, identity) for sequence, identity in self.state["queues"][queue]["ring"]
            if self.state["publications"].get(identity, {}).get("accepted", {}).get(queue) == sequence
        ]

    def receive(self, queue):
        self.require_clean()
        source = self.state["source_for_queue"][queue]
        if not self.state["source_open"][source]:
            raise KnownRejection("closed Source")
        if self.state["inflight"][source] is not None:
            return None
        gap = self.skip_overwritten(queue)
        if gap is not None:
            return gap
        available = [sequence for sequence, identity in self.state["queues"][queue]["history"]
                     if self.state["receipts"][(identity, queue)]["state"] == "pending"]
        if not available:
            return None
        desired = copy.deepcopy(self.state)
        desired["issuance"][source] += 1
        sequence = available[0]
        token = (desired["consumer_incarnation"], queue, sequence,
                 desired["issuance"][source], source)
        desired["inflight"][source] = token
        desired["receipts"][self.receipt_key(desired, queue, sequence)] = {
            "state": "issued", "token": token,
        }
        return self.plan("receive", desired)

    def finish_delivery(self, queue, token, acknowledge):
        self.require_clean()
        source = self.state["source_for_queue"][queue]
        if token is None or self.state["inflight"][source] != token or token[1] != queue:
            raise KnownRejection("StaleDelivery")
        desired = copy.deepcopy(self.state)
        desired["inflight"][source] = None
        desired["receipts"][self.receipt_key(desired, queue, token[2])] = {
            "state": "acknowledged" if acknowledge else "pending", "token": None,
        }
        if acknowledge:
            desired["queues"][queue]["terminal"] = token[2]
        return self.plan("acknowledge" if acknowledge else "release", desired)

    def replace_consumer(self):
        self.require_clean()
        desired = copy.deepcopy(self.state)
        desired["consumer_incarnation"] += 1
        for source, token in desired["inflight"].items():
            if token is not None:
                key = self.receipt_key(desired, token[1], token[2])
                desired["receipts"][key] = {"state": "pending", "token": None}
            desired["inflight"][source] = None
        return self.plan("replace_consumer", desired)

    def replace_producer(self):
        self.require_clean()
        desired = copy.deepcopy(self.state)
        desired["producer"]["incarnation"] += 1
        desired["producer"]["issued"] = 0
        return self.plan("replace_producer", desired)

    def close_source(self, source):
        self.require_clean()
        desired = copy.deepcopy(self.state)
        token = desired["inflight"][source]
        if token is not None:
            key = self.receipt_key(desired, token[1], token[2])
            desired["receipts"][key] = {"state": "pending", "token": None}
        desired["inflight"][source] = None
        desired["source_open"][source] = False
        return self.plan("close_source", desired)

    def open_source(self, source):
        self.require_clean()
        if self.state["source_open"][source]:
            raise KnownRejection("OwnershipBusy")
        desired = copy.deepcopy(self.state)
        desired["source_open"][source] = True
        return self.plan("open_source", desired)

    def skip_overwritten(self, queue):
        self.require_clean()
        source = self.state["source_for_queue"][queue]
        if not self.state["allow_loss"] or self.state["inflight"][source] is not None:
            return None
        state = self.state["queues"][queue]
        floor = state["ring"][0][0] if state["ring"] else state["last"] + 1
        lost = [sequence for sequence, identity in state["history"]
                if sequence < floor and self.state["receipts"][(identity, queue)]["state"] == "pending"]
        if not lost:
            return None
        desired = copy.deepcopy(self.state)
        for sequence in lost:
            desired["receipts"][self.receipt_key(desired, queue, sequence)] = {
                "state": "lost", "token": None,
            }
        desired["queues"][queue]["terminal"] = lost[-1]
        return self.plan("gap", desired)


class ProtocolModelTest(unittest.TestCase):
    scenarios = 0
    crash_cuts = 0
    recovery_cuts = 0
    notification_schedules = 0

    def assert_recovered(self, model, expected_state, expected_arena):
        list(model.recover())
        list(model.recover())
        self.assertEqual(model.control, Control.IDLE)
        self.assertEqual(model.state, expected_state)
        self.assertEqual(model.arena, expected_arena)

    def explore(self, base, intent):
        """Cut after every persistent step, then cut every recovery step too."""
        self.assertIsNotNone(intent)
        type(self).scenarios += 1
        completed = copy.deepcopy(base)
        phases = list(completed.execute(intent))
        commit_cut = phases.index("committed") + 1
        for cut in range(len(phases) + 1):
            with self.subTest(operation=intent.label, cut=cut):
                interrupted = copy.deepcopy(base)
                execution = interrupted.execute(intent)
                for _ in range(cut):
                    next(execution)
                committed = cut >= commit_cut
                expected = completed if committed else base
                if interrupted.control != Control.IDLE:
                    with self.assertRaises(RecoveryRequired):
                        interrupted.eligible_ring(1)
                recovered = copy.deepcopy(interrupted)
                recovery_phases = list(recovered.recover())
                for recovery_cut in range(len(recovery_phases) + 1):
                    again = copy.deepcopy(interrupted)
                    recovering = again.recover()
                    for _ in range(recovery_cut):
                        next(recovering)
                    self.assert_recovered(again, expected.state, expected.arena)
                    type(self).recovery_cuts += 1
                self.assert_recovered(interrupted, expected.state, expected.arena)
                type(self).crash_cuts += 1
        return completed

    @staticmethod
    def with_id(log_capacity=8, allow_loss=False, shared_source=False):
        model = Model(log_capacity, allow_loss, shared_source)
        model.finish(model.issue_id())
        return model

    def test_default_fanout_is_all_or_none_at_every_crash_cut(self):
        base = self.with_id()
        done = self.explore(base, base.publish(base.next_id()))
        identity = base.next_id()
        self.assertEqual(done.state["publications"][identity]["accepted"], {1: 1, 2: 1})
        self.assertEqual([done.state["queues"][q]["last"] for q in (1, 2)], [1, 1])

    def test_known_rejections_change_no_stored_state(self):
        for arguments in ({"unavailable": (2,)}, {"unavailable": (1, 2), "partial": True},
                          {"output_capacity": 1}):
            model = self.with_id()
            before = copy.deepcopy(model.__dict__)
            with self.assertRaises(KnownRejection):
                model.publish(model.next_id(), **arguments)
            self.assertEqual(model.__dict__, before)
        full = self.with_id(log_capacity=0)
        before = copy.deepcopy(full.__dict__)
        with self.assertRaisesRegex(KnownRejection, "LogFull"):
            full.publish(full.next_id())
        self.assertEqual(full.__dict__, before)

    def test_partial_retry_preserves_original_acceptance_without_duplicates(self):
        base = self.with_id()
        identity = base.next_id()
        partial = self.explore(base, base.publish(identity, unavailable=(2,), partial=True))
        self.assertEqual(partial.state["publications"][identity]["accepted"], {1: 1})
        self.assertEqual(partial.state["publications"][identity]["reserved_receipts"], (1, 2))
        self.assertEqual(partial.state["receipts"][(identity, 2)]["state"], "unused")
        self.assertIsNone(partial.publish(identity, destinations=(2,)))
        partial.finish(partial.receive(1))
        partial.finish(partial.finish_delivery(1, partial.state["inflight"][1], acknowledge=True))
        partial = self.explore(partial, partial.replace_producer())
        done = self.explore(partial, partial.retry(identity))
        self.assertEqual(done.state["publications"][identity]["accepted"], {1: 1, 2: 1})
        self.assertEqual(done.state["receipts"][(identity, 1)]["state"], "acknowledged")
        self.assertEqual(set(done.state["receipts"]), set(partial.state["receipts"]))
        before = copy.deepcopy(done.__dict__)
        self.assertIsNone(done.retry(identity))
        self.assertEqual(done.__dict__, before)
        with self.assertRaisesRegex(KnownRejection, "old incarnations"):
            done.publish(identity)
        self.assertEqual(len(done.arena), 2)
        self.assertEqual([len(done.state["queues"][q]["history"]) for q in (1, 2)], [1, 1])

    def test_full_log_still_allows_claim_issuance_receive_ack_and_release(self):
        base = self.with_id(log_capacity=1)
        base.finish(base.publish(base.next_id()))
        self.assertEqual(len(base.arena), base.log_capacity)
        self.explore(base, base.issue_id())
        self.explore(base, base.replace_producer())
        issued = self.explore(base, base.receive(1))
        token = issued.state["inflight"][1]
        self.explore(issued, issued.finish_delivery(1, token, acknowledge=True))
        released = self.explore(issued, issued.finish_delivery(1, token, acknowledge=False))
        replaced = self.explore(issued, issued.replace_consumer())
        replacement_delivery = self.explore(replaced, replaced.receive(1))
        self.assertNotEqual(token, replacement_delivery.state["inflight"][1])
        self.assertEqual(replacement_delivery.state["inflight"][1][2], token[2])
        reissued = self.explore(released, released.receive(1))
        self.assertNotEqual(token, reissued.state["inflight"][1])
        with self.assertRaisesRegex(KnownRejection, "StaleDelivery"):
            reissued.finish_delivery(1, token, acknowledge=True)

        closed = self.explore(issued, issued.close_source(1))
        reopened = self.explore(closed, closed.open_source(1))
        resumed = self.explore(reopened, reopened.receive(1))
        self.assertEqual(resumed.state["inflight"][1][:3], token[:3])
        self.assertGreater(resumed.state["inflight"][1][3], token[3])
        with self.assertRaisesRegex(KnownRejection, "StaleDelivery"):
            resumed.finish_delivery(1, token, acknowledge=True)

    def test_one_source_has_only_one_slot_across_multiple_member_queues(self):
        model = self.with_id(shared_source=True)
        model.finish(model.publish(model.next_id()))
        first = self.explore(model, model.receive(1))
        token = first.state["inflight"][1]
        self.assertIsNone(first.receive(2))
        self.assertEqual(first.state["inflight"][1], token)
        self.assertEqual(sum(value is not None for value in first.state["inflight"].values()), 1)
        first.finish(first.finish_delivery(1, token, acknowledge=True))
        second = self.explore(first, first.receive(2))
        self.assertEqual(second.state["inflight"][1][1:3], (2, 1))
        self.assertGreater(second.state["inflight"][1][3], token[3])

    def test_ring_overwrite_preserves_recovery_and_an_issued_loss_policy_pin(self):
        for allow_loss in (False, True):
            model = self.with_id(log_capacity=3, allow_loss=allow_loss)
            model.finish(model.publish(model.next_id()))
            if allow_loss:
                model.finish(model.receive(1))
                pinned = model.state["inflight"][1]
            for _ in range(2):
                model.finish(model.issue_id())
                model.finish(model.publish(model.next_id()))
            self.assertEqual([seq for seq, _ in model.eligible_ring(1)], [2, 3])
            if not allow_loss:
                recovered = self.explore(model, model.receive(1))
                self.assertEqual(recovered.state["inflight"][1][2], 1)
            else:
                self.assertIsNone(model.receive(1))
                self.assertEqual(model.state["inflight"][1], pinned)
                self.assertIsNone(model.skip_overwritten(1))
                model.finish(model.finish_delivery(1, pinned, acknowledge=False))
                gap = model.receive(1)
                self.assertEqual(gap.label, "gap")
                skipped = self.explore(model, gap)
                self.assertIsNone(skipped.state["inflight"][1])
                receipt = Model.receipt_key(skipped.state, 1, 1)
                self.assertEqual(skipped.state["receipts"][receipt]["state"], "lost")
                skipped.finish(skipped.receive(1))
                self.assertEqual(skipped.state["inflight"][1][2], 2)
                self.assertEqual(len(skipped.arena), 3)

    def test_a_ring_candidate_is_not_commit_authority(self):
        model = self.with_id()
        model.state["queues"][1]["ring"] = [(1, model.next_id())]
        self.assertEqual(model.eligible_ring(1), [])
        self.assertIsNone(model.receive(1))

    def test_mutations_serialize_after_recovering_an_interrupted_predecessor(self):
        for order in itertools.permutations(("first", "second")):
            base = self.with_id()
            base.finish(base.issue_id())
            initial = copy.deepcopy(base)
            intent = base.publish((1, 1), payload=order[0])
            probe = copy.deepcopy(base)
            phases = list(probe.execute(intent))
            for cut in range(len(phases) + 1):
                model = copy.deepcopy(initial)
                execution = model.execute(intent)
                for _ in range(cut):
                    next(execution)
                if model.control != Control.IDLE:
                    with self.assertRaises(RecoveryRequired):
                        model.publish((1, 2), payload=order[1])
                list(model.recover())
                before = len(model.state["queues"][1]["history"])
                model.finish(model.publish((1, 2), payload=order[1]))
                self.assertEqual(model.state["queues"][1]["last"], before + 1)
                self.assertEqual(model.state["queues"][2]["last"], before + 1)
                self.assertEqual(len(model.state["queues"][1]["history"]), before + 1)
                type(self).crash_cuts += 1

    def test_unknown_control_and_incomplete_committed_body_fail_closed(self):
        model = self.with_id()
        model.control = 99
        with self.assertRaises(CorruptJournal):
            list(model.recover())
        model = self.with_id()
        steps = model.execute(model.publish(model.next_id()))
        while next(steps) != "committed":
            pass
        model.arena.clear()
        with self.assertRaises(CorruptJournal):
            list(model.recover())

    def test_prepared_recovery_rejects_changed_canonical_roots(self):
        for append in (False, True):
            model = self.with_id()
            intent = model.publish(model.next_id()) if append else model.issue_id()
            steps = model.execute(intent)
            while next(steps) != "prepared":
                pass
            model.state["queues"][1]["last"] = 99
            evidence = copy.deepcopy(model.__dict__)
            with self.assertRaisesRegex(CorruptJournal, "canonical"):
                list(model.recover())
            self.assertEqual(model.__dict__, evidence)

    def test_prepared_fixed_state_never_truncates_unexpected_resources(self):
        model = self.with_id()
        steps = model.execute(model.issue_id())
        while next(steps) != "prepared":
            pass
        model.arena.append(("unexpected_resource",))
        evidence = copy.deepcopy(model.__dict__)
        with self.assertRaisesRegex(CorruptJournal, "FixedState"):
            list(model.recover())
        self.assertEqual(model.__dict__, evidence)

    def test_watchdog_covers_commit_before_lost_eventfd_notification(self):
        # Enumerate all order-preserving consumer/producer interleavings. FD
        # transport, notification generations, and real waits need native tests.
        def interleave(left, right):
            if not left:
                yield right
            elif not right:
                yield left
            else:
                for suffix in interleave(left[1:], right):
                    yield (left[0],) + suffix
                for suffix in interleave(left, right[1:]):
                    yield (right[0],) + suffix

        watchdog_needed = signal_woke = unarmed_commits = 0
        consumer = ("drain", "protected_check_arm_recheck", "sleep")
        for sends_signal in (False, True):
            producer = ("commit", "signal") if sends_signal else ("commit",)
            for spurious_count in (0, 1):
                for schedule in interleave(consumer, producer):
                    queued, count = False, spurious_count
                    delivered, sleeping, armed = False, False, False
                    notify_armed = False
                    for action in schedule:
                        if action == "commit":
                            queued = True
                            notify_armed = armed
                            unarmed_commits += int(not notify_armed)
                        elif action == "signal":
                            count += int(notify_armed)
                            if count and sleeping and not delivered:
                                delivered = queued
                                armed = not delivered
                                signal_woke += int(delivered)
                        elif not delivered:
                            if action == "drain":
                                count = 0
                            elif action == "protected_check_arm_recheck":
                                # This is one gate-protected transition: no
                                # producer commit can interleave its checks.
                                delivered = queued
                                armed = not delivered
                            elif action == "sleep":
                                sleeping = True
                                if count:
                                    delivered = queued
                                    armed = not delivered
                    if not delivered:
                        watchdog_needed += 1
                        # Assume the required eventual watchdog transition gets
                        # the gate after the final producer action. This checks
                        # the protocol's recheck rule, not actual 10-ms timing.
                        delivered = queued
                        armed = not delivered
                    self.assertTrue(delivered, schedule)
                    self.assertFalse(armed, schedule)
                    type(self).notification_schedules += 1
        self.assertGreater(watchdog_needed, 0)
        self.assertGreater(signal_woke, 0)
        self.assertGreater(unarmed_commits, 0)


def main():
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ProtocolModelTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    print(
        "Protocol model: "
        f"{ProtocolModelTest.scenarios} operation scenarios, "
        f"{ProtocolModelTest.crash_cuts} mutation crash cuts, "
        f"{ProtocolModelTest.recovery_cuts} recovery crash cuts. "
        f"{ProtocolModelTest.notification_schedules} notification schedules. "
        "Native mmap/OS/process-crash qualification remains separate.",
        flush=True,
    )
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
