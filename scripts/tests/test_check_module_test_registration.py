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
        for relative in (checker.MAKE_RULES, checker.BASELINE):
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
        """Tests CTest alone used to register now register under Make."""
        rows = {(r["module"], Path(r["test"]).stem): r for r in checker.report(REPO_ROOT)["tests"]}
        for key in (("audit", "test_audit_action"), ("git", "test_git_org_repos")):
            with self.subTest(key=key):
                self.assertTrue(rows[key]["make"])
                self.assertEqual(set(rows[key]), {"module", "test", "make"})

    def test_same_basename_in_another_directory_is_not_the_declared_source(self) -> None:
        """Source identity is the path under src/tests, not the basename."""
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            path = root / checker.MAKE_RULES
            path.write_text(
                path.read_text(encoding="utf-8").replace(
                    "$(OBJDIR)/tests/test_gateway_policy.o", "$(OBJDIR)/other/test_gateway_policy.o"
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(checker.RegistrationError, "test_gateway_policy"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_adding_a_make_registration_fails(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            path = root / checker.MAKE_RULES
            path.write_text(
                path.read_text(encoding="utf-8")
                + "\nunit-test-x: $(OBJDIR)/tests/test_postgres_local_session.o\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(checker.RegistrationError, "test_postgres_local_session"):
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
            (root / checker.MAKE_RULES).unlink()
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
