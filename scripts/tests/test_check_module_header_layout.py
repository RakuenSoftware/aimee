#!/usr/bin/env python3
"""Mutation tests for canonical module public-header layout."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
CHECKER = REPO / "scripts/check_module_header_layout.py"
SPEC = importlib.util.spec_from_file_location("module_header_layout", CHECKER)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class ModuleHeaderLayoutTests(unittest.TestCase):
    def fixture(self, root: Path) -> None:
        descriptor = root / "src/modules/audit/module.yaml"
        descriptor.parent.mkdir(parents=True)
        descriptor.write_text(json.dumps({
            "id": "audit",
            "public_headers": [
                "src/modules/audit/include/aimee/audit/audit_action.h",
                "src/modules/audit/include/aimee/audit/audit_worm.h",
            ],
        }), encoding="utf-8")
        canonical = root / "src/modules/audit/include/aimee/audit"
        canonical.mkdir(parents=True)
        for name in ("audit_action.h", "audit_worm.h"):
            (canonical / name).write_text("/* canonical */\n", encoding="utf-8")
        (root / "src/consumer.c").write_text(
            "#include <aimee/audit/audit_action.h>\n", encoding="utf-8"
        )
        (root / "src/Makefile").write_text(
            "C_FLAGS = -Imodules/audit/include\n", encoding="utf-8"
        )
        (root / "CMakeLists.txt").write_text(
            'set(AIMEE_AUDIT_INCLUDE_DIR "${AIMEE_SRC_DIR}/modules/audit/include")\n',
            encoding="utf-8",
        )

    def assert_rule(self, mutate, rule: str) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            mutate(root)
            with self.assertRaisesRegex(checker.HeaderLayoutError, f"rule={rule}"):
                checker.validate(root)

    def test_repository_and_fixture_pass(self) -> None:
        checker.validate(REPO)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            self.assertEqual(checker.violations(root), [])
            checker.validate(root)

    def test_restored_flat_header_is_rejected(self) -> None:
        self.assert_rule(
            lambda root: (root / "src/modules/audit/audit_action.h").write_text("legacy\n"),
            "retired-header-path",
        )

    def test_dangling_flat_header_symlink_is_rejected(self) -> None:
        self.assert_rule(
            lambda root: (root / "src/modules/audit/audit_action.h").symlink_to("missing.h"),
            "retired-header-path",
        )

    def test_descriptor_input_uses_shared_resource_bounds(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            descriptor = root / "src/modules/audit/module.yaml"
            self.assertIs(type(checker.validator.MAX_BYTES), int)
            descriptor.write_bytes(b" " * (checker.validator.MAX_BYTES + 1))
            with self.assertRaisesRegex(checker.HeaderLayoutError, "rule=descriptor-input"):
                checker.validate(root)

    def test_quoted_and_angle_basename_includes_are_rejected(self) -> None:
        for include in (
            '#include "audit_action.h"\n',
            '#include <audit_action.h> // retired form\n',
        ):
            with self.subTest(include=include):
                self.assert_rule(
                    lambda root, value=include: (root / "src/consumer.c").write_text(value),
                    "retired-header-include",
                )

    def test_make_source_root_forms_are_rejected(self) -> None:
        for value in (
            "-Imodules/audit",
            "-I modules/audit",
            "-Iheaders \\\n+  -Imodules/audit",
        ):
            with self.subTest(value=value):
                self.assert_rule(
                    lambda root, item=value: (root / "src/Makefile").write_text(
                        f"C_FLAGS = {item}\n"
                    ),
                    "retired-include-root",
                )
    def test_direct_cmake_source_root_is_rejected(self) -> None:
        self.assert_rule(
            lambda root: (root / "CMakeLists.txt").write_text(
                'target_include_directories(x PUBLIC\n  ${AIMEE_SRC_DIR}/modules/audit\n)\n'
            ),
            "retired-include-root",
        )

    def test_cmake_generator_expression_is_out_of_scope(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            (root / "CMakeLists.txt").write_text(
                "target_include_directories(x PUBLIC\n"
                "  $<BUILD_INTERFACE:${AIMEE_SRC_DIR}/modules/audit>\n)\n",
                encoding="utf-8",
            )
            self.assertEqual(checker.violations(root), [])

    def test_canonical_cmake_include_subpath_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            (root / "CMakeLists.txt").write_text(
                "target_include_directories(x PUBLIC\n"
                "  ${AIMEE_SRC_DIR}/modules/audit/include\n)\n",
                encoding="utf-8",
            )
            self.assertEqual(checker.violations(root), [])

    def test_nested_protocol_namespace_retires_nested_source_forms(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            descriptor = root / "src/modules/protocols/module.yaml"
            descriptor.parent.mkdir(parents=True)
            descriptor.write_text(json.dumps({
                "id": "protocols",
                "public_headers": [
                    "src/modules/protocols/include/aimee/protocols/mcp/mcp_client.h"
                ],
            }), encoding="utf-8")
            canonical = (
                root
                / "src/modules/protocols/include/aimee/protocols/mcp/mcp_client.h"
            )
            canonical.parent.mkdir(parents=True)
            canonical.write_text("/* canonical */\n", encoding="utf-8")
            (root / "src/consumer.c").write_text(
                '#include "mcp/mcp_client.h"\n', encoding="utf-8"
            )
            (root / "src/Makefile").write_text(
                "C_FLAGS = -Imodules/audit/include -Imodules/protocols/mcp\n",
                encoding="utf-8",
            )
            (root / "CMakeLists.txt").write_text(
                "target_include_directories(x PUBLIC\n"
                "  ${AIMEE_SRC_DIR}/modules/protocols/mcp\n)\n",
                encoding="utf-8",
            )
            problems = checker.violations(root)
            self.assertTrue(any("header=mcp/mcp_client.h" in item for item in problems))
            self.assertTrue(any("value=-Imodules/protocols/mcp" in item for item in problems))
            self.assertTrue(any(
                "value=${AIMEE_SRC_DIR}/modules/protocols/mcp" in item
                for item in problems
            ))

    def test_nested_protocol_namespace_also_retires_broad_module_root(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            descriptor = root / "src/modules/protocols/module.yaml"
            descriptor.parent.mkdir(parents=True)
            descriptor.write_text(json.dumps({
                "id": "protocols",
                "public_headers": [
                    "src/modules/protocols/include/aimee/protocols/mcp/mcp_client.h"
                ],
            }), encoding="utf-8")
            canonical = (
                root
                / "src/modules/protocols/include/aimee/protocols/mcp/mcp_client.h"
            )
            canonical.parent.mkdir(parents=True)
            canonical.write_text("/* canonical */\n", encoding="utf-8")
            (root / "src/Makefile").write_text(
                "C_FLAGS = -Imodules/audit/include -Imodules/protocols\n",
                encoding="utf-8",
            )
            (root / "CMakeLists.txt").write_text(
                "target_include_directories(x PUBLIC\n"
                "  ${AIMEE_SRC_DIR}/modules/protocols\n)\n",
                encoding="utf-8",
            )
            problems = checker.violations(root)
            self.assertTrue(any("value=-Imodules/protocols" in item for item in problems))
            self.assertTrue(any(
                "value=${AIMEE_SRC_DIR}/modules/protocols" in item
                for item in problems
            ))

    def test_missing_mandatory_build_inputs_fail_closed(self) -> None:
        for relative in ("src/Makefile", "CMakeLists.txt"):
            with self.subTest(relative=relative), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                self.fixture(root)
                (root / relative).unlink()
                with self.assertRaisesRegex(checker.HeaderLayoutError, "rule=missing-input"):
                    checker.validate(root)

    def test_missing_canonical_header_is_rejected(self) -> None:
        self.assert_rule(
            lambda root: (
                root / "src/modules/audit/include/aimee/audit/audit_action.h"
            ).unlink(),
            "missing-canonical-header",
        )

    def test_diagnostics_are_sorted_and_deduplicated(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            (root / "src/consumer.c").write_text(
                '#include "audit_worm.h"\n#include "audit_action.h"\n', encoding="utf-8"
            )
            problems = checker.violations(root)
            self.assertEqual(problems, sorted(set(problems)))
            self.assertEqual(len(problems), 2)
            self.assertIn("header=audit_action.h form=quoted", problems[0])
            self.assertIn("header=audit_worm.h form=quoted", problems[1])

    def test_source_symlink_is_aggregated_with_other_violations(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            (root / "src/link.c").symlink_to("missing.c")
            (root / "src/consumer.c").write_text(
                '#include "audit_action.h"\n', encoding="utf-8"
            )
            problems = checker.violations(root)
            self.assertEqual(problems, sorted(problems))
            self.assertEqual(len(problems), 2)
            self.assertTrue(any("rule=source-symlink" in item for item in problems))
            self.assertTrue(any("rule=retired-header-include" in item for item in problems))

    def test_gitignored_local_worktree_sources_are_not_repository_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            (root / ".gitignore").write_text("/.shadow/\n", encoding="utf-8")
            shadow = root / ".shadow/worktree/src/legacy.c"
            shadow.parent.mkdir(parents=True)
            shadow.write_text('#include "audit_action.h"\n', encoding="utf-8")
            self.assertEqual(checker.violations(root), [])

    def test_shared_basename_reports_each_claim_without_interference(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            other = root / "src/modules/other/module.yaml"
            other.parent.mkdir(parents=True)
            other.write_text(json.dumps({
                "id": "other",
                "public_headers": [
                    "src/modules/other/include/aimee/other/audit_action.h"
                ],
            }), encoding="utf-8")
            canonical = root / "src/modules/other/include/aimee/other/audit_action.h"
            canonical.parent.mkdir(parents=True)
            canonical.write_text("/* canonical */\n", encoding="utf-8")
            (root / "src/consumer.c").write_text(
                '#include "audit_action.h"\n', encoding="utf-8"
            )
            problems = checker.violations(root)
            self.assertEqual(len(problems), 2)
            self.assertTrue(any("module=audit " in item for item in problems))
            self.assertTrue(any("module=other " in item for item in problems))


if __name__ == "__main__":
    unittest.main()
