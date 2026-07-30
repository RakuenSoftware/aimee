import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

MODULE = Path(__file__).parents[1] / "code-agent-effectiveness" / "e6_evaluate.py"
SPEC = importlib.util.spec_from_file_location("e6_evaluate", MODULE)
e6 = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(e6)


class E6EvaluateTest(unittest.TestCase):
    def evaluate(self, coding):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.json"
            corpus = json.loads(e6.CORPUS_PATH.read_text())
            retrieval = [{"id": row["id"], "score_eligible": True, "expected": row["expected"],
                          "observed": row["expected"], "duplicate": False, "scope_leak": False,
                          "retrieval_latency_s": .1, "packet_tokens": 100} for row in corpus["retrieval_cases"]]
            path.write_text(json.dumps({"retrieval_cells": retrieval, "coding_cells": coding,
                                        "python_edge_precision": 1, "python_edge_recall": 1}))
            return e6.score(path)

    def test_incomplete_arms_cannot_promote(self):
        result = self.evaluate([])
        self.assertTrue(result["retrieval_gate_pass"])
        self.assertFalse(result["paired_agent_gate_pass"])
        self.assertEqual(result["promotion_decision"], "retain-observe")

    def test_complete_efficient_actuated_matrix_promotes(self):
        rows = []
        tasks = [row["id"] for row in json.loads(e6.CORPUS_PATH.read_text())["coding_tasks"]]
        for arm in e6.ARMS:
            for task in tasks:
                rows.append({"arm": arm, "task": task, "score_eligible": True, "task_success": True,
                             "answerable": True, "consumed_before_edit": arm == "on",
                             "uncached_input_tokens": 800 if arm == "on" else 1000,
                             "total_wall_s": 10})
        result = self.evaluate(rows)
        self.assertTrue(result["paired_agent_gate_pass"])
        self.assertEqual(result["promotion_decision"], "promote-on")

    def test_invalid_cells_are_excluded_and_reported(self):
        tasks = [row["id"] for row in json.loads(e6.CORPUS_PATH.read_text())["coding_tasks"]]
        rows = [{"arm": arm, "task": task, "score_eligible": False} for arm in e6.ARMS for task in tasks]
        result = self.evaluate(rows)
        self.assertEqual(result["coding"]["on"]["excluded"], 8)
        self.assertFalse(result["required_arms_complete"])

    def test_partial_retrieval_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.json"
            row = {"id":"r01", "score_eligible":True, "expected":"answer", "observed":"answer",
                   "duplicate":False, "scope_leak":False, "retrieval_latency_s":.1, "packet_tokens":1}
            path.write_text(json.dumps({"retrieval_cells":[row], "coding_cells":[],
                                        "python_edge_precision":1, "python_edge_recall":1}))
            self.assertFalse(e6.score(path)["retrieval_gate_pass"])

    def test_unknown_or_malformed_rows_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.json"
            path.write_text(json.dumps({"retrieval_cells":[{"id":"unknown"}], "coding_cells":[]}))
            with self.assertRaises(ValueError):
                e6.score(path)

    def test_malformed_eligible_coding_metrics_are_rejected(self):
        rows = []
        tasks = [row["id"] for row in json.loads(e6.CORPUS_PATH.read_text())["coding_tasks"]]
        for arm in e6.ARMS:
            for task in tasks:
                rows.append({"arm": arm, "task": task, "score_eligible": True,
                             "task_success": True, "answerable": True,
                             "consumed_before_edit": arm == "on", "uncached_input_tokens": 10,
                             "total_wall_s": 1})
        rows[0]["total_wall_s"] = float("nan")
        with self.assertRaisesRegex(ValueError, "finite"):
            self.evaluate(rows)


if __name__ == "__main__":
    unittest.main()
