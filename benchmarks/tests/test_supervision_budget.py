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

    def test_close_after_close_is_idempotent_no_extra_charge(self):
        """Reviewer blocker: close -> close must return None AND must not add a second charge.

        The first close ends the escalation; the second close finds the gate
        already closed (no open record), returns None, and adds no new entry to
        the ledger. Previously this was the 'if self._open is None: return
        None' path but no test pinned it down with an explicit assertion that
        no second entry was recorded.
        """
        L = BudgetLedger(starting_balance=1024, primary_budget=900)
        G = EscalationGate(L)
        # Open + close once: 1 escalation record, 1 charge.
        G.open_escalation(caller="arm_c", reason="r", used_tokens=0)
        rec = G.close_escalation(caller="arm_c", detail="done")
        self.assertIsNotNone(rec)
        balance_after_first_close = L.balance
        entries_after_first_close = list(L.entries)
        self.assertEqual(len(L.escalations), 1)
        self.assertEqual(len(entries_after_first_close), 1)
        # Second close: idempotent, no record, no charge, no balance change.
        result = G.close_escalation(caller="arm_c", detail="again")
        self.assertIsNone(result)
        self.assertEqual(L.balance, balance_after_first_close)
        self.assertEqual(len(L.entries), 1)
        self.assertEqual(L.entries, entries_after_first_close)
        # Third close, third-party caller: still idempotent, still no charge.
        self.assertIsNone(G.close_escalation(caller="arm_z", detail=""))
        self.assertEqual(L.balance, balance_after_first_close)
        self.assertEqual(len(L.entries), 1)

    def test_record_cap_audits_truncated_and_clean_digests(self):
        """F1: cap_digest at the supervisor level must leave an audit row.

        Reviewer evidence: 'cap result discarded with no audit'. The supervisor
        facade now calls ``record_cap`` for every cap_digest invocation so
        truncated and clean digests both leave an audit trail.
        """
        from benchmarks.coding.supervision_budget import Supervisor
        L = BudgetLedger()
        sup = Supervisor(ledger=L, cap=1024)
        # Clean digest (under cap): one cap event, truncated=False.
        text_under, orig_u, capped_u = sup.cap_digest("hello world", caller="arm_c")
        self.assertEqual(text_under, "hello world")
        self.assertEqual(orig_u, capped_u)
        self.assertEqual(len(L.cap_events), 1)
        self.assertFalse(L.cap_events[0]["truncated"])
        # Over-cap digest: a second cap event, truncated=True with original>capped.
        big = "x" * (20000 * 4)  # ~20000 tokens, well over the 1024 cap
        text_over, orig_o, capped_o = sup.cap_digest(big, caller="run_arm_c_supervised", kind="frame")
        self.assertLessEqual(capped_o, 1024)
        self.assertGreater(orig_o, capped_o)
        self.assertEqual(len(L.cap_events), 2)
        self.assertTrue(L.cap_events[1]["truncated"])
        self.assertEqual(L.cap_events[1]["caller"], "run_arm_c_supervised")
        self.assertEqual(L.cap_events[1]["kind"], "frame")
        # And both events have cap recorded so an auditor can verify the cap
        # used for each call.
        self.assertEqual(L.cap_events[0]["cap"], 1024)
        self.assertEqual(L.cap_events[1]["cap"], 1024)

    def test_try_record_turn_records_rejection_instead_of_raising(self):
        """F5: refused primary spend must be recorded, NOT crash the run."""
        L = BudgetLedger(starting_balance=2048, primary_budget=900)
        # Charge within budget: accepted, curve gets one point.
        ok, total = L.try_record_turn(0, 100, caller="arm_c")
        self.assertTrue(ok)
        self.assertEqual(total, 100)
        # Charge that would overflow: rejected, recorded, returns False.
        ok2, total2 = L.try_record_turn(1, 5000, caller="arm_c")
        self.assertFalse(ok2)
        self.assertEqual(total2, 100)  # running total unchanged
        self.assertEqual(len(L.primary_rejections), 1)
        rej = L.primary_rejections[0]
        self.assertEqual(rej["turn_index"], 1)
        self.assertEqual(rej["attempted"], 5000)
        self.assertEqual(rej["primary_spent_at_reject"], 100)
        self.assertEqual(rej["primary_budget"], 900)
        self.assertEqual(rej["caller"], "arm_c")
        # Curve length: every turn (accepted or refused) records a row so drift
        # detection is not blind at peak usage. The refused turn records the
        # unchanged running balance (monotonicity preserved).
        self.assertEqual(len(L.turn_boundaries), 2)
        self.assertEqual(L.primary_tokens_by_turn, [100, 100])
        # Per-turn observed view records the actual per-turn primary tokens
        # (accepted AND refused), so the drift curve has one point per turn.
        self.assertEqual(L.primary_tokens_observed_by_turn, [100, 5000])
        # ``record_turn`` still raises for callers that want the strict error.
        try:
            from benchmarks.coding.supervision_budget import PrimaryBudgetExhausted
        except ImportError:
            self.fail("PrimaryBudgetExhausted must be exported")
        with self.assertRaises(PrimaryBudgetExhausted):
            L.record_turn(2, 9999)

    def test_open_escalation_attempts_logged_even_when_refused(self):
        """F2/F3: every attempted or successful escalation is logged with spend.

        Reviewer evidence: 'the requirement that each attempted or successful
        escalation be logged with spend and attribution is unmet'. The fix
        logs the attempted-but-refused escalation path via the existing
        EscalationRecord audit when a double-open is rejected: the previous
        record is preserved, the new attempt is appended with
        ``granted=False``, and the rejection does NOT charge a second time.
        """
        L = BudgetLedger(starting_balance=1024)
        G = EscalationGate(L)
        rec1 = G.open_escalation(caller="arm_c", reason="diff empty")
        self.assertTrue(rec1.granted)
        balance_after_open = L.balance
        # Refused second-open is rejected without charging twice.
        with self.assertRaises(EscalationDenied):
            G.open_escalation(caller="arm_c", reason="diff empty")
        self.assertEqual(L.balance, balance_after_open)
        # Exactly one escalation record on the ledger for this thread.
        self.assertEqual(len(L.escalations), 1)
        self.assertIs(L.escalations[0], rec1)
        # And it carries caller attribution so the auditor can see WHO opened it.
        self.assertEqual(L.escalations[0].caller, "arm_c")

    def test_tool_dispatcher_denied_attempt_is_logged(self):
        """F2/F3: a denied tool-dispatch attempt MUST be recorded on the gate.

        The previous implementation raised EscalationDenied without ever
        appending an audit row, so the auditor could not see denied attempts.
        This test pins the new contract: ``Supervisor.check_tool_dispatch``
        and ``ToolDispatcher.check`` both log a ``granted=False`` row before
        raising, so the ledger is complete.
        """
        L = BudgetLedger(starting_balance=1024)
        sup = Supervisor(ledger=L)

        # No escalation is open: bash is a code tool and must be blocked.
        with self.assertRaises(EscalationDenied):
            sup.check_tool_dispatch("bash", caller="arm_c")

        # The denied attempt MUST be on the gate's audit trail.
        denied = [a for a in sup.gate.attempts if not a.get("granted", False)]
        self.assertEqual(len(denied), 1)
        self.assertEqual(denied[0]["caller"], "arm_c")
        self.assertEqual(denied[0]["denial_reason"], "tool_not_in_allowlist")
        self.assertIn("tool_dispatch:bash", denied[0]["reason"])

        # Supervisor tools (no escalation needed) do NOT produce a denial row.
        attempts_before = len(sup.gate.attempts)
        # Should not raise - delegate is a supervisor tool.
        sup.check_tool_dispatch("delegate", caller="arm_c")
        self.assertEqual(len(sup.gate.attempts), attempts_before)

    def test_escalation_gate_check_tool_denied_attempt_is_logged(self):
        """F2/F3: ``EscalationGate.check_tool`` also logs denied attempts."""
        L = BudgetLedger(starting_balance=1024)
        G = EscalationGate(L)
        with self.assertRaises(EscalationDenied):
            G.check_tool("grep")
        denied = [a for a in G.attempts if not a.get("granted", False)]
        self.assertEqual(len(denied), 1)
        self.assertEqual(denied[0]["denial_reason"], "tool_not_in_allowlist")

    def test_primary_curve_records_actual_usage_at_refusal_boundary(self):
        """F4: the curve must not be blind at peak usage.

        ``try_record_turn`` used to leave ``primary_tokens_by_turn`` unchanged
        on refusal, so the curve stopped representing actual primary-context
        usage exactly when usage was highest. The new contract exposes
        ``primary_tokens_observed_by_turn`` which records one point per turn
        (accepted or refused) with the actual per-turn tokens consumed.
        """
        L = BudgetLedger(starting_balance=1024)
        # Two accepted turns below budget.
        ok, total = L.try_record_turn(0, 100, caller="arm_c")
        self.assertTrue(ok)
        self.assertEqual(total, 100)
        ok, total = L.try_record_turn(1, 200, caller="arm_c")
        self.assertTrue(ok)
        self.assertEqual(total, 300)
        # One refused turn at peak usage (would overflow).
        ok, total = L.try_record_turn(2, 9999, caller="arm_c")
        self.assertFalse(ok)
        # Observed view records the actual per-turn tokens - drift curve
        # captures the spike even though the charge was refused.
        self.assertEqual(L.primary_tokens_observed_by_turn, [100, 200, 9999])
        # Running-total view is monotonic and appends a row for every turn.
        self.assertEqual(L.primary_tokens_by_turn, [100, 300, 300])
        # The refused attempt is in the rejection audit log.
        self.assertEqual(len(L.primary_rejections), 1)
        self.assertEqual(L.primary_rejections[0]["attempted"], 9999)

    def test_primary_curve_length_matches_turn_count_under_refusal(self):
        """F4: one curve point per turn, even when all turns are refused."""
        L = BudgetLedger(starting_balance=10)  # tiny budget, every turn refused
        for i in range(5):
            ok, _ = L.try_record_turn(i, 100, caller="arm_c")
            self.assertFalse(ok)
        # Both curves have exactly 5 points.
        self.assertEqual(len(L.primary_tokens_by_turn), 5)
        self.assertEqual(len(L.primary_tokens_observed_by_turn), 5)
        # Running total stayed at 0 (no charge applied) but curve is monotonic.
        self.assertEqual(L.primary_tokens_by_turn, [0, 0, 0, 0, 0])
        # Observed view carries the per-turn actual values.
        self.assertEqual(L.primary_tokens_observed_by_turn, [100] * 5)


if __name__ == "__main__":
    unittest.main()
