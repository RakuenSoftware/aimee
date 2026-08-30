#!/usr/bin/env python3
"""Mutation tests for the background skill-curator absence contract."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
CHECKER_PATH = REPO / "scripts/check_background_skill_curator_absence.py"
SPEC = importlib.util.spec_from_file_location("curator_absence", CHECKER_PATH)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class CuratorAbsenceTests(unittest.TestCase):
    def fixture(self, root: Path) -> None:
        (root / ".git").mkdir()
        (root / "src/server").mkdir(parents=True)
        files = set(checker.BUILD_FILES + (
            "src/modules/memory/memory_maintenance.c",
            "server-go/modules/aimee/families/runtime_state.go",
            "src/modules/kb-synthesis/kb_curator_pipeline.c",
            "src/modules/kb-synthesis/kb_curator_queue.c",
            "src/modules/kb-synthesis/kb_curator_drain.c",
            checker.DISPOSITION,
        ))
        for rel in files:
            src = REPO / rel
            dst = root / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)

    def assert_rejected(self, mutate, rule: str) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            mutate(root)
            with self.assertRaisesRegex(checker.CheckError, f"rule={rule}"):
                checker.validate(root)

    def test_repository_passes(self) -> None:
        checker.validate(REPO)

    def test_each_forbidden_reference_is_rejected(self) -> None:
        for token in checker.FORBIDDEN:
            with self.subTest(token=token):
                self.assert_rejected(
                    lambda root, value=token: (root / "src/server/reintroduced.c").write_text(value),
                    "retired-reference",
                )

    def test_deleted_file_and_build_objects_are_rejected(self) -> None:
        self.assert_rejected(
            lambda root: (root / checker.DELETED[0]).parent.mkdir(parents=True, exist_ok=True) or
            (root / checker.DELETED[0]).write_text("retired"),
            "deleted-file",
        )
        for rel in checker.BUILD_FILES:
            with self.subTest(build_file=rel):
                self.assert_rejected(
                    lambda root, path=rel: (root / path).write_text("skill_curator.o"),
                    "retired-build-object",
                )

    def test_canonical_curator_resurrection_paths_are_rejected(self) -> None:
        for relative in checker.CANONICAL_DELETED:
            with self.subTest(relative=relative):
                def resurrect(root: Path, path: str = relative) -> None:
                    target = root / path
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_text("")

                self.assert_rejected(resurrect, "deleted-file")

    def test_preserved_anchors_are_enforced(self) -> None:
        self.assert_rejected(
            lambda root: (root / "src/modules/memory/memory_maintenance.c").write_text("empty"),
            "memory-maintenance-preserved",
        )
        self.assert_rejected(
            lambda root: (root / "src/modules/kb-synthesis/kb_curator_pipeline.c").unlink(),
            "kb-curator-preserved",
        )

    def test_disposition_fails_closed(self) -> None:
        def corrupt(root: Path) -> None:
            path = root / checker.DISPOSITION
            value = json.loads(path.read_text())
            value["decision"] = "keep"
            path.write_text(json.dumps(value))

        self.assert_rejected(corrupt, "disposition")


if __name__ == "__main__":
    unittest.main()
