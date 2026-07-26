"""Contract tests for the frozen 10k Gemma baseline and resumable runners."""

from __future__ import annotations

import contextlib
import importlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
MODULES = ROOT / "benchmarks/gemma4_baseline"
sys.path.insert(0, str(MODULES))

builder = importlib.import_module("build_254_fixtures")
reranker = importlib.import_module("run_reranking_ab")
synthesis = importlib.import_module("run_synthesis_ab")
sweep = importlib.import_module("run_254_sweep")
validator = importlib.import_module("validate_fixtures")


class GemmaBaselineContractTests(unittest.TestCase):
    def test_sweep_requires_both_ettin_execution_profiles(self) -> None:
        self.assertEqual(sweep.ETTIN_CONTROLS, (
            {"label": "ettin68m", "tier": "cpu", "ngl": "0", "execution": "cpu"},
            {"label": "ettin400m", "tier": "mid", "ngl": "99", "execution": "gpu"},
        ))
        self.assertEqual(sweep.ETTIN_LOAD_PROFILE, {
            "workers": 8,
            "pairs_per_request": 4,
            "parallel_slots": 32,
            "context_tokens": 65536,
            "logical_batch_tokens": 8192,
            "physical_batch_tokens": 2048,
        })

    def test_frozen_bundle_is_exact_and_paired(self) -> None:
        result = validator.validate(ROOT / "benchmarks/fixtures/gemma4-unified/ab-v1")
        self.assertEqual(result["case_rows_per_view"], 10_000)
        self.assertEqual(result["shared_case_ids"], 10_000)
        self.assertEqual(set(result["baseline_model_views"]), {
            "gemma4_e2b", "gemma4_e4b", "gemma4_12b", "gemma4_26b_a4b",
            "gemma4_31b", "qwen36_35b_a3b", "ettin68m", "ettin400m",
        })

    def test_serialized_secret_scan_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.jsonl"
            path.write_text('{"value":"api_key=123456789abcdef"}\n', encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "secret/de-identification scan failed"):
                builder.assert_no_obvious_secrets([path])

    def test_reranker_bound_is_exact_and_keeps_head_and_tail(self) -> None:
        value = "A" * 900 + "B" * 900
        bounded = reranker.bounded_text(value, 1_024)
        self.assertEqual(len(bounded), 1_024)
        self.assertTrue(bounded.startswith("A"))
        self.assertTrue(bounded.endswith("B"))
        self.assertIn("[...truncated...]", bounded)

    def test_reranker_resume_retries_failed_case(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle, output = root / "bundle", root / "results"
            bundle.mkdir()
            output.mkdir()
            (bundle / "manifest.json").write_text('{"suite":"test"}\n', encoding="utf-8")
            (bundle / "corpus.jsonl").write_text(
                json.dumps({"doc_id": "doc", "content": "candidate"}) + "\n", encoding="utf-8"
            )
            case = {
                "case_id": "case", "query": "query", "candidate_doc_ids": ["doc"],
                "relevance": {"doc": 1},
            }
            (bundle / "reranking.jsonl").write_text(json.dumps(case) + "\n", encoding="utf-8")
            raw = output / "raw_reranking_resume.jsonl"
            raw.write_text(json.dumps({"case_id": "case", "ok": False}) + "\n", encoding="utf-8")
            successful = {
                "case_id": "case", "ok": True, "attempts": 1, "latency_s": 0.1,
                "request_chunks": 1, "scores": [1.0], "ranked_doc_ids": ["doc"],
                "metrics": reranker.ranking_metrics(["doc"], {"doc": 1}),
            }
            argv = [
                "run_reranking_ab.py", "--endpoint", "http://unused", "--label", "resume",
                "--bundle", str(bundle), "--output-dir", str(output), "--workers", "8",
            ]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(reranker, "call", return_value=successful) as call:
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(reranker.main(), 0)
            call.assert_called_once()
            self.assertEqual(json.loads(raw.read_text(encoding="utf-8").splitlines()[-1])["ok"], True)
            summary = json.loads((output / "summary_reranking_resume.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["load_profile"]["workers"], 8)
            self.assertEqual(summary["load_profile"]["maximum_inflight_pairs"], 32)

    def test_synthesis_resume_does_not_repeat_completed_case(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle, output = root / "bundle", root / "results"
            bundle.mkdir()
            (bundle / "manifest.json").write_text('{"suite":"test"}\n', encoding="utf-8")
            (bundle / "corpus.jsonl").write_text(
                json.dumps({"doc_id": "doc", "content": "source"}) + "\n", encoding="utf-8"
            )
            case = {
                "case_id": "case", "task": "entity", "instruction": "extract",
                "source_doc_id": "doc", "expected": {"name": "x", "entity_kind": "test", "context": "source"},
            }
            (bundle / "synthesis.jsonl").write_text(json.dumps(case) + "\n", encoding="utf-8")
            result = {
                "case_id": "case", "task": "entity", "ok": True, "attempts": 1, "latency_s": 0.1,
                "raw_parse": True, "empty": False, "truncated": False,
                "usage": {"completion_tokens": 1, "prompt_tokens": 1}, "timings": {},
                "metrics": {"schema_valid": True, "required_field_recall": 1.0, "content_f1": 1.0},
            }
            argv = [
                "run_synthesis_ab.py", "--endpoint", "http://unused", "--model", "test", "--label", "resume",
                "--bundle", str(bundle), "--output-dir", str(output),
            ]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(synthesis, "call", return_value=result), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(synthesis.main(), 0)
            with mock.patch.object(sys, "argv", argv), mock.patch.object(synthesis, "call", side_effect=AssertionError("repeated")), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(synthesis.main(), 0)


if __name__ == "__main__":
    unittest.main()
