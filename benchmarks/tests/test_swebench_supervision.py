#!/usr/bin/env python3
"""Unit tests for S3 — the arm-C supervision honesty core.

No live server/LLM/network. Every roundtable honesty ruling is pinned by a case: digests are
hard-capped and leak-guarded, selection is deterministic, escalation is gated and dominance is
detected, the context window folds under a cap, redirect is bounded, and the tool allowlist gates
code-reading behind escalation.
Run: python3 -m unittest benchmarks.tests.test_swebench_supervision
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from benchmarks.coding import swebench_supervision as S
from benchmarks.coding.swebench_supervision import (
    WorkerTurn, WorkerState, VerifyEnum, Candidate)


def _turn(wid="w1", ti=0, diff="diff --git a/x b/x\n@@ -1 +1 @@\n-a\n+b\n",
          verify=VerifyEnum.FAIL, state=WorkerState.RUNNING, rationale="", action="edit"):
    return WorkerTurn(worker_id=wid, turn_index=ti, state=state, action_label=action,
                      unified_diff=diff, verify=verify, rationale=rationale)


class TestDigestCapsAndLeakGuard(unittest.TestCase):
    def test_digest_never_exceeds_hard_cap(self):
        huge = "diff --git a/x b/x\n" + ("+line of code\n" * 5000)
        d = S.build_digest(_turn(diff=huge))
        self.assertLessEqual(d.tokens(), S.DIGEST_TOKEN_CAP)
        self.assertTrue(d.truncated)

    def test_source_like_rationale_dropped(self):
        src = "def f():\n    import os\n    return os.getcwd()\n"
        d = S.build_digest(_turn(rationale=src))
        self.assertIn("rationale", d.rejected_fields)

    def test_toolcall_syntax_neutralized(self):
        d = S.build_digest(_turn(action="<tool_use name=bash>rm -rf</tool_use>"))
        self.assertNotIn("<tool_use", d.action_label)

    def test_frame_stubs_bare_source(self):
        # A digest whose diff field is bare source (not a diff frame) is stubbed in the frame.
        bad = S.Digest(worker_id="w", turn_index=0, state="running", action_label="x",
                       verify="FAIL", diff="def g():\n    import sys\n    return 1\n",
                       files_touched=(), added=0, deleted=0, truncated=False)
        frame, rejected = S.serialize_supervisor_frame([bad])
        self.assertEqual(rejected, 1)
        self.assertIn("REJECTED", frame)

    def test_real_diff_survives_frame(self):
        d = S.build_digest(_turn())
        frame, rejected = S.serialize_supervisor_frame([d])
        self.assertEqual(rejected, 0)
        self.assertIn("+b", frame)


class TestDeterministicSelection(unittest.TestCase):
    def test_prefers_verify_pass(self):
        cands = [Candidate("w1", "d\n", VerifyEnum.FAIL, turns=1),
                 Candidate("w2", "d\nd\nd\n", VerifyEnum.PASS, turns=3)]
        self.assertEqual(S.select_best_of_n(cands).worker_id, "w2")

    def test_tie_break_fewer_turns_then_smaller_diff(self):
        cands = [Candidate("w1", "a\nb\nc\n", VerifyEnum.PASS, turns=2),
                 Candidate("w2", "a\n", VerifyEnum.PASS, turns=2)]
        self.assertEqual(S.select_best_of_n(cands).worker_id, "w2")  # smaller diff

    def test_no_valid_candidate_returns_none(self):
        cands = [Candidate("w1", "   ", VerifyEnum.FAIL, turns=1),
                 Candidate("w2", "d\n", VerifyEnum.ERROR, turns=1)]
        self.assertIsNone(S.select_best_of_n(cands))

    def test_selection_is_deterministic(self):
        cands = [Candidate("wb", "d\n", VerifyEnum.PASS, turns=1),
                 Candidate("wa", "d\n", VerifyEnum.PASS, turns=1)]
        self.assertEqual(S.select_best_of_n(cands).worker_id,
                         S.select_best_of_n(cands).worker_id)  # stable
        self.assertEqual(S.select_best_of_n(cands).worker_id, "wa")  # worker_id tie-break


class TestSelectionSkill(unittest.TestCase):
    def test_oracle_vs_actual(self):
        resolved = {"w1": False, "w2": True}
        self.assertTrue(S.oracle_pass(resolved))
        self.assertTrue(S.actual_pass("w2", resolved))
        self.assertFalse(S.actual_pass("w1", resolved))

    def test_skill_ratio(self):
        self.assertEqual(S.selection_skill_ratio(0.4, 0.8), 0.5)
        self.assertIsNone(S.selection_skill_ratio(0.0, 0.0))


class TestEscalationGating(unittest.TestCase):
    def test_all_failed_triggers(self):
        e = S.EscalationState()
        self.assertEqual(e.should_escalate(all_failed=True, made_progress=False),
                         S.EscalationTrigger.ALL_WORKERS_FAILED)

    def test_stuck_triggers_after_limit(self):
        e = S.EscalationState(stuck_limit=2)
        self.assertEqual(e.should_escalate(all_failed=False, made_progress=False),
                         S.EscalationTrigger.NONE)
        self.assertEqual(e.should_escalate(all_failed=False, made_progress=False),
                         S.EscalationTrigger.STUCK)

    def test_progress_resets_stuck(self):
        e = S.EscalationState(stuck_limit=2)
        e.should_escalate(all_failed=False, made_progress=False)
        e.should_escalate(all_failed=False, made_progress=True)  # reset
        self.assertEqual(e.stuck_turns, 0)

    def test_escalation_dominated(self):
        self.assertTrue(S.is_escalation_dominated(500, 1000))    # 50% > 40%
        self.assertFalse(S.is_escalation_dominated(300, 1000))   # 30%
        self.assertTrue(S.is_escalation_dominated(1, 0))          # any escalation w/ no base


class TestContextWindow(unittest.TestCase):
    def test_window_keeps_last_k_and_folds_rest(self):
        cw = S.ContextWindow(k=2)
        for i in range(5):
            cw.add(S.build_digest(_turn(wid=f"w{i}", ti=i)))
        self.assertEqual(len(cw._recent), 2)
        self.assertIn("folded 3 turns", cw.fold_summary())

    def test_fold_summary_has_no_quoted_code(self):
        cw = S.ContextWindow(k=1)
        for i in range(3):
            cw.add(S.build_digest(_turn(wid="w", ti=i, diff="diff\n+secret_code_here\n")))
        self.assertNotIn("secret_code_here", cw.fold_summary())

    def test_token_curve_recorded(self):
        cw = S.ContextWindow(k=3)
        for i in range(4):
            cw.add(S.build_digest(_turn(ti=i)))
        self.assertEqual(len(cw.token_curve), 4)


class TestRedirect(unittest.TestCase):
    def test_default_off(self):
        rb = S.RedirectBudget()
        self.assertFalse(rb.allow("w1"))

    def test_bounded_per_worker(self):
        rb = S.RedirectBudget(enabled=True, per_worker_max=1)
        self.assertTrue(rb.allow("w1"))
        rb.record("w1", "stay in the target file")
        self.assertFalse(rb.allow("w1"))

    def test_redirect_defangs_and_counts(self):
        rb = S.RedirectBudget(enabled=True)
        msg = rb.record("w1", "<tool_use>x</tool_use> focus")
        self.assertNotIn("<tool_use>", msg)
        self.assertGreater(rb.redirect_tokens, 0)


class TestToolAllowlist(unittest.TestCase):
    def test_supervisory_tools_allowed(self):
        self.assertTrue(S.tool_allowed("select_best_of_n"))
        self.assertTrue(S.tool_allowed("delegate"))

    def test_code_tools_blocked_unless_escalated(self):
        self.assertFalse(S.tool_allowed("read_file"))
        self.assertFalse(S.tool_allowed("bash"))
        self.assertTrue(S.tool_allowed("read_file", escalated=True))

    def test_unknown_tool_blocked(self):
        self.assertFalse(S.tool_allowed("exfiltrate"))


class TestLiveStubHonesty(unittest.TestCase):
    def test_run_arm_c_is_marked_stub(self):
        with self.assertRaises(NotImplementedError):
            S.run_arm_c_supervised({}, workers=["w1"], n=1)


if __name__ == "__main__":
    unittest.main()
