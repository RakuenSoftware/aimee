import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from scripts.memory_hygiene_schedule import tick


class HygieneScheduleTests(unittest.TestCase):
    def reply(self, cursor="", partial=False):
        return subprocess.CompletedProcess([], 0, json.dumps({"status": "ok", "dry_run": False,
            "canonical_writes": 0, "job_id": "1", "run_id": "run", "partial": partial,
            "resume_cursor": cursor, "policy": "bounded-proposal-hygiene-v1"}), "")

    def test_bounded_tick_resumes_and_never_requests_apply(self):
        with tempfile.TemporaryDirectory() as folder, patch("scripts.memory_hygiene_schedule.subprocess.run") as run:
            state = Path(folder) / "state.json"
            run.side_effect = [self.reply("next", True), self.reply()]
            first = tick("/aimee", "project:p", state, max_pages=1)
            self.assertEqual(first["status"], "partial")
            second = tick("/aimee", "project:p", state, max_pages=1)
            self.assertEqual(second["status"], "complete")
            self.assertIn("next", run.call_args.args[0])
            for call in run.call_args_list:
                self.assertNotIn("--apply", call.args[0])
            self.assertEqual(state.stat().st_mode & 0o777, 0o600)
            with self.assertRaises(ValueError):
                tick("/aimee", "project:other", state)

    def test_failure_preserves_cursor_and_stall_is_not_clean(self):
        with tempfile.TemporaryDirectory() as folder, patch("scripts.memory_hygiene_schedule.subprocess.run") as run:
            state = Path(folder) / "state.json"
            run.return_value = self.reply("next", True)
            tick("/aimee", "project:p", state, max_pages=1)
            before = state.read_bytes()
            run.return_value = subprocess.CompletedProcess([], 1, "private failure", "")
            with self.assertRaises(RuntimeError):
                tick("/aimee", "project:p", state)
            self.assertEqual(before, state.read_bytes())
            run.return_value = self.reply("next", True)
            self.assertEqual(tick("/aimee", "project:p", state)["status"], "partial_without_progress")


if __name__ == "__main__":
    unittest.main()
