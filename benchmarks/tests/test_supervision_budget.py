#!/usr/bin/env python3
"""Unit tests for supervision_budget - S3 supervisor honesty core.

Run via the standard project test runner::

    python -m unittest benchmarks.tests.test_supervision_budget -v

Acceptance criteria covered (roundtable approval p3):

  1. Digest capper: 16,000-token cap, at-boundary / over-boundary behavior, overflow label.
  2. Escalation gating: explicit budget charge on every attempt, success+failure logged,
     caller attribution present in the ledger.
  3. Tool allowlist: read_file/bash/grep blocked outside escalation, allowed inside.
  4. Telemetry: primary_tokens_by_turn time series emitted per turn.
"""
from __future__ import annotations

import unittest

from benchmarks.coding.supervision_budget import (
    BudgetLedger,
    CODE_TOOLS,
    DIGEST_CAP,
    ESCALATION_COST_TOKENS,
    EscalationDenied,
    EscalationGate,
    SUPERVISOR_TOOLS,
    Supervisor,
    ToolDispatcher,
    cap_digest,
    est_tokens,
    tool_allowed,
)


class DigestCapperTests(unittest.TestCase):

    def test_under_cap_preserved(self):
        text = "x" * 100
        capped, orig, final = cap_digest(text)
        self.assertEqual(capped, text)
        self.assertEqual(orig, 25)
        self.assertEqual(final, 25)

    def test_at_cap_exact_boundary_preserved(self):
        # DIGEST_CAP tokens exactly - boundary must be inclusive (at-or-below preserved).
        text = "a" * (DIGEST_CAP * 4)
        self.assertEqual(est_tokens(text), DIGEST_CAP)
        capped, orig, final = cap_digest(text)
        self.assertEqual(capped, text)
        self.assertEqual(orig, DIGEST_CAP)
        self.assertEqual(final, DIGEST_CAP)

    def test_over_cap_truncated_with_overflow_note(self):
        text = "b" * ((DIGEST_CAP + 100) * 4)
        capped, orig, final = cap_digest(text)
        self.assertLessEqual(final, DIGEST_CAP)
        self.assertGreater(orig, DIGEST_CAP)
        self.assertIn("[OVERFLOW:", capped)
        self.assertIn("digest truncated at", capped)
        self.assertIn(f"original was {orig} tokens", capped)

    def test_overflow_note_format(self):
        text = "c" * ((DIGEST_CAP + 10) * 4)
        capped, _, _ = cap_digest(text)
        self.assertTrue(capped.endswith("]"))
        self.assertIn(str(DIGEST_CAP), capped)

    def test_respects_custom_cap(self):
        text = "d" * 8000
        capped, orig, final = cap_digest(text, cap=1000)
        self.assertLessEqual(final, 1000)
        self.assertEqual(orig, 2000)
        self.assertIn("[OVERFLOW:", capped)

    def test_tiny_cap_does_not_exceed_contract(self):
        # Regression: if the overflow note alone already exceeds ``cap``, the
        # older implementation returned the over-cap note because the truncate
        # loop's ``while head`` guard prevented any progress. The public
        # ``est_tokens(capped) <= cap`` contract must hold for every cap.
        text = "x" * 4000
        for tiny_cap in (1, 5, 10):
            capped, orig, final = cap_digest(text, cap=tiny_cap)
            self.assertLessEqual(
                final, tiny_cap,
                f"cap_digest exceeded cap={tiny_cap}: {final} > {tiny_cap}",
            )
            self.assertEqual(orig, 1000)

    def test_empty_string(self):
        capped, orig, final = cap_digest("")
        self.assertEqual(capped, "")
        self.assertEqual(orig, 0)
        self.assertEqual(final, 0)


