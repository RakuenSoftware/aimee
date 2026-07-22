#!/usr/bin/env python3
"""Tests for the deterministic refactor surface baseline."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "refactor_baselines.py"
SPEC = importlib.util.spec_from_file_location("refactor_baselines", SCRIPT)
assert SPEC and SPEC.loader
baselines = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(baselines)


class RefactorBaselineTests(unittest.TestCase):
    def test_repository_baseline_is_cwd_independent(self) -> None:
        with tempfile.TemporaryDirectory() as cwd:
            result = subprocess.run(
                [sys.executable, "-I", "-S", str(SCRIPT)],
                cwd=cwd,
                text=True,
                capture_output=True,
                check=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_output_is_deterministic_and_sorted(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "inputs").mkdir()
            (root / "inputs/z.txt").write_bytes(b"z\r\n")
            (root / "inputs/a.txt").write_bytes(b"a\n")
            old = baselines.SURFACES
            baselines.SURFACES = {"example": ("inputs/*.txt",)}
            try:
                with mock.patch.object(
                    baselines,
                    "_tracked_files",
                    return_value=sorted((root / "inputs").glob("*")),
                ):
                    first = baselines.build_index(root)
                    second = baselines.build_index(root)
            finally:
                baselines.SURFACES = old
        self.assertEqual(first, second)
        self.assertEqual(
            [item["path"] for item in first["surfaces"]["example"]["files"]],
            ["inputs/a.txt", "inputs/z.txt"],
        )

    def test_content_and_membership_drift_fail(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "inputs").mkdir()
            target = root / "inputs/a.txt"
            target.write_text("a\n", encoding="utf-8")
            index = root / "index.json"
            old = baselines.SURFACES
            baselines.SURFACES = {"example": ("inputs/*.txt",)}
            try:
                with mock.patch.object(baselines, "_tracked_files", return_value=[target]):
                    index.write_text(baselines._canonical(baselines.build_index(root)), encoding="utf-8")
                    target.write_text("changed\n", encoding="utf-8")
                    with self.assertRaisesRegex(baselines.BaselineError, "surface drift"):
                        baselines.check(root, index)
                    target.write_text("a\n", encoding="utf-8")
                    new = root / "inputs/new.txt"
                    new.write_text("new\n", encoding="utf-8")
                    with mock.patch.object(baselines, "_tracked_files", return_value=[target, new]):
                        with self.assertRaisesRegex(baselines.BaselineError, "surface drift"):
                            baselines.check(root, index)
            finally:
                baselines.SURFACES = old

    def test_malformed_index_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "index.json"
            path.write_text("{", encoding="utf-8")
            with self.assertRaisesRegex(baselines.BaselineError, "cannot load"):
                baselines.check(Path(directory), path)

    def test_duplicate_index_key_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "index.json"
            path.write_text('{"schema_version":1,"schema_version":1}', encoding="utf-8")
            with self.assertRaisesRegex(baselines.BaselineError, "duplicate object key"):
                baselines.check(Path(directory), path)

    def test_install_recipe_requires_tracked_nonempty_tabbed_body(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            makefile = root / "src/Makefile"
            makefile.write_text(
                ".PHONY: install\ninstall:\n\tfirst \\\n\t  continued\n\n\tsecond\nnext:\n\tignored\n",
                encoding="utf-8",
            )
            recipe = baselines._make_install_recipe(root, [makefile]).decode()
            self.assertEqual(recipe, "install:\n\tfirst \\\n\t  continued\n\n\tsecond\n")
            with self.assertRaisesRegex(baselines.BaselineError, "tracked"):
                baselines._make_install_recipe(root, [])
            makefile.write_text("install:\nnext:\n", encoding="utf-8")
            with self.assertRaisesRegex(baselines.BaselineError, "no tab-indented recipe"):
                baselines._make_install_recipe(root, [makefile])

    def test_freeze_requires_explicit_dirty_tree_acceptance(self) -> None:
        result = mock.Mock(stdout=" M src/example.c\n")
        with mock.patch.object(baselines.subprocess, "run", return_value=result):
            with self.assertRaisesRegex(baselines.BaselineError, "--accept-dirty"):
                baselines._require_freeze_intent(ROOT, False)
            baselines._require_freeze_intent(ROOT, True)


if __name__ == "__main__":
    unittest.main()
