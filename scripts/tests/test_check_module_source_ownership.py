#!/usr/bin/env python3
"""Mutation tests for canonical module source ownership."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import shutil
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
CHECKER_PATH = REPO / "scripts/check_module_source_ownership.py"
SPEC = importlib.util.spec_from_file_location("module_source_ownership", CHECKER_PATH)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = checker
SPEC.loader.exec_module(checker)


class ModuleSourceOwnershipTests(unittest.TestCase):
    def fixture(self, root: Path) -> None:
        (root / ".git").mkdir()
        files = {
            "src/Makefile",
            "CMakeLists.txt",
            "src/tests/CMakeLists.txt",
            "src/tests/Rules.mk",
            "src/headers/aimee_features.h",
            "src/modules/plugin-loader/module.yaml",
        }
        for contract in checker.CONTRACTS:
            files.update((contract.canonical_source, contract.canonical_header,
                          contract.document, *contract.consumers))
        for relative in files:
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / relative, target)

    def assert_rejected(self, contract, mutate, rule: str) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            mutate(root, contract)
            with self.assertRaisesRegex(checker.CheckError, f"rule={rule}"):
                checker.validate(root)

    def test_repository_and_atomic_fixture_pass(self) -> None:
        checker.validate(REPO)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            checker.validate(root)

    def test_legacy_paths_are_rejected_for_every_contract(self) -> None:
        for contract in checker.CONTRACTS:
            with self.subTest(contract=contract.module):
                self.assert_rejected(
                    contract,
                    lambda root, value: (root / value.legacy_source).write_text("legacy"),
                    "legacy-path-removed",
                )

    def test_legacy_skills_root_and_build_paths_are_rejected(self) -> None:
        contract = checker.CONTRACTS[0]
        module, legacy_root, build_files = checker.LEGACY_MODULE_ROOTS[0]
        self.assertEqual(module, "skills")
        self.assert_rejected(
            contract,
            lambda root, value: (root / legacy_root).mkdir(parents=True),
            "legacy-module-root",
        )
        for relative in build_files:
            with self.subTest(relative=relative):
                self.assert_rejected(
                    contract,
                    lambda root, value, path=relative: (root / path).write_text(
                        (root / path).read_text() + "\nmodules/skill/skill.c\n"
                    ),
                    "legacy-build-root",
                )

    def test_legacy_root_must_be_normalized_beneath_src(self) -> None:
        for legacy_root in ("modules/skill", "/src/modules/skill", "src/../modules/skill"):
            with self.subTest(legacy_root=legacy_root):
                with self.assertRaisesRegex(checker.CheckError, "rule=legacy-root-format"):
                    checker.validate_legacy_module_root(
                        REPO,
                        "skills",
                        legacy_root,
                        ("src/Makefile",),
                    )

    def test_build_and_test_drift_is_rejected_for_every_contract(self) -> None:
        for contract in checker.CONTRACTS:
            with self.subTest(contract=contract.module):
                self.assert_rejected(
                    contract,
                    lambda root, value: (root / "src/Makefile").write_text(
                        (root / "src/Makefile").read_text() + "\n" + value.make_source
                    ),
                    "core-source-unique",
                )
                self.assert_rejected(
                    contract,
                    lambda root, value: (root / "src/tests/Rules.mk").write_text(
                        (root / "src/tests/Rules.mk").read_text() + value.legacy_test_object
                    ),
                    "focused-test-object",
                )

    def test_consumer_include_drift_is_rejected_for_every_contract(self) -> None:
        for contract in checker.CONTRACTS:
            with self.subTest(contract=contract.module):
                def mutate(root, value):
                    path = root / value.consumers[1]
                    path.write_text(path.read_text().replace(
                        value.canonical_include, Path(value.legacy_header).name
                    ))
                self.assert_rejected(contract, mutate, "canonical-include-missing")

    def test_unlisted_legacy_ir_include_is_rejected(self) -> None:
        contract = next(item for item in checker.CONTRACTS if item.module == "ir-messaging")
        self.assert_rejected(
            contract,
            lambda root, value: (root / "src/unlisted.c").write_text(
                '#include "aimee_ir.h"\n'
            ),
            "non-canonical-module-include",
        )

    def test_ir_test_cmake_path_drift_is_rejected(self) -> None:
        contract = next(item for item in checker.CONTRACTS if item.module == "ir-messaging")
        self.assert_rejected(
            contract,
            lambda root, value: (root / "src/tests/CMakeLists.txt").write_text(
                (root / "src/tests/CMakeLists.txt").read_text().replace(
                    value.test_cmake_source,
                    value.legacy_test_cmake_source,
                )
            ),
            "focused-test-source",
        )

    def test_cmake_absence_contracts_reject_source_additions(self) -> None:
        for contract in checker.CONTRACTS:
            if contract.cmake_source is not None:
                continue
            with self.subTest(contract=contract.module):
                self.assert_rejected(
                    contract,
                    lambda root, value: (root / "CMakeLists.txt").write_text(
                        (root / "CMakeLists.txt").read_text()
                        + "\n"
                        + value.canonical_source
                        + "\n"
                    ),
                    "core-source-unique",
                )

    def test_document_markers_are_required_for_every_contract(self) -> None:
        for contract in checker.CONTRACTS:
            markers = (contract.canonical_source, contract.canonical_header,
                       *contract.consumers, *contract.document_markers)
            for marker in markers:
                with self.subTest(contract=contract.module, marker=marker):
                    self.assert_rejected(
                        contract,
                        lambda root, value, item=marker: (root / value.document).write_text(
                            (root / value.document).read_text().replace(item, "missing")
                        ),
                        "module-document",
                    )

    def test_removed_wrapper_symbols_cannot_return(self) -> None:
        contract = next(
            item for item in checker.CONTRACTS if item.module == "module-runtime-extension"
        )
        self.assert_rejected(
            contract,
            lambda root, value: (root / value.canonical_source).write_text(
                (root / value.canonical_source).read_text() + "\nplugin_ctx_create_ex\n"
            ),
            "dead-wrapper-removed",
        )

    def test_required_runtime_cannot_include_optional_loader(self) -> None:
        contract = next(
            item for item in checker.CONTRACTS if item.module == "module-runtime-extension"
        )
        self.assert_rejected(
            contract,
            lambda root, value: (root / value.canonical_source).write_text(
                (root / value.canonical_source).read_text()
                + '\n#include "aimee/plugin-loader/plugin.h"\n'
            ),
            "core-to-optional-edge",
        )

    def test_retired_plugin_headers_cannot_be_included(self) -> None:
        contract = next(
            item for item in checker.CONTRACTS if item.module == "module-runtime-extension"
        )
        self.assert_rejected(
            contract,
            lambda root, value: (root / value.canonical_source).write_text(
                (root / value.canonical_source).read_text()
                + '\n#include "headers/plugin_ctx.h"\n'
            ),
            "legacy-include-removed",
        )

    def test_plugin_loader_profile_defaults_cannot_drift(self) -> None:
        contract = checker.CONTRACTS[0]
        mutations = (
            ("src/Makefile", "AIMEE_WITH_PLUGIN_LOADER ?= 0", "AIMEE_WITH_PLUGIN_LOADER ?= 1"),
            ("CMakeLists.txt", "plugin manifest loader\" OFF", "plugin manifest loader\" ON"),
            ("src/headers/aimee_features.h", "AIMEE_WITH_PLUGIN_LOADER 0",
             "AIMEE_WITH_PLUGIN_LOADER 1"),
            ("src/modules/plugin-loader/module.yaml", '"enabled_by_default": false',
             '"enabled_by_default": true'),
        )
        for relative, before, after in mutations:
            with self.subTest(relative=relative):
                self.assert_rejected(
                    contract,
                    lambda root, value, path=relative, old=before, new=after: (
                        root / path
                    ).write_text((root / path).read_text().replace(old, new)),
                    "plugin-loader-profile-default",
                )


if __name__ == "__main__":
    unittest.main()