class EscalationGateTests(unittest.TestCase):

    def setUp(self):
        self.ledger = BudgetLedger()
        self.gate = EscalationGate(self.ledger)

    def test_closed_by_default(self):
        self.assertFalse(self.gate.is_open)
        self.assertIsNone(self.gate.open_record)

    def test_open_costs_budget_and_logs(self):
        before = self.ledger.balance
        rec = self.gate.open_escalation(caller="arm_c", reason="candidate diff empty")
        self.assertTrue(rec.granted)
        self.assertEqual(rec.caller, "arm_c")
        self.assertEqual(rec.cost_tokens, ESCALATION_COST_TOKENS)
        self.assertEqual(self.ledger.balance, before - ESCALATION_COST_TOKENS)
        self.assertIn(rec, self.ledger.escalations)
        self.assertTrue(self.gate.is_open)

    def test_close_does_not_double_charge(self):
        # Regression: closing must NOT charge a second ESCALATION_COST_TOKENS.
        # The gate privilege was bought at open time; closing merely records
        # whether the dispatch actually succeeded, so the audit log and balance
        # must reflect exactly one charge per escalation.
        self.gate.open_escalation(caller="arm_c", reason="r")
        before = self.ledger.balance
        rec = self.gate.close_escalation(caller="arm_c", detail="done")
        # Same open record, mutated: still 1 record, no second entry, balance
        # drops by exactly ESCALATION_COST_TOKENS (the open charge), not 2x.
        self.assertEqual(rec.caller, "arm_c")
        self.assertTrue(rec.granted)
        self.assertEqual(self.ledger.balance, before)
        self.assertEqual(len(self.ledger.escalations), 1)
        self.assertEqual(len(self.ledger.entries), 1)
        self.assertEqual(self.ledger.entries[0].source, "escalation.open:arm_c")
        self.assertFalse(self.gate.is_open)

    def test_close_records_unused_outcome(self):
        # Regression: when the dispatch was opened but never actually used,
        # the open record's ``granted`` must flip to False so the audit log
        # reflects intent rather than intent-masking "gate bought" semantics.
        self.gate.open_escalation(caller="arm_c", reason="r")
        rec = self.gate.close_escalation(
            caller="arm_c", actual_used=False, detail="cancelled",
        )
        self.assertFalse(rec.granted)
        self.assertEqual(rec.detail, "cancelled")
        # Still no second charge even when the gate was unused.
        self.assertEqual(len(self.ledger.entries), 1)
        self.assertEqual(len(self.ledger.escalations), 1)

    def test_close_idempotent_when_no_open(self):
        # Closing a gate that was never opened must NOT pretend a charge happened.
        before = self.ledger.balance
        self.assertIsNone(self.gate.close_escalation(caller="arm_c"))
        self.assertEqual(self.ledger.balance, before)
        self.assertEqual(len(self.ledger.entries), 0)
        self.assertEqual(len(self.ledger.escalations), 0)

    def test_open_charge_reflected_in_ledger_entries(self):
        self.gate.open_escalation(caller="arm_c", reason="r")
        self.gate.close_escalation(caller="arm_c")
        # 1 charge total: open only. close is a state mutation, not a billable
        # event - see test_close_does_not_double_charge above.
        self.assertEqual(len(self.ledger.entries), 1)
        self.assertEqual(self.ledger.entries[0].source, "escalation.open:arm_c")
        self.assertEqual(self.ledger.entries[0].tokens, ESCALATION_COST_TOKENS)

    def test_caller_attribution_distinct(self):
        gate_b = EscalationGate(self.ledger)
        self.gate.open_escalation(caller="arm_c", reason="a")
        gate_b.open_escalation(caller="primary", reason="b")
        sources = [e.source for e in self.ledger.entries]
        self.assertIn("escalation.open:arm_c", sources)
        self.assertIn("escalation.open:primary", sources)


