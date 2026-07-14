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

    def test_close_logs_teardown_and_clears_state(self):
        self.gate.open_escalation(caller="arm_c", reason="r")
        before = self.ledger.balance
        rec = self.gate.close_escalation(caller="arm_c", detail="done")
        self.assertFalse(rec.granted)
        self.assertEqual(rec.caller, "arm_c")
        self.assertEqual(self.ledger.balance, before - ESCALATION_COST_TOKENS)
        self.assertEqual(len(self.ledger.escalations), 2)
        self.assertFalse(self.gate.is_open)

    def test_open_charge_reflected_in_ledger_entries(self):
        self.gate.open_escalation(caller="arm_c", reason="r")
        self.gate.close_escalation(caller="arm_c")
        # 2 charges: open + close, each ESCALATION_COST_TOKENS
        self.assertEqual(len(self.ledger.entries), 2)
        self.assertEqual(self.ledger.entries[0].source, "escalation.open:arm_c")
        self.assertEqual(self.ledger.entries[1].source, "escalation.close:arm_c")
        for e in self.ledger.entries:
            self.assertEqual(e.tokens, ESCALATION_COST_TOKENS)

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
        sup = Supervisor()
        sup.record_turn(0, 100)
        sup.record_turn(1, 200)
        sup.gate.open_escalation(caller="arm_c", reason="r")
        sup.gate.close_escalation(caller="arm_c")
        # 2 primary turns + 2 escalation charges = 4 entries
        self.assertEqual(len(sup.ledger.entries), 4)
        sources = [e.source for e in sup.ledger.entries]
        self.assertEqual(sources, [
            "primary.turn:0", "primary.turn:1",
            "escalation.open:arm_c", "escalation.close:arm_c",
        ])


if __name__ == "__main__":
    unittest.main()
