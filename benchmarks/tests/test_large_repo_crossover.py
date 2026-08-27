import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from benchmarks.roi.large_repo_crossover import (
    ProgressController,
    bounded_output,
    command_allowed,
    diff_metrics,
    load_tasks,
    prepare_diff,
    summarize,
    tool_result_usable,
)


class LargeRepoCrossoverTests(unittest.TestCase):
    def test_manifest_selection_is_ordered_and_rejects_unknown_tasks(self):
        with tempfile.TemporaryDirectory() as raw:
            manifest = Path(raw) / "tasks.json"
            manifest.write_text(json.dumps({
                "tasks": [{
                    "task_id": "one", "fix_commit": "abc", "prompt": "fix it",
                    "languages": ["C"], "selection_stratum": "test",
                    "hidden_test_files": ["tests/test_one.c"],
                    "grader_commands": ["make test"],
                }],
            }))
            _, tasks = load_tasks(manifest, ["one"])
            self.assertEqual([task.task_id for task in tasks], ["one"])
            with self.assertRaises(SystemExit):
                load_tasks(manifest, ["missing"])

    def test_command_allowlist_checks_every_pipeline_or_chain_head(self):
        self.assertTrue(command_allowed("make -C src test && src/unit-test-example"))
        self.assertTrue(command_allowed("rg needle src | head -20"))
        self.assertFalse(command_allowed("make test && curl https://example.invalid"))
        self.assertFalse(command_allowed("git log --all"))
        self.assertFalse(command_allowed("make $(dangerous)"))

    def test_bounded_output_retains_both_ends(self):
        visible, truncated = bounded_output("a" * 100 + "z" * 100, 80)
        self.assertTrue(truncated)
        self.assertTrue(visible.startswith("a"))
        self.assertTrue(visible.endswith("z" * 40))

    def test_diff_metrics_include_untracked_tests_after_intent_to_add(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.email", "roi@example.invalid"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.name", "ROI Test"], cwd=root, check=True)
            (root / "README.md").write_text("base\n")
            subprocess.run(["git", "add", "README.md"], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "base"], cwd=root, check=True)
            (root / "tests").mkdir()
            (root / "tests" / "test_regression.c").write_text("new test\n")
            prepare_diff(root)
            metrics = diff_metrics(root)
            self.assertEqual(metrics["test_files"], ["tests/test_regression.c"])
            self.assertEqual(metrics["added"], 1)

    def test_summary_identifies_context_and_test_crossovers(self):
        def cell(condition, resolved, reason, authored, sensitive, tokens):
            return {
                "task_id": "large", "repeat": 0, "condition": condition,
                "resolved": resolved, "terminal_reason": reason,
                "diff": {"test_files": ["tests/test.c"] if authored else []},
                "authored_test_sensitivity": {"regression_sensitive": sensitive},
                "usage": {"input_tokens": tokens, "output_tokens": 10,
                          "total_tokens": tokens + 10},
            }

        result = summarize([
            cell("off", False, "context_limit", False, False, 1000),
            cell("aimee", True, "submitted", True, True, 700),
        ])
        self.assertEqual(result["context_capacity_crossovers"][0]["task_id"], "large")
        self.assertEqual(result["by_condition"]["aimee"]["regression_sensitive_test_cells"], 1)
        self.assertEqual(result["by_condition"]["off"]["resolved"], 0)

    def test_progress_controller_detects_nonconsecutive_overlap(self):
        guard = ProgressController()
        self.assertEqual(guard.observe(
            "read_file", {"path": "src/a.c", "start": 1, "end": 100},
            usable=True, mutation=False,
        )["action"], "none")
        guard.observe("read_file", {"path": "src/b.c", "start": 1, "end": 100},
                      usable=True, mutation=False)
        self.assertEqual(guard.observe(
            "read_file", {"path": "src/a.c", "start": 90, "end": 150},
            usable=True, mutation=False,
        )["action"], "none")
        event = guard.observe(
            "read_file", {"path": "src/a.c", "start": 20, "end": 40},
            usable=True, mutation=False,
        )
        self.assertEqual(event["action"], "checkpoint")
        self.assertEqual(event["duplicate_hits"], 2)

    def test_progress_controller_resets_only_on_successful_mutation(self):
        guard = ProgressController()
        for _ in range(4):
            guard.observe("search", {"query": "same"}, usable=True, mutation=False)
        before = guard.calls_since_mutation
        guard.observe("apply_patch", {"patch": "bad"}, usable=False, mutation=True)
        self.assertEqual(guard.calls_since_mutation, before)
        event = guard.observe("apply_patch", {"patch": "good"}, usable=True, mutation=True)
        self.assertEqual(event["action"], "mutation_reset")
        self.assertEqual(event["calls_since_mutation"], 0)
        self.assertEqual(event["successful_mutations"], 1)

    def test_tool_result_usability_distinguishes_failed_patch(self):
        self.assertTrue(tool_result_usable("apply_patch", "[exit_code=0]\n[tool_output_ref=x]"))
        self.assertFalse(tool_result_usable("apply_patch", "patch failed\n[exit_code=1]\n[tool_output_ref=x]"))
        self.assertFalse(tool_result_usable("read_file", "read_file: no such file: missing"))


if __name__ == "__main__":
    unittest.main()