class ToolAllowlistTests(unittest.TestCase):

    def test_supervisor_tools_always_allowed(self):
        for tool in SUPERVISOR_TOOLS:
            self.assertTrue(tool_allowed(tool, escalated=False),
                            f"supervisor tool {tool!r} must always be allowed")
            self.assertTrue(tool_allowed(tool, escalated=True))

    def test_code_tools_blocked_outside_escalation(self):
        for tool in ("read_file", "bash", "grep", "cat", "open", "edit_file"):
            self.assertFalse(tool_allowed(tool, escalated=False),
                             f"code tool {tool!r} must be blocked outside escalation")
            self.assertTrue(tool_allowed(tool, escalated=True),
                            f"code tool {tool!r} must be allowed inside escalation")

    def test_unknown_tools_fail_closed(self):
        self.assertFalse(tool_allowed("rm_rf", escalated=False))
        self.assertFalse(tool_allowed("rm_rf", escalated=True))

    def test_dispatcher_raises_on_blocked_code_tool(self):
        ledger = BudgetLedger()
        gate = EscalationGate(ledger)
        d = ToolDispatcher(gate)
        # Supervisor tools pass.
        d.check("delegate")
        d.check("gated_escalate")
        # Code tools blocked outside escalation.
        for tool in CODE_TOOLS:
            with self.assertRaises(EscalationDenied, msg=f"expected denial for {tool}"):
                d.check(tool)
        # Open the gate and they should pass.
        gate.open_escalation(caller="arm_c", reason="r")
        for tool in CODE_TOOLS:
            d.check(tool)  # must not raise
        # Close it: blocked again.
        gate.close_escalation(caller="arm_c")
        for tool in CODE_TOOLS:
            with self.assertRaises(EscalationDenied):
                d.check(tool)


class TelemetryTests(unittest.TestCase):

    def test_primary_tokens_by_turn_is_running_total(self):
        ledger = BudgetLedger()
        ledger.record_turn(0, primary_tokens_this_turn=100)
        ledger.record_turn(1, primary_tokens_this_turn=150)
        ledger.record_turn(2, primary_tokens_this_turn=50)
        # Each entry is the accumulated primary spend at end-of-turn.
        self.assertEqual(ledger.primary_tokens_by_turn, [100, 250, 300])

    def test_record_turn_rejects_negative(self):
        ledger = BudgetLedger()
        with self.assertRaises(ValueError):
            ledger.record_turn(0, -1)

    def test_charge_rejects_negative(self):
        ledger = BudgetLedger()
        with self.assertRaises(ValueError):
            ledger.charge(-5, source="x")

    def test_drift_detects_monotonicity_violation(self):
        # The ``turn_boundaries`` series is the auditor-visible primary spend
        # curve. By construction ``record_turn`` only appends a non-decreasing
        # boundary, so the only way the curve can back down is if someone
        # tampered with the ledger from outside. ``detect_drift`` must catch
        # exactly that bypass: mutation directly on ``turn_boundaries`` so a
        # primary spend was unwound outside the audit log.
        ledger = BudgetLedger()
        ledger.record_turn(0, 100)
        ledger.record_turn(1, 250)
        ledger.record_turn(2, 300)
        # External tamper: rewind the running total - simulate an attacker
        # unwinding primary spend without going through ``record_turn``.
        ledger.turn_boundaries[-1] = 100
        findings = ledger.detect_drift()
        self.assertEqual(len(findings), 1)
        self.assertIn("monotonicity violated", findings[0])

    def test_drift_ignores_honest_monotonic_curve(self):
        # Sanity: as long as every turn's running total is non-decreasing
        # (which ``record_turn`` enforces), ``detect_drift`` reports zero
        # monotonicity findings even when per-turn deltas are large.
        ledger = BudgetLedger()
        for i, delta in enumerate([100, 250, 200, 300]):
            ledger.record_turn(i, delta)
        # Only check the monotonicity part - rapid growth may or may not
        # fire on this curve, but monotonicity MUST be clean.
        findings = [f for f in ledger.detect_drift() if "monotonicity" in f]
        self.assertEqual(findings, [])

    def test_drift_detects_rapid_growth(self):
        # ``record_turn`` accepts per-turn DELTAS. A 10x jump over the rolling
        # median of recent deltas is the rapid-growth signal that audit should
        # surface; it's the kind of curve that often accompanies a primary
        # agent digging into the codebase instead of delegating.
        ledger = BudgetLedger()
        # First five turns: small steady deltas (median ~10).
        for i, d in enumerate([10, 11, 9, 12, 10]):
            ledger.record_turn(i, d)
        # Sixth turn: a 500x spike. This MUST fire the rapid-growth alarm.
        ledger.record_turn(5, 5000)
        findings = ledger.detect_drift()
        self.assertTrue(any("grew rapidly" in f for f in findings),
                        f"expected rapid-growth finding, got {findings!r}")

    def test_drift_clean_curve_no_findings(self):
        ledger = BudgetLedger()
        for i, d in enumerate([50, 60, 70, 80, 90]):
            ledger.record_turn(i, d)
        self.assertEqual(ledger.detect_drift(), [])

    def test_drift_handles_short_history(self):
        # Zero or one turn: not enough data to flag anything.
        ledger = BudgetLedger()
        self.assertEqual(ledger.detect_drift(), [])
        ledger.record_turn(0, 100)
        self.assertEqual(ledger.detect_drift(), [])


