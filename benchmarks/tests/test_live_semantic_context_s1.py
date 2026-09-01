from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / "benchmarks" / "live-semantic-context"


def load_runner():
    spec = importlib.util.spec_from_file_location("s1_paired_runner", BASE / "run_s1_paired_study.py")
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_summarizer():
    spec = importlib.util.spec_from_file_location(
        "s1_result_summarizer", BASE / "summarize_s1_results.py"
    )
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_candidate_probe():
    spec = importlib.util.spec_from_file_location(
        "s1_candidate_probe", BASE / "run_s1_candidate_probe.py"
    )
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class LiveSemanticContextS1Test(unittest.TestCase):
    def test_contract_and_instrumentation_pins(self) -> None:
        subprocess.run(
            ["python3", str(BASE / "validate_s1_contract.py")], cwd=ROOT, check=True,
            capture_output=True, text=True,
        )

    def test_per_task_arm_order_is_deterministic(self) -> None:
        runner = load_runner()
        manifest = json.loads((BASE / "s1-task-manifest.json").read_text())
        task = manifest["tasks"][0]
        first = [arm for _, arm in runner.arm_plan([task], 41904)]
        second = [arm for _, arm in runner.arm_plan([task], 41904)]
        self.assertEqual(first, second)
        self.assertEqual(set(first), set(runner.ARMS))

    def test_production_mcp_surface_is_exact_and_reads_checked_files(self) -> None:
        workspace = BASE / "fixtures" / "go"
        with tempfile.TemporaryDirectory(prefix="aimee-s1-mcp-test-") as temporary:
            log_path = Path(temporary) / "tools.jsonl"
            env = {
                **os.environ,
                "S1_WORKSPACE": str(workspace),
                "S1_ARM": "production",
                "S1_TOOL_LOG": str(log_path),
            }
            requests = [
                {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
                {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}},
                {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {
                    "name": "file_read", "arguments": {"path": "main.go"},
                }},
            ]
            completed = subprocess.run(
                ["python3", str(BASE / "s1_mcp_server.py")],
                input="".join(json.dumps(item) + "\n" for item in requests),
                env=env, text=True, capture_output=True, timeout=10, check=True,
            )
            responses = [json.loads(line) for line in completed.stdout.splitlines()]
            tools = responses[1]["result"]["tools"]
            self.assertEqual([item["name"] for item in tools], [
                "local_text_search", "local_structure_search", "file_read",
                "code_span_get", "file_edit", "test_execution",
            ])
            result = responses[2]["result"]["structuredContent"]
            self.assertEqual(result["status"], "ok")
            self.assertIn("func add", result["content"])
            records = [json.loads(line) for line in log_path.read_text().splitlines()]
            self.assertEqual([record["tool"] for record in records], ["file_read"])

    def test_location_surface_translates_checked_absolute_paths(self) -> None:
        workspace = (BASE / "fixtures" / "go").resolve()
        with tempfile.TemporaryDirectory(prefix="aimee-s1-location-test-") as temporary:
            temporary_path = Path(temporary)
            bridge = temporary_path / "bridge.py"
            bridge.write_text(
                "#!/usr/bin/env python3\n"
                "import json,sys\n"
                "for line in sys.stdin:\n"
                " request=json.loads(line)\n"
                " anchor=request['anchors'][0]\n"
                " print(json.dumps({'status':'ok','locations':[anchor]}), flush=True)\n"
            )
            bridge.chmod(0o700)
            log_path = temporary_path / "tools.jsonl"
            env = {
                **os.environ,
                "S1_WORKSPACE": str(workspace), "S1_ARM": "location_only",
                "S1_TOOL_LOG": str(log_path), "S1_BRIDGE": str(bridge),
                "S1_PROVIDER_COMMAND": "/bin/true", "S1_PROVIDER_ARG": "-",
                "S1_PROVIDER_EXTENSION": ".go",
            }
            request = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {
                "name": "lsp_definition", "arguments": {
                    "workspace": str(workspace), "file": str(workspace / "main.go"),
                    "line": 8, "column": 9,
                },
            }}
            completed = subprocess.run(
                ["python3", str(BASE / "s1_mcp_server.py")], input=json.dumps(request) + "\n",
                env=env, text=True, capture_output=True, timeout=10, check=True,
            )
            result = json.loads(completed.stdout)["result"]["structuredContent"]
            self.assertEqual(result["status"], "ok")
            self.assertEqual(result["locations"][0]["file"], "main.go")

    def test_grade_accepts_checked_authority_without_punctuation(self) -> None:
        runner = load_runner()
        task = {
            "oracle": {"targets": [{"file": "main.go", "line": 3, "symbol": "add"}]},
        }
        result = runner.grade(task, "batched_context", {
            "answer_status": "ok",
            "authority": "local_checkout (worktree local:x)",
            "targets": [{"file": "main.go", "line": 3, "symbol": "add"}],
        })
        self.assertTrue(result["task_success"])
        self.assertTrue(result["exact_target_correctness"])

    def test_grade_requires_lsp_arms_to_preserve_injected_typed_failure(self) -> None:
        runner = load_runner()
        task = {
            "oracle": {"targets": [{"file": "main.go", "line": 3, "symbol": "add"}]},
            "failure_overlay": {"expected_status": "unavailable"},
        }
        result = runner.grade(task, "batched_context", {
            "answer_status": "unavailable", "authority": "local_checkout:.", "targets": [],
        }, failure_active=True)
        self.assertTrue(result["task_success"])
        self.assertTrue(result["typed_failure_preserved"])
        self.assertIsNone(result["exact_target_correctness"])
        self.assertEqual(result["false_current_results"], 0)

        false_current = runner.grade(task, "location_only", {
            "answer_status": "ok", "authority": "local_checkout:.",
            "targets": [{"file": "main.go", "line": 3, "symbol": "add"}],
        }, failure_active=True)
        self.assertFalse(false_current["task_success"])
        self.assertEqual(false_current["false_current_results"], 1)

        production = runner.grade(task, "production", {
            "answer_status": "ok", "authority": "local_checkout:.",
            "targets": [{"file": "main.go", "line": 3, "symbol": "add"}],
        })
        self.assertTrue(production["task_success"])
        self.assertEqual(production["expected_behavior"], "exact_targets")

    def test_failure_suite_is_separate_from_paired_value_plan(self) -> None:
        runner = load_runner()
        manifest = json.loads((BASE / "s1-task-manifest.json").read_text())
        primary = runner.arm_plan(manifest["tasks"], 41904)
        failure = [
            cell for cell in runner.arm_plan(
                [task for task in manifest["tasks"] if task.get("failure_overlay")], 41904
            ) if cell[1] != "production"
        ]
        self.assertEqual(len(primary), 135)
        self.assertEqual(len(failure), 12)
        self.assertTrue(all(task.get("failure_overlay") for task, _ in failure))

    def test_summary_uses_complete_pairs_and_paired_intervals(self) -> None:
        summarizer = load_summarizer()
        contract = json.loads((BASE / "s1-experiment-contract.json").read_text())
        rows = []
        for task_id, semantic in (("s01", True), ("c01", False)):
            for arm, success, calls, wall in (
                ("production", True, 3, 10.0),
                ("location_only", True, 2, 9.0),
                ("batched_context", True, 1, 5.0),
            ):
                rows.append({
                    "task_id": task_id, "arm": arm, "semantic_eligible": semantic,
                    "infrastructure_failure": False, "cell_eligible": True,
                    "grade": {
                        "task_success": success, "exact_target_correctness": True,
                        "authority_cited": True, "false_empty_count": 0,
                        "stale_result_count": 0, "false_current_results": 0,
                    },
                    "wall_seconds": wall, "agent_turns": 1, "tool_calls": calls,
                    "usage": {"input_tokens": 1000 if arm == "production" else 500,
                              "output_tokens": 10},
                    "measurement": {
                        "preparation_bytes": 10, "tool_input_bytes": 20,
                        "tool_output_bytes": 30,
                    },
                    "candidate_used_before_decisive_edit": arm == "batched_context",
                })
        summary = summarizer.summarize({
            "run_id": "test", "study_kind": "paired_value",
            "claim_status": "calibration_only", "cells": rows,
        }, contract)
        semantic = summary["paired"]["semantic"]
        self.assertEqual(semantic["complete_pairs"], 1)
        self.assertEqual(semantic["median_tool_call_reduction"]["estimate"], 2 / 3)
        self.assertEqual(semantic["median_wall_time_reduction"]["estimate"], 0.5)
        self.assertEqual(summary["candidate_adoption"], {"rate": 1.0, "denominator": 1})
        self.assertEqual(summary["promotion_decision"], "incomplete")

    def test_cold_start_aggregate_keeps_full_denominator_and_reference_quality(self) -> None:
        probe = load_candidate_probe()
        trials = []
        for _ in range(20):
            trials.append({
                "available": True,
                "observation": {
                    "timed_out": False, "exit_code": 0,
                    "peak_process_tree_count": 3, "peak_process_tree_rss_kib": 4096,
                    "probe": {
                        "synchronized": True, "document_version": 1,
                        "provider_generation": 1, "definition_matched": True,
                        "reference_count": 3, "active_servers_after_query": 1,
                    },
                },
            })
        aggregate = probe.aggregate_provider("gopls", trials)
        self.assertEqual(aggregate["cold_start_attempts"], 20)
        self.assertEqual(aggregate["cold_start_success_rate"], 1.0)
        self.assertEqual(aggregate["reference_recall"], 1.0)
        self.assertEqual(aggregate["reference_false_positive_rate"], 0.0)
        self.assertEqual(aggregate["peak_process_tree_count"], 3)


if __name__ == "__main__":
    unittest.main()
