#!/usr/bin/env python3
"""Tests for the descriptor-declared module test registration baseline."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "scripts/check_module_test_registration.py"
SPEC = importlib.util.spec_from_file_location("check_module_test_registration", CHECKER)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class RegistrationTests(unittest.TestCase):
    def repo(self) -> tempfile.TemporaryDirectory[str]:
        tmp = tempfile.TemporaryDirectory()
        root = Path(tmp.name)
        for relative in (checker.CMAKE_TESTS, checker.MAKE_RULES, checker.BASELINE):
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO_ROOT / relative, target)
        for source in (REPO_ROOT / checker.MODULES).glob("*/module.yaml"):
            target = root / checker.MODULES / source.parent.name / "module.yaml"
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        return tmp

    def test_production_tree_matches_its_baseline(self) -> None:
        checker.check(REPO_ROOT)

    def test_report_is_deterministic_and_sorted(self) -> None:
        report = checker.report(REPO_ROOT)
        self.assertEqual(report["schema_version"], 1)
        keys = [(row["module"], row["test"]) for row in report["tests"]]
        self.assertEqual(keys, sorted(keys))
        self.assertEqual(len(keys), len(set(keys)))
        self.assertEqual(report, checker.report(REPO_ROOT))

    def test_known_registration_facts(self) -> None:
        """The exact facts slices 35 and 36 originally got wrong."""
        rows = {(r["module"], Path(r["test"]).stem): r for r in checker.report(REPO_ROOT)["tests"]}
        self.assertTrue(rows[("module-runtime", "test_plugin_c_hook")]["ctest"])

    def test_ctest_registration_is_read_from_the_test_subdirectory(self) -> None:
        """The original audit error was reading only the top-level CMakeLists.txt."""
        sources = checker.ctest_sources(REPO_ROOT)
        self.assertIn("src/tests/test_plugin_c_hook.c", sources)

    def test_registration_is_bound_to_the_source_not_the_target_name(self) -> None:
        """Re-pointing a registered target at another source must count as drift."""
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            path = root / checker.CMAKE_TESTS
            path.write_text(
                path.read_text(encoding="utf-8").replace(
                    "aimee_add_test(test_plugin_c_hook test_plugin_c_hook.c)",
                    "aimee_add_test(test_plugin_c_hook test_something_else.c)",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(checker.RegistrationError, "test_plugin_c_hook"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_same_basename_in_another_directory_is_not_the_declared_source(self) -> None:
        """Source identity is the path, not the basename."""
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            path = root / checker.CMAKE_TESTS
            path.write_text(
                path.read_text(encoding="utf-8").replace(
                    "aimee_add_test(test_plugin_c_hook test_plugin_c_hook.c)",
                    "aimee_add_test(test_plugin_c_hook ../other/test_plugin_c_hook.c)",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(checker.RegistrationError, "test_plugin_c_hook"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_a_bracket_commented_source_does_not_count(self) -> None:
        """CMake bracket comments span lines; their contents are not arguments."""
        for opener, closer in (("#[[", "]]"), ("#[=[", "]=]")):
            tmp = self.repo()
            try:
                root = Path(tmp.name)
                path = root / checker.CMAKE_TESTS
                path.write_text(
                    path.read_text(encoding="utf-8").replace(
                        "aimee_add_test(test_plugin_c_hook test_plugin_c_hook.c)",
                        f"aimee_add_test(test_plugin_c_hook\n"
                        f"    {opener}\n    test_plugin_c_hook.c\n    {closer}\n"
                        f"    test_something_else.c)",
                    ),
                    encoding="utf-8",
                )
                with self.subTest(opener=opener), self.assertRaisesRegex(
                    checker.RegistrationError, "test_plugin_c_hook"
                ):
                    checker.check(root)
            finally:
                tmp.cleanup()

    def test_a_commented_out_source_does_not_count(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            path = root / checker.CMAKE_TESTS
            path.write_text(
                path.read_text(encoding="utf-8").replace(
                    "aimee_add_test(test_plugin_c_hook test_plugin_c_hook.c)",
                    "aimee_add_test(test_plugin_c_hook\n"
                    "    # test_plugin_c_hook.c\n"
                    "    test_something_else.c)",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(checker.RegistrationError, "test_plugin_c_hook"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_a_case_running_another_target_binds_to_that_target(self) -> None:
        """add_test COMMAND names the target; the case name need not match it."""
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            path = root / checker.CMAKE_TESTS
            path.write_text(
                path.read_text(encoding="utf-8")
                + "\nadd_executable(audit_worm_bin test_audit_worm.c)\n"
                + "add_test(NAME unrelated_case COMMAND audit_worm_bin)\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(checker.RegistrationError, "test_audit_worm.c"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_an_executable_without_add_test_is_not_registered(self) -> None:
        """Building a test binary is not registering it as a CTest case."""
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            path = root / checker.CMAKE_TESTS
            path.write_text(
                path.read_text(encoding="utf-8") + "\nadd_executable(test_audit_worm test_audit_worm.c)\n",
                encoding="utf-8",
            )
            checker.check(root)
            path.write_text(
                path.read_text(encoding="utf-8")
                + "add_test(NAME test_audit_worm COMMAND test_audit_worm)\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(checker.RegistrationError, "test_audit_worm"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_unbalanced_call_is_a_closed_error(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            path = root / checker.CMAKE_TESTS
            path.write_text(
                path.read_text(encoding="utf-8") + "\nadd_executable(broken broken.c\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(checker.RegistrationError, "unbalanced"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_dropping_a_ctest_registration_fails(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            path = root / checker.CMAKE_TESTS
            path.write_text(
                path.read_text(encoding="utf-8").replace(
                    "aimee_add_test(test_plugin_c_hook test_plugin_c_hook.c)", ""
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(checker.RegistrationError, "drifted"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_adding_a_ctest_registration_fails(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            path = root / checker.CMAKE_TESTS
            path.write_text(
                path.read_text(encoding="utf-8") + "\naimee_add_test(test_audit_worm test_audit_worm.c)\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(checker.RegistrationError, "test_audit_worm"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_dropping_a_make_registration_fails(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            path = root / checker.MAKE_RULES
            path.write_text(
                path.read_text(encoding="utf-8").replace(
                    "$(OBJDIR)/tests/test_gateway_policy.o", "$(OBJDIR)/tests/removed.o"
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(checker.RegistrationError, "test_gateway_policy"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_declaring_a_new_test_fails_until_the_baseline_is_regenerated(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            path = root / checker.MODULES / "gateway/module.yaml"
            value = json.loads(path.read_text(encoding="utf-8"))
            value["tests"].append("src/tests/test_gateway_unregistered.c")
            path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(checker.RegistrationError, "test_gateway_unregistered"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_missing_or_malformed_baseline_is_a_closed_error(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            (root / checker.BASELINE).unlink()
            with self.assertRaisesRegex(checker.RegistrationError, "cannot read"):
                checker.check(root)
            (root / checker.BASELINE).write_text("{", encoding="utf-8")
            with self.assertRaisesRegex(checker.RegistrationError, "not valid JSON"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_missing_build_file_is_a_closed_error(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            (root / checker.CMAKE_TESTS).unlink()
            with self.assertRaisesRegex(checker.RegistrationError, "cannot read"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_write_regenerates_a_matching_baseline(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            (root / checker.BASELINE).write_text('{"schema_version": 1, "tests": []}\n',
                                                 encoding="utf-8")
            with self.assertRaises(checker.RegistrationError):
                checker.check(root)
            self.assertEqual(checker.main(["--root", str(root), "--write"]), 0)
            checker.check(root)
        finally:
            tmp.cleanup()


if __name__ == "__main__":
    unittest.main(verbosity=2)
