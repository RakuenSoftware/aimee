#!/usr/bin/env python3
"""Unit tests for S0 — the transport verification matrix.

Exercises the five ledger-attribution assertions against synthesized token_audit rows
(no live aimee/server). The point is to prove each check CATCHES its regression: for every
assertion there is a fake ledger that breaks exactly that property and must fail.
Run: python3 -m pytest benchmarks/tests/test_swebench_transport_verify.py
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from benchmarks.coding import swebench_transport_verify as V

PRIMARY = "gpt-5.5"
WORKER = "glm-5.2"
BIG = V._BIG_WORKER_RETURN_TOKENS


def _verify(scenario):
    rows = V._fake_rows(scenario, PRIMARY, WORKER)
    return V.verify_rows(rows, PRIMARY, expected_primary_turns=2, big_return_tokens=BIG)


class TestPassScenario(unittest.TestCase):
    def test_pass_satisfies_all_five(self):
        v = _verify("pass")
        self.assertTrue(v["passed"], v)
        for k in ("P1_primary_captured", "P2_worker_attributed", "P3_no_cross_bill",
                  "P4_cache_split", "P5_primary_tools_billed"):
            self.assertTrue(v[k]["ok"], f"{k} should pass: {v[k]['detail']}")


class TestEachAssertionCatchesItsRegression(unittest.TestCase):
    def test_no_primary_breaks_P1(self):
        v = _verify("no_primary")
        self.assertFalse(v["P1_primary_captured"]["ok"])
        self.assertFalse(v["passed"])

    def test_no_worker_breaks_P2(self):
        v = _verify("no_worker")
        self.assertFalse(v["P2_worker_attributed"]["ok"])
        self.assertFalse(v["passed"])

    def test_worker_under_primary_breaks_P2(self):
        # A delegate turn billed under the primary model is cross-attribution.
        v = _verify("worker_under_primary")
        self.assertFalse(v["P2_worker_attributed"]["ok"])

    def test_cross_bill_breaks_P3(self):
        v = _verify("cross_bill")
        self.assertFalse(v["P3_no_cross_bill"]["ok"])
        self.assertFalse(v["passed"])

    def test_tool_bypass_breaks_P5(self):
        v = _verify("tool_bypass")
        self.assertFalse(v["P5_primary_tools_billed"]["ok"])
        self.assertFalse(v["passed"])


class TestPrimaryTokenPolarity(unittest.TestCase):
    def test_primary_rows_are_empty_delegation_only(self):
        rows = V._fake_rows("pass", PRIMARY, WORKER)
        pr = V._primary_rows(rows, PRIMARY)
        self.assertTrue(all(not r["delegation_id"] for r in pr))
        self.assertTrue(all(r["model"] == PRIMARY for r in pr))

    def test_worker_rows_are_nonempty_delegation(self):
        rows = V._fake_rows("pass", PRIMARY, WORKER)
        wr = V._worker_rows(rows)
        self.assertTrue(wr and all(r["delegation_id"] for r in wr))


class TestCacheSplitHeadline(unittest.TestCase):
    def test_uncached_headline_is_prompt_minus_cache_read(self):
        rows = [V._row(PRIMARY, prompt=1000, completion=50, cache_read=300),
                V._row(PRIMARY, prompt=500, completion=20, cache_read=100)]
        ok, detail = V.assert_cache_split(rows, PRIMARY)
        self.assertTrue(ok)
        # prompt 1500 - cache_read 400 = 1100 uncached headline
        self.assertIn("uncached headline=1100", detail)


class TestNoCrossBillBoundary(unittest.TestCase):
    def test_small_primary_prompt_passes(self):
        rows = [V._row(PRIMARY, prompt=800, completion=100, tool_name="read_file")]
        ok, _ = V.assert_no_cross_bill(rows, PRIMARY, BIG)
        self.assertTrue(ok)

    def test_leaked_worker_return_fails(self):
        rows = [V._row(PRIMARY, prompt=V._PRIMARY_PROMPT_CEILING + BIG, completion=100,
                       tool_name="read_file")]
        ok, _ = V.assert_no_cross_bill(rows, PRIMARY, BIG)
        self.assertFalse(ok)


class TestMatrixFailClosed(unittest.TestCase):
    def test_render_marks_sanctioned(self):
        results = {"v1_runs": _verify("pass"), "agent_shell": _verify("cross_bill")}
        out = V.render(results)
        self.assertIn("[PASS] v1_runs", out)
        self.assertIn("[FAIL] agent_shell", out)


if __name__ == "__main__":
    unittest.main()
