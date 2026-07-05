#!/usr/bin/env python3
"""Unit tests for S4 — the supervised-report panels.

Pure, deterministic. Covers BCa CI, Pareto dominance, failure-mode counts, selection-skill
oracle-vs-actual, escalation exclusion, context-drift distribution, two-wall-clock, and the
Q6 arm-parity invariants.
Run: python3 -m unittest benchmarks.tests.test_supervised_report_panels
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from benchmarks.coding import supervised_report_panels as P


class TestBCaCI(unittest.TestCase):
    def test_brackets_the_mean(self):
        vals = [10, 12, 9, 11, 13, 8, 10, 12]
        lo, hi = P.bca_ci(vals)
        mean = sum(vals) / len(vals)
        self.assertLessEqual(lo, mean)
        self.assertGreaterEqual(hi, mean)

    def test_deterministic(self):
        vals = [1.0, 2.0, 3.0, 4.0, 5.0]
        self.assertEqual(P.bca_ci(vals), P.bca_ci(vals))

    def test_degenerate_all_equal(self):
        lo, hi = P.bca_ci([5.0, 5.0, 5.0, 5.0])
        self.assertEqual((lo, hi), (5.0, 5.0))

    def test_single_value(self):
        self.assertEqual(P.bca_ci([7.0]), (7.0, 7.0))


class TestNormPpf(unittest.TestCase):
    def test_median_is_zero(self):
        self.assertAlmostEqual(P._norm_ppf(0.5), 0.0, places=6)

    def test_symmetry(self):
        self.assertAlmostEqual(P._norm_ppf(0.975), -P._norm_ppf(0.025), places=4)
        self.assertAlmostEqual(P._norm_ppf(0.975), 1.959964, places=4)


class TestPareto(unittest.TestCase):
    def test_frontier_excludes_dominated(self):
        configs = [
            {"name": "cheap_fast_good", "primary_tokens": 100, "wall_p95": 10, "resolve_rate": 0.9},
            {"name": "dominated", "primary_tokens": 200, "wall_p95": 20, "resolve_rate": 0.5},
            {"name": "tradeoff", "primary_tokens": 50, "wall_p95": 30, "resolve_rate": 0.95},
        ]
        out = P.pareto_panel(configs)
        self.assertIn("cheap_fast_good", out["frontier"])
        self.assertIn("tradeoff", out["frontier"])
        self.assertNotIn("dominated", out["frontier"])

    def test_dominated_records_its_dominator(self):
        configs = [
            {"name": "good", "primary_tokens": 100, "wall_p95": 10, "resolve_rate": 0.9},
            {"name": "bad", "primary_tokens": 200, "wall_p95": 20, "resolve_rate": 0.5},
        ]
        out = P.pareto_panel(configs)
        bad = next(c for c in out["configs"] if c["name"] == "bad")
        self.assertEqual(bad["dominated_by"], ["good"])


class TestFailureModes(unittest.TestCase):
    def test_counts_by_arm_and_mode(self):
        recs = [
            {"arm": "C", "resolved": True},
            {"arm": "C", "resolved": False, "failure_mode": "worker_dropout"},
            {"arm": "C", "resolved": False, "failure_mode": "mis_selection"},
            {"arm": "A", "resolved": True},
        ]
        out = P.failure_mode_breakdown(recs)
        self.assertEqual(out["C"]["ok"], 1)
        self.assertEqual(out["C"]["worker_dropout"], 1)
        self.assertEqual(out["C"]["mis_selection"], 1)
        self.assertEqual(out["A"]["ok"], 1)


class TestSelectionSkill(unittest.TestCase):
    def test_oracle_actual_and_ratio(self):
        recs = [
            {"arm": "C", "oracle_resolved": True, "actual_resolved": True},
            {"arm": "C", "oracle_resolved": True, "actual_resolved": False},  # recoverable miss
            {"arm": "C", "oracle_resolved": False, "actual_resolved": False},
            {"arm": "C", "oracle_resolved": True, "actual_resolved": True},
        ]
        out = P.selection_skill(recs)
        self.assertEqual(out["oracle_pass_rate"], 0.75)   # 3/4
        self.assertEqual(out["actual_pass_rate"], 0.5)    # 2/4
        self.assertEqual(out["selection_skill_ratio"], round(0.5 / 0.75, 4))
        self.assertEqual(out["recoverable_gap"], 1)

    def test_no_decomposition_records(self):
        self.assertEqual(P.selection_skill([{"arm": "C"}])["n"], 0)


class TestEscalationSplit(unittest.TestCase):
    def test_excludes_dominated_from_headline(self):
        recs = [
            {"arm": "C", "instance_id": "i1"},
            {"arm": "C", "instance_id": "i2", "escalation_dominated": True},
            {"arm": "C", "instance_id": "i3"},
        ]
        out = P.escalation_split(recs)
        self.assertEqual(out["headline_n"], 2)
        self.assertEqual(out["escalation_dominated_n"], 1)
        self.assertEqual(out["escalation_dominated"], ["i2"])


class TestContextDrift(unittest.TestCase):
    def test_peak_and_curve(self):
        recs = [
            {"arm": "C", "primary_context_tokens": 8000, "primary_tokens_by_turn": [100, 200, 300]},
            {"arm": "C", "primary_context_tokens": 12000, "primary_tokens_by_turn": [150, 250]},
        ]
        out = P.context_size_distribution(recs)
        self.assertEqual(out["peak_context"]["n"], 2)
        self.assertEqual(out["peak_context"]["max"], 12000)
        self.assertEqual(out["mean_tokens_by_turn"][0], 125.0)   # (100+150)/2
        self.assertEqual(out["mean_tokens_by_turn"][2], 300.0)   # only first record has turn 3


class TestWallTwoClock(unittest.TestCase):
    def test_total_and_work_separate(self):
        recs = [{"arm": "C", "wall_s": 30.0, "wall_work_s": 28.0},
                {"arm": "C", "wall_s": 40.0, "wall_work_s": 35.0}]
        out = P.wall_two_clock(recs, "C")
        self.assertEqual(out["total"]["n"], 2)
        self.assertEqual(out["work"]["max"], 35.0)
        self.assertLessEqual(out["work"]["p95"], out["total"]["p95"] + 1)


class TestArmParity(unittest.TestCase):
    def _a(self, **kw):
        base = {"supervisor_input_tokens": 100000, "supervisor_output_tokens": 4000,
                "worker_input_tokens": 0, "worker_output_tokens": 0,
                "patch_fingerprint": "abc", "resolved": True}
        base.update(kw)
        return base

    def _c(self, **kw):
        base = {"supervisor_input_tokens": 800, "supervisor_output_tokens": 100,
                "patch_fingerprint": "abc", "resolved": True, "escalations": 0}
        base.update(kw)
        return base

    def test_parity_all_pass_on_trivial(self):
        inv = P.check_arm_parity(self._a(), self._c())
        self.assertTrue(inv["all_pass"], inv)

    def test_different_patch_fails(self):
        inv = P.check_arm_parity(self._a(patch_fingerprint="x"), self._c(patch_fingerprint="y"))
        self.assertFalse(inv["same_patch"])
        self.assertFalse(inv["all_pass"])

    def test_c_not_cheaper_fails(self):
        # C primary spend not an order of magnitude below A total
        inv = P.check_arm_parity(self._a(), self._c(supervisor_input_tokens=90000))
        self.assertFalse(inv["c_primary_much_cheaper"])

    def test_escalation_on_trivial_fails(self):
        inv = P.check_arm_parity(self._a(), self._c(escalations=1))
        self.assertFalse(inv["no_escalation_on_trivial"])


class TestAugment(unittest.TestCase):
    def test_attaches_panels(self):
        recs = [{"arm": "C", "instance_id": "i1", "wall_s": 10.0, "resolved": True}]
        rep = P.augment_report({"base": 1}, recs)
        for k in ("failure_modes", "selection_skill", "escalation", "context_drift",
                  "wall_two_clock"):
            self.assertIn(k, rep)


if __name__ == "__main__":
    unittest.main()
