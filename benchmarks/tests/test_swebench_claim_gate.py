#!/usr/bin/env python3
"""Unit tests for S6 — the fail-closed public-claim gate.

Pure. The gate must EMIT only when every criterion holds on BOTH benchmarks with an independent
review, and must WITHHOLD (fail closed) on any single unmet criterion — while always producing an
honest summary. Each criterion has a case that breaks it in isolation.
Run: python3 -m unittest benchmarks.tests.test_swebench_claim_gate
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from benchmarks.coding import swebench_claim_gate as G
from benchmarks.coding.swebench_claim_gate import BenchmarkMetrics as M


def _good(name, **kw):
    base = dict(name=name, token_reduction_ci=(0.60, 0.72), wall_p95_ratio_ci=(0.7, 0.95),
                resolved_c=8, resolved_a=9, total=10, k=10, escalation_excluded_frac=0.1,
                n_anchor=1)
    base.update(kw)
    return M(**base)


class TestPasses(unittest.TestCase):
    def test_emits_when_all_hold(self):
        d = G.evaluate_claim_gate(_good("b1"), _good("b2", k=3), independent_review=True)
        self.assertTrue(d["emit_claim"], d["withheld_reasons"])
        self.assertIn("Beats Reddit", d["summary"])


class TestFailClosedPerCriterion(unittest.TestCase):
    def test_token_ci_lo_not_positive_withholds(self):
        d = G.evaluate_claim_gate(_good("b1", token_reduction_ci=(-0.01, 0.5)),
                                  _good("b2", k=3), independent_review=True)
        self.assertFalse(d["emit_claim"])
        self.assertTrue(any("token" in r for r in d["withheld_reasons"]))

    def test_wall_p95_over_one_withholds(self):
        d = G.evaluate_claim_gate(_good("b1", wall_p95_ratio_ci=(0.9, 1.05)),
                                  _good("b2", k=3), independent_review=True)
        self.assertFalse(d["emit_claim"])
        self.assertTrue(any("wall" in r for r in d["withheld_reasons"]))
        self.assertIn("WITHHELD", d["summary"])

    def test_resolution_floor_absolute(self):
        # 2/10 = 0.2 < 0.25 absolute floor
        d = G.evaluate_claim_gate(_good("b1", resolved_c=2, resolved_a=9),
                                  _good("b2", k=3), independent_review=True)
        self.assertFalse(d["emit_claim"])
        self.assertTrue(any("resolution" in r for r in d["withheld_reasons"]))

    def test_resolution_floor_parity(self):
        # C=6/10=0.6, A=10/10=1.0, floor=0.7 -> 0.6<0.7 fails parity
        d = G.evaluate_claim_gate(_good("b1", resolved_c=6, resolved_a=10),
                                  _good("b2", k=3), independent_review=True)
        self.assertFalse(d["emit_claim"])

    def test_k_below_10_withholds_on_b1_only(self):
        # B1 requires K>=10; B2 does not
        d = G.evaluate_claim_gate(_good("b1", k=3), _good("b2", k=3), independent_review=True)
        self.assertFalse(d["emit_claim"])
        self.assertTrue(any("k_adequate" in r for r in d["withheld_reasons"]))

    def test_escalation_dominated_withholds(self):
        d = G.evaluate_claim_gate(_good("b1", escalation_excluded_frac=0.5),
                                  _good("b2", k=3), independent_review=True)
        self.assertFalse(d["emit_claim"])
        self.assertTrue(any("escalation" in r for r in d["withheld_reasons"]))

    def test_n_not_1_withholds(self):
        d = G.evaluate_claim_gate(_good("b1", n_anchor=3), _good("b2", k=3),
                                  independent_review=True)
        self.assertFalse(d["emit_claim"])

    def test_b2_failure_withholds_even_if_b1_passes(self):
        d = G.evaluate_claim_gate(_good("b1"), _good("b2", k=3, token_reduction_ci=(-0.1, 0.1)),
                                  independent_review=True)
        self.assertFalse(d["emit_claim"])
        self.assertTrue(any(r.startswith("B2") for r in d["withheld_reasons"]))

    def test_missing_independent_review_withholds(self):
        d = G.evaluate_claim_gate(_good("b1"), _good("b2", k=3), independent_review=False)
        self.assertFalse(d["emit_claim"])
        self.assertTrue(any("independent reviewer" in r for r in d["withheld_reasons"]))


class TestHonestSummaryAlwaysPresent(unittest.TestCase):
    def test_summary_present_when_withheld(self):
        d = G.evaluate_claim_gate(_good("b1", wall_p95_ratio_ci=(1.1, 1.3)),
                                  _good("b2", k=3), independent_review=True)
        self.assertIn("WITHHELD", d["summary"])
        self.assertIn("%", d["summary"])  # still reports the honest number

    def test_zero_total_fails_closed(self):
        d = G.evaluate_claim_gate(_good("b1", resolved_c=0, resolved_a=0, total=0),
                                  _good("b2", k=3), independent_review=True)
        self.assertFalse(d["emit_claim"])


if __name__ == "__main__":
    unittest.main()
