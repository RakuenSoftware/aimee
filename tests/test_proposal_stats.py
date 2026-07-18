#!/usr/bin/env python3
"""End-to-end tests for scripts/proposal_stats.py.

Covers acceptance criteria for WI bfd95891...s1:
  - reports counts of files in docs/proposals/{pending,done}
  - --json emits the same counts as a single JSON object on stdout
  - pending_words is whitespace-token count over pending files only
  - symlinked state directories that resolve outside the proposals root
    are refused with RuntimeError

The CLI has no path flag, so the script-under-test is fixed to
<repo>/docs/proposals. We cover that with a subprocess invocation.
We also drive _collect() directly against a tempdir fixture so the
file-counting logic — including the symlink-refusal safety check — is
exercised without depending on the real workspace contents.
"""
from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).parent.parent
SCRIPT = REPO_ROOT / "scripts" / "proposal_stats.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("proposal_stats", SCRIPT)
    assert spec and spec.loader, "could not load proposal_stats"
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


class TestProposalStats(unittest.TestCase):
    # --- subprocess / CLI behavior against the real repo layout -------------

    def test_cli_human_format_lists_counts(self):
        result = subprocess.run(
            [sys.executable, str(SCRIPT)],
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        for label in ("pending:", "done:", "pending_words:"):
            self.assertIn(label, result.stdout)

    def test_cli_json_shape_matches_contract(self):
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--json"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(
            set(payload.keys()), {"pending", "done", "pending_words"}
        )
        self.assertIsInstance(payload["pending"], int)
        self.assertIsInstance(payload["done"], int)
        self.assertIsInstance(payload["pending_words"], int)
        self.assertGreaterEqual(payload["pending"], 1)
        self.assertGreaterEqual(payload["done"], 0)
        self.assertGreaterEqual(payload["pending_words"], 1)

    # --- unit tests against a tempdir fixture -------------------------------

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        (self.root / "pending").mkdir()
        (self.root / "done").mkdir()
        self.mod = _load_module()

    def tearDown(self):
        self._tmp.cleanup()

    def _write(self, subdir: str, name: str, body: str) -> Path:
        path = self.root / subdir / name
        path.write_text(textwrap.dedent(body), encoding="utf-8")
        return path

    def test_collect_counts_files_per_directory(self):
        self._write("pending", "a.md", "alpha beta gamma\n")
        self._write("pending", "b.md", "delta\n")
        self._write("done", "c.md", "epsilon\n")

        stats = self.mod._collect(str(self.root))
        self.assertEqual(stats["pending"], 2)
        self.assertEqual(stats["done"], 1)

    def test_collect_counts_pending_words_only(self):
        self._write("pending", "a.md", "one two three\n")  # 3 tokens
        self._write("pending", "b.md", "four\n")           # 1 token
        self._write("done", "c.md", "ignored tokens here\n")  # must NOT count

        stats = self.mod._collect(str(self.root))
        self.assertEqual(stats["pending_words"], 4)

    def test_collect_empty_tree_returns_zero_counts(self):
        stats = self.mod._collect(str(self.root))
        self.assertEqual(stats, {"pending": 0, "done": 0, "pending_words": 0})

    def test_collect_refuses_symlinked_pending_dir(self):
        # The state directory itself must not be a symlink, even if the
        # target is a real directory. This is the safety boundary that
        # keeps `pending/` from being followed into arbitrary locations.
        external = Path(self._tmp.name).parent / "external_pending"
        external.mkdir()
        (external / "real.md").write_text("content\n", encoding="utf-8")
        try:
            os.replace(self.root / "pending", self.root / "pending_real")
            os.symlink(external, self.root / "pending")
            with self.assertRaises(RuntimeError):
                self.mod._collect(str(self.root))
        finally:
            if os.path.islink(self.root / "pending"):
                os.remove(self.root / "pending")
            if (self.root / "pending_real").exists():
                os.rename(self.root / "pending_real", self.root / "pending")
            (external / "real.md").unlink(missing_ok=True)
            external.rmdir()


if __name__ == "__main__":
    unittest.main()