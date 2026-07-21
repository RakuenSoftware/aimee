#!/usr/bin/env python3
"""Mutation tests for plugin-loader physical ownership."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
CHECKER_PATH = REPO / "scripts/check_plugin_loader_ownership.py"
SPEC = importlib.util.spec_from_file_location("plugin_loader_ownership", CHECKER_PATH)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class PluginLoaderOwnershipTests(unittest.TestCase):
    def fixture(self, root: Path) -> None:
        (root / ".git").mkdir()
        files = {
            checker.CANONICAL_SOURCE,
            checker.CANONICAL_HEADER,
            "src/server/server_main.c",
            "src/tests/test_plugin_loader.c",
            "src/tests/Rules.mk",
            "src/Makefile",
            "CMakeLists.txt",
            checker.DOC,
        }
        for relative in files:
            source = REPO / relative
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)

    def assert_rejected(self, mutate, rule: str) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            mutate(root)
            with self.assertRaisesRegex(checker.CheckError, f"rule={rule}"):
                checker.validate(root)

    def test_repository_and_atomic_fixture_pass(self) -> None:
        checker.validate(REPO)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            checker.validate(root)

    def test_legacy_path_is_rejected(self) -> None:
        self.assert_rejected(
            lambda root: (root / checker.LEGACY_SOURCE).write_text("legacy"),
            "legacy-path-removed",
        )

    def test_duplicate_build_source_is_rejected(self) -> None:
        self.assert_rejected(
            lambda root: (root / "src/Makefile").write_text(
                (root / "src/Makefile").read_text() + checker.MAKE_SOURCE
            ),
            "core-source-unique",
        )

    def test_legacy_include_is_rejected(self) -> None:
        def mutate(root: Path) -> None:
            path = root / "src/server/server_main.c"
            path.write_text(path.read_text().replace(
                checker.CANONICAL_INCLUDE, '#include "headers/plugin_loader.h"'
            ))

        self.assert_rejected(mutate, "canonical-include-missing")

    def test_duplicate_canonical_include_is_rejected(self) -> None:
        def mutate(root: Path) -> None:
            path = root / "src/server/server_main.c"
            path.write_text(path.read_text() + "\n" + checker.CANONICAL_INCLUDE + "\n")

        self.assert_rejected(mutate, "canonical-include-duplicated")

    def test_legacy_build_entries_are_rejected(self) -> None:
        self.assert_rejected(
            lambda root: (root / "src/tests/Rules.mk").write_text(
                (root / "src/tests/Rules.mk").read_text() + checker.LEGACY_TEST_OBJECT
            ),
            "focused-test-object",
        )
        self.assert_rejected(
            lambda root: (root / "CMakeLists.txt").write_text(
                (root / "CMakeLists.txt").read_text() + "\n${AIMEE_SRC_DIR}/plugin_loader.c\n"
            ),
            "core-source-unique",
        )
        self.assert_rejected(
            lambda root: (root / "src/Makefile").write_text(
                (root / "src/Makefile").read_text() + "\nplugin_loader.c\n"
            ),
            "core-source-unique",
        )

    def test_incomplete_document_is_rejected(self) -> None:
        for marker in checker.DOC_MARKERS:
            with self.subTest(marker=marker):
                self.assert_rejected(
                    lambda root, value=marker: (root / checker.DOC).write_text(
                        (root / checker.DOC).read_text().replace(value, "missing", 1)
                    ),
                    "module-document",
                )

    def test_include_root_drift_is_rejected(self) -> None:
        self.assert_rejected(
            lambda root: (root / "src/Makefile").write_text(
                (root / "src/Makefile").read_text().replace(checker.MAKE_INCLUDE, "-Iheaders")
            ),
            "module-include-root",
        )


if __name__ == "__main__":
    unittest.main()
