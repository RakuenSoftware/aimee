#!/usr/bin/env python3
"""Tests for benchmarks/compare_targets.py cross-target comparison report."""

from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from benchmarks.compare_targets import (
    ProvenanceError,
    build_comparison,
    build_group,
    render_markdown,
)
from benchmarks.verify_scores import verify_file


def _row(system: str, qid: str, verdict: str, subset: str) -> dict:
    return {
        "system": system,
        "track": "direct",
        "git_commit": "abc1234",
        "question_id": qid,
        "question": "q?",
        "gold_answer": "a",
        "verdict": verdict,
        "retrieval_latency_s": 0.01,
        "retrieved_ids": [],
        "citations": [],
        "subset": subset,
    }


def _payload(system: str, verdicts: list[str], *, judge="frontier", dhash="hash-A") -> dict:
    rows = [_row(system, f"q{i}", v, "single-session") for i, v in enumerate(verdicts)]
    correct = sum(1 for v in verdicts if v == "CORRECT")
    return {
        "dataset": "msc",
        "track": "direct",
        "judge_profile": judge,
        "dataset_hash": dhash,
        "summary": {"overall_accuracy": correct / len(verdicts)},
        "results": rows,
    }


class CompareTargetsTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _file(self, name: str, payload: dict) -> Path:
        path = self.dir / name
        path.write_text(json.dumps(payload))
        return path

    def test_verifier_preserves_undefined_derived_metrics(self) -> None:
        payload = _payload("aimee", ["CORRECT"])
        payload["summary"]["derived"] = {
            "factoid_recall": 1.0,
            "abstention_precision": None,
            "false_abstention_rate": 0.0,
        }
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            report = verify_file(self._file("null-metric.json", payload))
        self.assertIn("factoid_recall=1.000", output.getvalue())
        self.assertIn("abstention_precision=undefined", output.getvalue())
        self.assertIn("false_abstention_rate=0.000", output.getvalue())
        self.assertIsNone(report["payload"]["summary"]["derived"]["abstention_precision"])

    def test_groups_targets_and_sorts_by_accuracy(self) -> None:
        f_lo = self._file("msc_model_only.json", _payload("model_only", ["WRONG", "WRONG", "CORRECT"]))
        f_hi = self._file("msc_aimee.json", _payload("aimee", ["CORRECT", "CORRECT", "WRONG"]))
        comparison = build_comparison([f_lo, f_hi])
        self.assertEqual(len(comparison["groups"]), 1)
        group = comparison["groups"][0]
        self.assertEqual(group["dataset"], "msc")
        systems = [t["system"] for t in group["targets"]]
        # aimee (0.667) must sort ahead of model_only (0.333)
        self.assertEqual(systems[0], "aimee")
        self.assertAlmostEqual(group["targets"][0]["overall_accuracy"], 2 / 3, places=3)

    def test_refuses_mixed_judge_profile(self) -> None:
        a = verify_file(self._file("a.json", _payload("aimee", ["CORRECT"], judge="frontier")))
        b = verify_file(self._file("b.json", _payload("model_only", ["CORRECT"], judge="small")))
        with self.assertRaises(ProvenanceError):
            build_group([a, b])

    def test_refuses_mixed_dataset_hash(self) -> None:
        a = verify_file(self._file("a.json", _payload("aimee", ["CORRECT"], dhash="hash-A")))
        b = verify_file(self._file("b.json", _payload("model_only", ["CORRECT"], dhash="hash-B")))
        with self.assertRaises(ProvenanceError):
            build_group([a, b])

    def test_build_comparison_records_provenance_error(self) -> None:
        f_a = self._file("a.json", _payload("aimee", ["CORRECT"], judge="frontier"))
        f_b = self._file("b.json", _payload("model_only", ["CORRECT"], judge="small"))
        comparison = build_comparison([f_a, f_b])
        self.assertEqual(comparison["groups"], [])
        self.assertEqual(len(comparison["provenance_errors"]), 1)

    def test_invalid_file_is_skipped_not_fatal(self) -> None:
        # A non-schema-valid result file (e.g. a retrieval-only adapter direct
        # report) must be skipped, not crash the whole comparison.
        good = self._file("good.json", _payload("aimee", ["CORRECT"]))
        bad = self._file("bad.json", {"dataset": "msc", "track": "direct",
                                      "results": [{"id": "x", "answer": "y"}]})
        comparison = build_comparison([good, bad])
        self.assertEqual(len(comparison["skipped_files"]), 1)
        self.assertIn("bad.json", comparison["skipped_files"][0]["file"])
        # the good file still produced a group
        self.assertEqual(len(comparison["groups"]), 1)
        self.assertIn("Skipped (not schema-valid", render_markdown(comparison))

    def test_render_markdown_has_table(self) -> None:
        f_a = self._file("a.json", _payload("aimee", ["CORRECT", "WRONG"]))
        f_b = self._file("b.json", _payload("model_only", ["WRONG", "WRONG"]))
        md = render_markdown(build_comparison([f_a, f_b]))
        self.assertIn("# Cross-target benchmark comparison", md)
        self.assertIn("msc", md)
        self.assertIn("aimee", md)
        self.assertIn("**overall**", md)


if __name__ == "__main__":
    unittest.main()
