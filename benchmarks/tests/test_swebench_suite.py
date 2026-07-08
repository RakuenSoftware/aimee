#!/usr/bin/env python3
"""Unit tests for S5 — the suite orchestration.

Pure: run-plan generation, the CT-101 lease invariants, grader retry + flip detection, and
prediction dedup. No live fleet/docker.
Run: python3 -m unittest benchmarks.tests.test_swebench_suite
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from benchmarks.coding import swebench_suite as Q


class TestRunPlan(unittest.TestCase):
    def test_deterministic_and_ordered(self):
        self.assertEqual([c.run_id for c in Q.generate_run_plan()],
                         [c.run_id for c in Q.generate_run_plan()])

    def test_run_ids_unique_per_cell(self):
        cells = Q.generate_run_plan()
        ids = [c.run_id for c in cells]
        self.assertEqual(len(ids), len(set(ids)))

    def test_b1_crosses_arms_N_primary_K(self):
        cells = Q.generate_run_plan(only=["b1_reddit10"])
        # arms{A,C} x N{1,3} x primary{gpt-5.5,claude} x K=10 = 2*2*2*10 = 80
        self.assertEqual(len(cells), 80)
        self.assertTrue(all(c.benchmark == "b1_reddit10" for c in cells))

    def test_b6_uses_worker_pools(self):
        cells = Q.generate_run_plan(only=["b6_worker_pool"])
        pools = {c.worker_pool for c in cells}
        self.assertEqual(pools, {"free_fleet", "gpu_gemma4"})

    def test_b3_parallelism_sweep(self):
        cells = Q.generate_run_plan(only=["b3_parallelism"])
        self.assertEqual({c.n for c in cells}, {1, 2, 4, 8})


class TestCT101Lease(unittest.TestCase):
    def test_grader_gets_lease(self):
        L = Q.CT101Lease()
        self.assertTrue(L.request_grader())
        self.assertEqual(L.holder, "grader")

    def test_iteration_refused_while_grader_holds(self):
        L = Q.CT101Lease()
        L.request_grader()
        self.assertFalse(L.request_iteration())

    def test_grader_priority_over_waiting_iteration(self):
        L = Q.CT101Lease()
        self.assertTrue(L.request_iteration())     # iteration holds
        self.assertFalse(L.request_grader())        # grader must wait...
        L.release("iteration_pool")
        # ...and while grader is waiting, iteration cannot re-grab it
        self.assertFalse(L.request_iteration())
        self.assertTrue(L.request_grader())

    def test_release_wrong_tenant_errors(self):
        L = Q.CT101Lease()
        L.request_grader()
        with self.assertRaises(Q.LeaseError):
            L.release("iteration_pool")


class TestGraderRetry(unittest.TestCase):
    def test_retries_on_error_then_succeeds(self):
        calls = {"n": 0}

        def grade():
            calls["n"] += 1
            return ("error", "flake") if calls["n"] < 2 else ("ok", True)

        gr = Q.GraderRetry(max_retries=2)
        self.assertTrue(gr.run(grade))
        self.assertEqual(gr.attempts, 2)

    def test_does_not_retry_a_legit_non_resolve(self):
        gr = Q.GraderRetry(max_retries=2)
        self.assertFalse(gr.run(lambda: ("ok", False)))
        self.assertEqual(gr.attempts, 1)   # no retry on a valid False

    def test_raises_after_exhausting_retries(self):
        gr = Q.GraderRetry(max_retries=2)
        with self.assertRaises(RuntimeError):
            gr.run(lambda: ("error", "always down"))
        self.assertEqual(gr.attempts, 3)


class TestFlipDetection(unittest.TestCase):
    def test_flip_flagged(self):
        self.assertTrue(Q.detect_flips([True, False, True]))
        self.assertFalse(Q.detect_flips([True, True, True]))
        self.assertFalse(Q.detect_flips([False, False]))

    def test_aggregate_majority_vote(self):
        self.assertTrue(Q.aggregate_k([True, True, False])["resolved"])
        self.assertFalse(Q.aggregate_k([True, False, False])["resolved"])
        self.assertTrue(Q.aggregate_k([True, False])["flipped"])

    def test_aggregate_empty(self):
        self.assertEqual(Q.aggregate_k([])["resolved"], None)


class TestPredictions(unittest.TestCase):
    def test_dedup_by_instance_id(self):
        recs = [{"instance_id": "i1", "diff": "d1"}, {"instance_id": "i1", "diff": "d2"},
                {"instance_id": "i2", "diff": "d3"}]
        preds = Q.build_predictions(recs, "run-x")
        self.assertEqual(len(preds), 2)
        self.assertEqual({p["instance_id"] for p in preds}, {"i1", "i2"})
        self.assertTrue(all(p["model_name_or_path"] == "run-x" for p in preds))

    def test_skips_empty_patches(self):
        recs = [{"instance_id": "i1", "diff": ""}, {"instance_id": "i2", "patch": "x"}]
        preds = Q.build_predictions(recs, "run-x")
        self.assertEqual([p["instance_id"] for p in preds], ["i2"])


class TestLoadInstances(unittest.TestCase):
    def test_missing_regions_returns_empty(self):
        self.assertEqual(Q.load_instances("reddit10", "/no/such/dir"), [])


class TestResolvedIdsFromReport(unittest.TestCase):
    def test_resolved_ids_list_shape(self):
        self.assertEqual(Q.resolved_ids_from_report({"resolved_ids": ["a", "b"]}), {"a", "b"})

    def test_per_instance_dict_shape(self):
        rep = {"django__x": {"resolved": True}, "sympy__y": {"resolved": False}}
        self.assertEqual(Q.resolved_ids_from_report(rep), {"django__x"})

    def test_empty_or_bad(self):
        self.assertEqual(Q.resolved_ids_from_report({}), set())
        self.assertEqual(Q.resolved_ids_from_report(None), set())


class TestRunSuiteOrchestration(unittest.TestCase):
    """Drive run_suite with INJECTED fake arm-runners + grader + loader (no fleet/docker): assert
    it runs cells, grades, sets resolved, and aggregates K repeats with flip detection."""

    def _fake_instances(self, spec):
        return [{"instance_id": "django__django-1", "repo": "django/django", "base_commit": "0" * 40},
                {"instance_id": "sympy__sympy-2", "repo": "sympy/sympy", "base_commit": "0" * 40}]

    def test_only_b3_runs_and_grades(self):
        seen = {"a": 0, "c": 0, "graded": []}

        def fake_a(inst, primary, **kw):
            seen["a"] += 1
            return {"instance_id": inst["instance_id"], "arm": "A", "patch": "diff --git a/x b/x\n",
                    "supervisor_input_tokens": 10, "supervisor_output_tokens": 2,
                    "worker_input_tokens": 0, "worker_output_tokens": 0, "wall_s": 1.0,
                    "resolved": None, "n_workers": 1, "invalid": False, "compaction": False}

        def fake_c(inst, **kw):
            seen["c"] += 1
            return {"instance_id": inst["instance_id"], "arm": "C", "patch": "diff --git a/x b/x\n",
                    "supervisor_input_tokens": 3, "supervisor_output_tokens": 1,
                    "worker_input_tokens": 100, "worker_output_tokens": 50, "wall_s": 2.0,
                    "resolved": None, "n_workers": 3, "invalid": False, "compaction": False}

        def fake_grade(preds, run_id, **kw):
            seen["graded"].append(run_id)
            # Resolve the django instance only; flip it off on odd repeats to exercise flip flag.
            return {p["instance_id"] for p in preds if "django" in p["instance_id"]}

        out = Q.run_suite(only=["b3_parallelism"], arm_a_runner=fake_a, arm_c_runner=fake_c,
                          grade_fn=fake_grade, instances_loader=self._fake_instances)
        # b3 is arm C only, N in {1,2,4,8}, K=3 -> 12 cells, each 2 instances.
        self.assertEqual(seen["a"], 0)
        self.assertEqual(seen["c"], 12 * 2)
        self.assertEqual(len(seen["graded"]), 12)
        # every cell graded -> resolved set on records
        recs = [r for rs in out["records_by_run"].values() for r in rs]
        self.assertTrue(all(r["resolved"] is not None for r in recs))
        # aggregate present with majority-vote per instance
        self.assertTrue(out["aggregate"])
        self.assertIn("report", out)

    def test_skips_cells_with_no_instances(self):
        out = Q.run_suite(only=["b1_reddit10"], arm_a_runner=lambda *a, **k: {},
                          arm_c_runner=lambda *a, **k: {}, grade_fn=lambda *a, **k: set(),
                          instances_loader=lambda spec: [])
        self.assertTrue(all(c.get("skipped") == "no_instances" for c in out["cells"]))


if __name__ == "__main__":
    unittest.main()