class SupervisorFacadeTests(unittest.TestCase):

    def test_full_lifecycle(self):
        sup = Supervisor()
        # 1) record three turns of primary spend
        self.assertEqual(sup.record_turn(0, 400), 400)
        self.assertEqual(sup.record_turn(1, 600), 1000)
        self.assertEqual(sup.record_turn(2, 250), 1250)
        self.assertEqual(sup.primary_tokens_by_turn, [400, 1000, 1250])
        # 2) cap a digest
        text = "z" * ((DIGEST_CAP + 50) * 4)
        capped, orig, final = sup.cap_digest(text)
        self.assertLessEqual(final, DIGEST_CAP)
        self.assertIn("[OVERFLOW:", capped)
        # 3) tools: supervisor tools fine, code tools denied
        sup.check_tool_dispatch("delegate")
        with self.assertRaises(EscalationDenied):
            sup.check_tool_dispatch("read_file")
        # 4) open escalation, code tools allowed
        sup.gate.open_escalation(caller="arm_c", reason="empty diff")
        sup.check_tool_dispatch("read_file")
        sup.check_tool_dispatch("bash")
        sup.gate.close_escalation(caller="arm_c")
        with self.assertRaises(EscalationDenied):
            sup.check_tool_dispatch("read_file")

    def test_ledger_audit_trail_complete(self):
        # Audit trail after one open + close + two primary turns:
        #   - 2 primary.turn entries (one per record_turn)
        #   - 1 escalation.open entry (the gate privilege purchase)
        #   - 0 escalation.close entries (close is a state mutation, not a
        #     billable event - see test_close_does_not_double_charge)
        # Total: 3 entries, 1 escalation record (the open record, mutated).
        sup = Supervisor()
        sup.record_turn(0, 100)
        sup.record_turn(1, 200)
        sup.gate.open_escalation(caller="arm_c", reason="r")
        open_rec = sup.gate.open_record
        sup.gate.close_escalation(caller="arm_c")
        self.assertEqual(len(sup.ledger.entries), 3)
        sources = [e.source for e in sup.ledger.entries]
        # Per-turn distinct: each record_turn call carries a unique nonce suffix
        # so two record_turn calls with the same turn_index cannot collapse into
        # one entry. The escalation.open entry is unique by caller identity.
        self.assertEqual(sources, [
            "primary.turn:0#0", "primary.turn:1#1", "escalation.open:arm_c",
        ])
        # And per_turn_primary retains one distinct row per call, not one row
        # per turn_index.
        self.assertEqual(len(sup.ledger.per_turn_primary), 2)
        # The single escalation record is the (mutated) open record, not a
        # separate close record - the audit log must show one gate event per
        # escalation, never two.
        self.assertEqual(len(sup.ledger.escalations), 1)
        self.assertIs(sup.ledger.escalations[0], open_rec)
        self.assertTrue(sup.ledger.escalations[0].granted)


if __name__ == "__main__":
    unittest.main()


