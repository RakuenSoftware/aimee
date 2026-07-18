#!/usr/bin/env python3
"""End-to-end tests for scripts/proposal_stats.py.

Covers acceptance criteria for WI bfd95891...s1:
  - reports counts of files in <root>/{pending,done}
  - --json emits the same counts as a single JSON object on stdout
  - pending_words is whitespace-token count over pending files only
  - symlinked state directories that resolve outside the proposals root
    are refused with RuntimeError

The CLI accepts --root, so subprocess tests drive the script against a
freshly-built tempdir fixture (no coupling to the host workspace). A
separate smoke test exercises the real docs/proposals tree at the
default location; it is intentionally distinct from the acceptance
criteria so that an empty or absent live tree does not flake the suite.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "proposal_stats.py"


def _make_fixture(tmp: Path, pending: dict[str, str], done: dict[str, str]) -> Path:
    """Build a tempdir containing pending/<name> and done/<name> files."""
    (tmp / "pending").mkdir(parents=True, exist_ok=True)
    (tmp / "done").mkdir(parents=True, exist_ok=True)
    for name, body in pending.items():
        (tmp / "pending" / name).write_text(body, encoding="utf-8")
    for name, body in done.items():
        (tmp / "done" / name).write_text(body, encoding="utf-8")
    return tmp


def _run(*args: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        capture_output=True,
        text=True,
        cwd=cwd,
    )


class ProposalStatsTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    # --- subprocess CLI: driven against the tempdir fixture ---

    def test_cli_human_format_lists_counts(self) -> None:
        _make_fixture(
            self.tmp,
            pending={"a.md": "alpha beta gamma\n", "b.md": "delta\n"},
            done={"c.md": "epsilon\n", "d.md": "zeta\n", "e.md": "eta\n"},
        )
        proc = _run("--root", str(self.tmp))
        self.assertEqual(proc.returncode, 0, msg=proc.stderr)
        # Exact counts from the fixture: 2 pending, 3 done.
        self.assertEqual(
            proc.stdout,
            "pending: 2\ndone:    3\npending_words: 4\n",
        )

    def test_cli_json_shape_matches_contract(self) -> None:
        # 5 words across 2 pending files; 0 done.
        _make_fixture(
            self.tmp,
            pending={"a.md": "one two\n", "b.md": "three four five\n"},
            done={},
        )
        proc = _run("--root", str(self.tmp), "--json")
        self.assertEqual(proc.returncode, 0, msg=proc.stderr)
        parsed = json.loads(proc.stdout)
        self.assertEqual(parsed, {"pending": 2, "done": 0, "pending_words": 5})

    def test_cli_refuses_escaping_symlinked_state_dir(self) -> None:
        # Build a fixture whose pending/ symlink escapes the tempdir, then
        # point --root at a wrapper directory containing the symlink. The
        # script should refuse with a non-zero exit and a clear error.
        real = self.tmp / "real_pending"
        real.mkdir()
        (real / "a.md").write_text("outside content\n", encoding="utf-8")
        wrapper = self.tmp / "wrapper"
        wrapper.mkdir()
        os.symlink(real, wrapper / "pending")
        (wrapper / "done").mkdir()

        proc = _run("--root", str(wrapper))
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("symlink", proc.stderr)

    # --- smoke: real-tree layout, not part of the acceptance contract ---

    def test_smoke_default_root_runs_against_repo_layout(self) -> None:
        """If docs/proposals/{pending,done} exist in the repo, the script
        runs end-to-end at the default location. This is a smoke check only;
        the acceptance-criteria tests above drive a tempdir fixture."""
        repo_root = ROOT / "docs" / "proposals"
        if not (repo_root / "pending").is_dir() or not (repo_root / "done").is_dir():
            self.skipTest("docs/proposals/{pending,done} not present in repo")
        proc = _run()
        self.assertEqual(proc.returncode, 0, msg=proc.stderr)
        self.assertIn("pending:", proc.stdout)
        self.assertIn("done:", proc.stdout)
        self.assertIn("pending_words:", proc.stdout)


if __name__ == "__main__":
    unittest.main()