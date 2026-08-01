import importlib.util
import base64
import json
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "code-agent-effectiveness" / "checkpoint_runner.py"
SPEC = importlib.util.spec_from_file_location("checkpoint_runner", MODULE_PATH)
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


def make_plan(path: Path, cells: list[dict]) -> Path:
    path.write_text(json.dumps({"pinned_commit": "abc123", "prompt_fixture": "v1", "cells": cells}))
    return path


def output_bytes(path: Path) -> bytes:
    envelope = json.loads(path.read_text())
    assert envelope["encoding"] == "base64"
    return base64.b64decode(envelope["data"])


class CheckpointRunnerTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_failure_is_preserved_and_excluded_from_scoring(self):
        source = make_plan(self.root / "plan.json", [
            {"id": "broken", "command": ["sh", "-c", "printf evidence; exit 9"]}
        ])
        run_dir = self.root / "run"
        self.assertEqual(runner.run(source, run_dir, "after-broken"), 2)
        results = list(run_dir.glob("artifacts/broken/attempt-*/result.json"))
        self.assertEqual(len(results), 1)
        result = json.loads(results[0].read_text())
        self.assertEqual(result["status"], "infrastructure-invalid")
        self.assertFalse(result["score_eligible"])
        self.assertIn(b"evidence", output_bytes(next(run_dir.glob("artifacts/broken/attempt-*/stdout.json"))))
        checkpoint = json.loads((run_dir / "checkpoints/after-broken.json").read_text())
        self.assertNotIn("broken", checkpoint["completed"])

    def test_named_resume_runs_only_unfinished_cells(self):
        marker = self.root / "marker"
        source = make_plan(self.root / "plan.json", [
            {"id": "first", "command": ["sh", "-c", f"printf x >> {marker}"]},
            {"id": "second", "command": ["sh", "-c", "true"]},
        ])
        run_dir = self.root / "run"
        self.assertEqual(runner.run(source, run_dir, "matrix-a"), 0)
        self.assertEqual(runner.run(None, run_dir, "matrix-a"), 0)
        self.assertEqual(marker.read_text(), "x")

    def test_resume_refuses_checkpoint_splicing(self):
        source = make_plan(self.root / "plan.json", [
            {"id": "one", "command": ["sh", "-c", "true"]}
        ])
        run_dir = self.root / "run"
        self.assertEqual(runner.run(source, run_dir, "matrix-a"), 0)
        checkpoint_path = run_dir / "checkpoints/matrix-a.json"
        checkpoint = json.loads(checkpoint_path.read_text())
        checkpoint["run_id"] = "foreign-run"
        checkpoint_path.write_text(json.dumps(checkpoint))
        with self.assertRaisesRegex(ValueError, "splicing"):
            runner.run(None, run_dir, "matrix-a")

    def test_resume_refuses_spliced_result_artifact(self):
        source = make_plan(self.root / "plan.json", [
            {"id": "one", "command": ["sh", "-c", "true"]}
        ])
        run_dir = self.root / "run"
        self.assertEqual(runner.run(source, run_dir, "matrix-a"), 0)
        result_path = next(run_dir.glob("artifacts/one/attempt-*/result.json"))
        result = json.loads(result_path.read_text())
        result["run_id"] = "foreign-run"
        result_path.write_text(json.dumps(result))
        with self.assertRaisesRegex(ValueError, "splicing"):
            runner.run(None, run_dir, "matrix-a")

    def test_resume_refuses_modified_raw_output(self):
        source = make_plan(self.root / "plan.json", [
            {"id": "one", "command": ["sh", "-c", "printf original"]}
        ])
        run_dir = self.root / "run"
        self.assertEqual(runner.run(source, run_dir, "matrix-a"), 0)
        stdout_path = next(run_dir.glob("artifacts/one/attempt-*/stdout.json"))
        stdout_path.write_text(json.dumps({"text": "replacement"}))
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            runner.run(None, run_dir, "matrix-a")

    def test_resume_refuses_cross_cell_output_splicing(self):
        source = make_plan(self.root / "plan.json", [
            {"id": "one", "command": ["sh", "-c", "printf one"]},
            {"id": "two", "command": ["sh", "-c", "printf two"]},
        ])
        run_dir = self.root / "run"
        self.assertEqual(runner.run(source, run_dir, "matrix-a"), 0)
        checkpoint_path = run_dir / "checkpoints/matrix-a.json"
        checkpoint = json.loads(checkpoint_path.read_text())
        checkpoint["completed"]["one"]["files"]["stdout"] = checkpoint["completed"]["two"]["files"]["stdout"]
        checkpoint_path.write_text(json.dumps(checkpoint))
        with self.assertRaisesRegex(ValueError, "provenance mismatch"):
            runner.run(None, run_dir, "matrix-a")

    def test_checkpoint_name_cannot_escape_run_directory(self):
        source = make_plan(self.root / "plan.json", [
            {"id": "one", "command": ["sh", "-c", "true"]}
        ])
        with self.assertRaisesRegex(ValueError, "checkpoint name"):
            runner.run(source, self.root / "run", "../foreign")

    def test_timeout_preserves_captured_output(self):
        source = make_plan(self.root / "plan.json", [
            {"id": "slow", "command": ["sh", "-c", "printf before; printf warning >&2; sleep 1"],
             "timeout_seconds": 0.05}
        ])
        run_dir = self.root / "run"
        self.assertEqual(runner.run(source, run_dir, "matrix-a"), 2)
        self.assertEqual(output_bytes(next(run_dir.glob("artifacts/slow/attempt-*/stdout.json"))), b"before")
        self.assertEqual(output_bytes(next(run_dir.glob("artifacts/slow/attempt-*/stderr.json"))), b"warning")
        result = json.loads(next(run_dir.glob("artifacts/slow/attempt-*/result.json")).read_text())
        self.assertIn("timed out", result["infrastructure_error"])
        self.assertNotIn(b"timed out", output_bytes(next(run_dir.glob("artifacts/slow/attempt-*/stderr.json"))))

    def test_resume_refuses_modified_manifest_plan(self):
        marker = self.root / "ready"
        source = make_plan(self.root / "plan.json", [
            {"id": "one", "command": ["sh", "-c", f"test -f {marker}"]}
        ])
        run_dir = self.root / "run"
        self.assertEqual(runner.run(source, run_dir, "matrix-a"), 2)
        manifest_path = run_dir / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["plan"]["cells"][0]["command"] = ["sh", "-c", "true"]
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "plan digest"):
            runner.run(None, run_dir, "matrix-a")

    def test_resume_refuses_evidence_for_cell_absent_from_plan(self):
        source = make_plan(self.root / "plan.json", [
            {"id": "one", "command": ["sh", "-c", "true"]}
        ])
        run_dir = self.root / "run"
        self.assertEqual(runner.run(source, run_dir, "matrix-a"), 0)
        checkpoint_path = run_dir / "checkpoints/matrix-a.json"
        checkpoint = json.loads(checkpoint_path.read_text())
        checkpoint["attempts"][0]["cell_id"] = "foreign"
        checkpoint_path.write_text(json.dumps(checkpoint))
        with self.assertRaisesRegex(ValueError, "absent from immutable plan"):
            runner.run(None, run_dir, "matrix-a")

    def test_new_run_never_overwrites_existing_artifacts(self):
        source = make_plan(self.root / "plan.json", [
            {"id": "one", "command": ["sh", "-c", "true"]}
        ])
        run_dir = self.root / "run"
        self.assertEqual(runner.run(source, run_dir, "matrix-a"), 0)
        with self.assertRaises(FileExistsError):
            runner.run(source, run_dir, "matrix-a")

    def test_new_run_rejects_preexisting_symlink_artifact_tree(self):
        source = make_plan(self.root / "plan.json", [
            {"id": "one", "command": ["sh", "-c", "true"]}
        ])
        run_dir = self.root / "run"
        run_dir.mkdir()
        (run_dir / "artifacts").symlink_to(self.root / "outside", target_is_directory=True)
        with self.assertRaises(FileExistsError):
            runner.run(source, run_dir, "matrix-a")
        self.assertFalse((self.root / "outside").exists())

    def test_completed_resume_rejects_replaced_artifact_root(self):
        source = make_plan(self.root / "plan.json", [
            {"id": "one", "command": ["sh", "-c", "true"]}
        ])
        run_dir = self.root / "run"
        self.assertEqual(runner.run(source, run_dir, "matrix-a"), 0)
        artifacts = run_dir / "artifacts"
        real_artifacts = run_dir / "real-artifacts"
        artifacts.rename(real_artifacts)
        artifacts.symlink_to(real_artifacts, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "replaced or symlinked"):
            runner.run(None, run_dir, "matrix-a")

    def test_completed_resume_rejects_in_tree_evidence_symlink(self):
        source = make_plan(self.root / "plan.json", [
            {"id": "one", "command": ["sh", "-c", "printf output"]}
        ])
        run_dir = self.root / "run"
        self.assertEqual(runner.run(source, run_dir, "matrix-a"), 0)
        stdout_path = next(run_dir.glob("artifacts/one/attempt-*/stdout.json"))
        saved_path = stdout_path.with_name("saved-stdout.json")
        stdout_path.rename(saved_path)
        stdout_path.symlink_to(saved_path.name)
        with self.assertRaisesRegex(ValueError, "contains a symlink"):
            runner.run(None, run_dir, "matrix-a")

    def test_new_run_rejects_symlinked_parent(self):
        source = make_plan(self.root / "plan.json", [
            {"id": "one", "command": ["sh", "-c", "true"]}
        ])
        real_parent = self.root / "real-parent"
        real_parent.mkdir()
        linked_parent = self.root / "linked-parent"
        linked_parent.symlink_to(real_parent, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "parent"):
            runner.run(source, linked_parent / "run", "matrix-a")

    def test_invalid_cell_is_retried_as_a_new_preserved_attempt(self):
        marker = self.root / "dependency-ready"
        command = f"if test -f {marker}; then exit 0; else touch {marker}; printf outage >&2; exit 8; fi"
        source = make_plan(self.root / "plan.json", [
            {"id": "retry", "command": ["sh", "-c", command]}
        ])
        run_dir = self.root / "run"
        self.assertEqual(runner.run(source, run_dir, "matrix-a"), 2)
        self.assertEqual(runner.run(None, run_dir, "matrix-a"), 0)
        attempts = list(run_dir.glob("artifacts/retry/attempt-*/result.json"))
        self.assertEqual(len(attempts), 2)
        self.assertEqual(
            {json.loads(attempt.read_text())["status"] for attempt in attempts},
            {"valid", "infrastructure-invalid"},
        )


if __name__ == "__main__":
    unittest.main()