class ReviewerBlockerTests(unittest.TestCase):
    """Tests that lock down the three reviewer blockers from the second round."""

    def test_double_open_escalation_is_rejected(self):
        """Re-opening the gate while one is open must NOT orphan the active record.

        Reviewer evidence: 'A second open_escalation() overwrites _open without
        rejecting or closing the existing authorization.' The fix is a hard
        refusal that names the existing caller in the error so the audit gap
        is impossible to walk past silently.
        """
        L = BudgetLedger()
        G = EscalationGate(L)
        G.open_escalation(caller="arm_c", reason="diff empty")
        with self.assertRaises(EscalationDenied) as cm:
            G.open_escalation(caller="other_caller", reason="noise")
        self.assertIn("arm_c", str(cm.exception))
        self.assertTrue(G.is_open)
        # Original record is intact and still bound to the original caller.
        self.assertEqual(G.open_record.caller, "arm_c")

    def test_close_enforces_caller_identity(self):
        """close_escalation must reject a caller that is not the opening caller.

        Reviewer evidence: 'close_escalation(caller=...) ignores its caller
        argument, and tool checks only inspect the global boolean is_open.'
        The fix enforces identity so caller B cannot close A's gate and so
        no third party can use code tools under A's authorization.
        """
        L = BudgetLedger()
        G = EscalationGate(L)
        G.open_escalation(caller="arm_c", reason="diff empty")
        with self.assertRaises(EscalationDenied) as cm:
            G.close_escalation(caller="other_caller")
        self.assertIn("caller attribution", str(cm.exception))
        # Gate stays open; the wrong-caller close did not steal the record.
        self.assertTrue(G.is_open)
        self.assertEqual(G.open_record.caller, "arm_c")
        # The legitimate caller can still close it.
        closed = G.close_escalation(caller="arm_c")
        self.assertIsNotNone(closed)
        self.assertFalse(G.is_open)

    def test_open_carries_used_tokens_on_record(self):
        """The EscalationRecord now records realized escalation input token spend.

        Reviewer note: 'The caller-attributed escalation record also contains
        only the fixed 256-token permission cost. Actual escalation input
        usage (e_in) is recorded separately ... As a result, the emitted
        audit data does not provide a single record associating the caller
        with the escalation's realized token use.' Fix: ``used_tokens`` is
        recorded on the same row as cost_tokens.
        """
        L = BudgetLedger()
        G = EscalationGate(L)
        rec = G.open_escalation(caller="arm_c", reason="r", used_tokens=4096)
        self.assertEqual(rec.cost_tokens, ESCALATION_COST_TOKENS)
        self.assertEqual(rec.used_tokens, 4096)
        # And closing can update used_tokens to a final value.
        closed = G.close_escalation(caller="arm_c", used_tokens=5120)
        self.assertEqual(closed.used_tokens, 5120)

    def test_hard_model_token_cap_refuses_negative_balance(self):
        """charge() must refuse requests that would drive the ledger negative.

        Reviewer note: 'The cap set is -256, and unlock code tools. Thus
        budget availability does not actually gate escalation.' Fix:
        HARD_MODEL_TOKEN_CAP is the floor; charges below it raise.
        """
        L = BudgetLedger(starting_balance=512)
        # First 256-token escalation spend: succeeds, balance -> 256.
        L.charge(256, source="escalation.open:arm_c")
        self.assertEqual(L.balance, 256)
        # Next charge that would push negative: refused.
        with self.assertRaises(RuntimeError) as cm:
            L.charge(512, source="escalation.open:arm_c")
        self.assertIn("hard model-token cap exceeded", str(cm.exception))
        # Ledger never went below zero.
        self.assertGreaterEqual(L.balance, 0)

    def test_primary_budget_cap_is_independent(self):
        """primary_budget is an explicit sub-cap so escalation cannot drain it."""
        L = BudgetLedger(primary_budget=100)
        L.record_turn(0, 60)
        L.record_turn(1, 39)  # total primary = 99, ok
        with self.assertRaises(RuntimeError) as cm:
            L.record_turn(2, 50)  # would push primary to 149 > 100
        self.assertIn("primary_budget cap exceeded", str(cm.exception))

    def test_per_turn_primary_records_one_point_per_call(self):
        """record_turn yields one telemetry point per call, not per turn_index."""
        L = BudgetLedger()
        L.record_turn(0, 100)
        L.record_turn(0, 100)  # same turn_index, second distinct call
        L.record_turn(1, 100)
        # Three distinct calls, three distinct points and rows.
        self.assertEqual(len(L.per_turn_primary), 3)
        self.assertEqual(len(L.turn_boundaries), 3)
        # Sources are nonce-suffixed to disambiguate.
        sources = [e.source for e in L.entries]
        self.assertEqual(sources, [
            "primary.turn:0#0", "primary.turn:0#1", "primary.turn:1#2",
        ])
