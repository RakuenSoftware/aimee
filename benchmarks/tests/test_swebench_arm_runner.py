#!/usr/bin/env python3
"""Unit tests for S2 — the arm-A runner's pure measurement surface.

No live server/docker/network: token totals over synthesized ledgers, wall-clock decomposition,
and record-schema conformance (the record must be consumable by supervised_report.build_report).
Run: python3 -m unittest benchmarks.tests.test_swebench_arm_runner
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from benchmarks.coding import swebench_arm_runner as R
from benchmarks.coding import swebench_transport_verify as V
from benchmarks.coding import supervised_report as REP

PRIMARY = "gpt-5.5"


class TestPrimaryTokenTotals(unittest.TestCase):
    def test_headline_is_uncached_input_plus_output(self):
        rows = [V._row(PRIMARY, prompt=1000, completion=100, cache_read=300),
                V._row(PRIMARY, prompt=500, completion=50, cache_read=100)]
        t = R.primary_token_totals(rows, PRIMARY)
        self.assertEqual(t.input_uncached, 1500 - 400)   # 1100
        self.assertEqual(t.input_cached, 400)
        self.assertEqual(t.output, 150)
        self.assertEqual(t.total_headline, 1100 + 150)
        self.assertEqual(t.turns, 2)

    def test_cached_excluded_from_headline(self):
        # A turn that is ALL cache-read (a re-read) adds nothing to the uncached headline.
        rows = [V._row(PRIMARY, prompt=800, completion=0, cache_read=800)]
        t = R.primary_token_totals(rows, PRIMARY)
        self.assertEqual(t.input_uncached, 0)
        self.assertEqual(t.input_cached, 800)

    def test_worker_rows_excluded_from_primary(self):
        rows = V._fake_rows("pass", PRIMARY, "glm-5.2")
        t = R.primary_token_totals(rows, PRIMARY)
        wi, wo = R.worker_token_totals(rows)
        # worker return of 5000 tokens must land in worker totals, not primary
        self.assertGreaterEqual(wo, V._BIG_WORKER_RETURN_TOKENS)
        self.assertNotIn(V._BIG_WORKER_RETURN_TOKENS, (t.output,))

    def test_thinking_is_none_until_ledger_carries_it(self):
        rows = [V._row(PRIMARY, prompt=10, completion=5)]
        self.assertIsNone(R.primary_token_totals(rows, PRIMARY).thinking)


class TestWallClock(unittest.TestCase):
    def test_total_work_queue_decomposition(self):
        w = R.WallClock(t0=100.0, first_work=103.0, t1=115.0)
        self.assertEqual(w.total_s, 15.0)
        self.assertEqual(w.work_s, 12.0)
        self.assertEqual(w.queue_s, 3.0)

    def test_queue_plus_work_equals_total(self):
        w = R.WallClock(t0=0.0, first_work=2.5, t1=20.0)
        self.assertAlmostEqual(w.queue_s + w.work_s, w.total_s)

    def test_negative_clamped(self):
        w = R.WallClock(t0=10.0, first_work=5.0, t1=8.0)
        self.assertGreaterEqual(w.queue_s, 0.0)
        self.assertGreaterEqual(w.work_s, 0.0)


class TestArmRecordSchema(unittest.TestCase):
    def _rec(self):
        tok = R.PrimaryTokens(input_uncached=1000, input_cached=200, output=100, turns=3)
        wall = R.WallClock(t0=0.0, first_work=1.0, t1=30.0)
        return R.build_arm_record("inst-1", "A", PRIMARY, tokens=tok, worker_in=0, worker_out=0,
                                  wall=wall, resolved=True, patch="diff\n", base_commit="c0ffee",
                                  repo="x/y")

    def test_has_all_report_schema_keys(self):
        rec = self._rec()
        for k in ("instance_id", "arm", "compaction", "supervisor_input_tokens",
                  "supervisor_output_tokens", "worker_input_tokens", "worker_output_tokens",
                  "wall_s", "resolved", "n_workers", "invalid"):
            self.assertIn(k, rec)

    def test_headline_input_is_uncached(self):
        rec = self._rec()
        self.assertEqual(rec["supervisor_input_tokens"], 1000)         # uncached only
        self.assertEqual(rec["supervisor_input_cached_tokens"], 200)

    def test_consumable_by_build_report(self):
        # The whole point: a record produced here must flow through the report unchanged.
        rec = self._rec()
        rep = REP.build_report([rec], compaction=False)
        # wall-clock summary picked the record up (median/p95 computed from wall_s)
        self.assertEqual(rep["wall_clock"]["A"]["n"], 1)
        self.assertEqual(rep["wall_clock"]["A"]["p95_s"], 30.0)
        # resolution picked the graded record up
        self.assertEqual(rep["resolution"]["A"]["resolved"], 1)

    def test_two_wall_clocks_present(self):
        rec = self._rec()
        self.assertEqual(rec["wall_s"], 30.0)
        self.assertEqual(rec["wall_work_s"], 29.0)
        self.assertEqual(rec["wall_queue_s"], 1.0)


class TestFakeMode(unittest.TestCase):
    def test_fake_record_is_schema_valid(self):
        inst = {"instance_id": "inst-0", "repo": "x/y", "base_commit": "0" * 40}
        rec = R._fake_arm_a_record(inst, PRIMARY)
        self.assertEqual(rec["arm"], "A")
        self.assertEqual(rec["instance_id"], "inst-0")
        self.assertIn("patch_fingerprint", rec)


class TestLiveStubHonesty(unittest.TestCase):
    def test_run_arm_a_live_is_marked_stub(self):
        inst = {"instance_id": "i", "repo": "x/y", "base_commit": "0" * 40}
        with self.assertRaises(NotImplementedError):
            R.run_arm_a(inst, PRIMARY, token_db="/x", base_repo="/y",
                        allocator=None, budget=None)


if __name__ == "__main__":
    unittest.main()
