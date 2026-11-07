"""Suite registration must agree with Make's frozen execution list."""
import importlib.util
import os
from pathlib import Path
import subprocess
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location(
    "check_tests_are_run", Path(__file__).resolve().parents[1] / "check_tests_are_run.py"
)
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


class SuiteSelectionTest(unittest.TestCase):
    def make_selection(self, rules):
        result = subprocess.run(
            ["make", "--no-print-directory", "-s", "-f", "-", "print"],
            input=rules + "\nprint:\n\t@printf '%s\\n' $(UNIT_TEST_TARGETS)\n",
            text=True, capture_output=True, check=True,
        )
        return set(result.stdout.split())

    def test_registration_before_freeze_runs(self):
        rules = (
            "TEST_TARGETS = unit-test-first\n"
            "TEST_TARGETS += unit-test-second\n"
            "UNIT_TEST_TARGETS := $(TEST_TARGETS)\n"
        )
        self.assertEqual(CHECK.selected_tests(rules), self.make_selection(rules))
        self.assertEqual(CHECK.selected_tests(rules), {"unit-test-first", "unit-test-second"})

    def test_nested_make_banners_do_not_become_test_names(self):
        rules = ("TEST_TARGETS = unit-test-first\n"
                 "UNIT_TEST_TARGETS := $(TEST_TARGETS)\n")
        # CI's script-tests lane is launched by make -C, unlike direct Python
        # execution. Preserve the real recursive-Make environment that exposed
        # the failure and keep checking exact stdout, not a filtered subset.
        with mock.patch.dict(os.environ, {"MAKELEVEL": "1", "MAKEFLAGS": "w"}):
            self.assertEqual(self.make_selection(rules), {"unit-test-first"})

    def test_late_registration_is_rejected_even_though_make_accepts_it(self):
        rules = (
            "TEST_TARGETS = unit-test-first\n"
            "UNIT_TEST_TARGETS := $(TEST_TARGETS)\n"
            "TEST_TARGETS += unit-test-missing\n"
        )
        self.assertEqual(self.make_selection(rules), {"unit-test-first"})
        with self.assertRaisesRegex(ValueError, "after UNIT_TEST_TARGETS is frozen"):
            CHECK.selected_tests(rules)

    def test_late_multiline_assignment_is_rejected(self):
        rules = (
            "TEST_TARGETS = unit-test-first\n"
            "UNIT_TEST_TARGETS := $(TEST_TARGETS)\n"
            "TEST_TARGETS += \\\n unit-test-missing\n"
        )
        with self.assertRaises(ValueError):
            CHECK.selected_tests(rules)

    def test_target_definition_may_follow_freeze(self):
        rules = (
            "TEST_TARGETS = unit-test-first\n"
            "UNIT_TEST_TARGETS := $(TEST_TARGETS)\n"
            "unit-test-first: source.c\n"
        )
        self.assertEqual(CHECK.selected_tests(rules), {"unit-test-first"})

    def test_unknown_selection_refuses(self):
        with self.assertRaisesRegex(ValueError, "cannot locate"):
            CHECK.selected_tests("TEST_TARGETS = unit-test-first\n")

    def test_shard_snapshot_also_rejects_late_registration(self):
        rules = (
            "TEST_TARGETS = unit-test-first\n"
            "UNIT_TEST_TARGETS := $(shell printf '%s\\n' $(TEST_TARGETS))\n"
            "TEST_TARGETS += unit-test-missing\n"
        )
        with self.assertRaises(ValueError):
            CHECK.selected_tests(rules)


class OrphanSourceTest(unittest.TestCase):
    def test_a_longer_target_does_not_hide_a_shorter_source(self):
        rules = "unit-test-kb-store: $(OBJDIR)/tests/test_kb_store.o\n"
        self.assertTrue(CHECK.source_is_named("test_kb_store", rules))
        self.assertFalse(CHECK.source_is_named("test_kb", rules))

    def test_object_and_source_spellings_count(self):
        self.assertTrue(CHECK.source_is_named("test_kb", "x: $(OBJDIR)/tests/test_kb.o\n"))
        self.assertTrue(CHECK.source_is_named("test_kb", "SRC = tests/test_kb.c\n"))


if __name__ == "__main__":
    unittest.main()
