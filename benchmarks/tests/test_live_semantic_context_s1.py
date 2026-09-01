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


if __name__ == "__main__":
    unittest.main()
