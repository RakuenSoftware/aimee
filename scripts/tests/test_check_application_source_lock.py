#!/usr/bin/env python3
"""Release locks reject drift without rewriting independent repository pins."""

import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "application_lock", ROOT / "scripts/check_application_source_lock.py"
)
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class ApplicationLockTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.expected = checker.snapshot()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.lock = Path(self.temp.name) / "lock.json"

    def write(self, value):
        self.lock.write_text(json.dumps(value), encoding="utf-8")

    def test_current_sources_pass_without_independent_pin_refresh(self):
        before = checker.exporter.LOCK.read_bytes()
        self.write(self.expected)
        checker.check(self.lock)
        self.assertEqual(before, checker.exporter.LOCK.read_bytes())
        self.assertTrue(any(m["source"] == "external" for m in self.expected["modules"]))
        for module in self.expected["modules"]:
            if module["source"] == "bundled":
                self.assertNotIn("commit", module)
                self.assertNotIn("repository", module)

    def test_snapshot_drift_fails(self):
        mutations = {
            "core content": lambda v: v["core"].update(source_sha256="0" * 64),
            "core version": lambda v: v["core"].update(version="999"),
            "missing module": lambda v: v["modules"].pop(),
            "duplicate module": lambda v: v["modules"].append(v["modules"][0]),
            "module content": lambda v: v["modules"][0].update(source_sha256="0" * 64),
            "classification": lambda v: v["modules"][0].update(classification="unknown"),
            "placement": lambda v: v["modules"][0].update(placements=[]),
            "schema": lambda v: v.update(schema_version=2),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                value = copy.deepcopy(self.expected)
                mutate(value)
                self.write(value)
                with self.assertRaisesRegex(checker.exporter.ExportError, "snapshot is stale"):
                    checker.check(self.lock)

    def test_process_identity_and_grants_drift_fails(self):
        for field, value in (("principal_ref", 999), ("serve", []), ("runtime", "unknown")):
            with self.subTest(field=field):
                changed = copy.deepcopy(self.expected)
                process = next(m for m in changed["modules"] if m["execution"] == "process")
                process[field] = value
                self.write(changed)
                with self.assertRaises(checker.exporter.ExportError):
                    checker.check(self.lock)

    def test_new_core_file_requires_review(self):
        self.write(self.expected)
        # Use an actual additional file: path and bytes must affect the digest.
        with tempfile.TemporaryDirectory(dir=ROOT) as directory:
            extra = Path(directory) / "new.c"
            extra.write_text("int new_core_api;\n", encoding="utf-8")
            files = checker.exporter.core_files()
            with mock.patch.object(checker.exporter, "core_files", return_value=files + [extra]):
                with self.assertRaisesRegex(checker.exporter.ExportError, "snapshot is stale"):
                    checker.check(self.lock)

    def test_freeze_cannot_accept_stale_or_duplicate_external_pin(self):
        original = checker.exporter.load_json
        for duplicate in (False, True):
            with self.subTest(duplicate=duplicate):
                def load(path):
                    value = original(path)
                    if path == checker.exporter.LOCK:
                        pin = next(m for m in value["modules"] if m["id"] == "config")
                        if duplicate:
                            value["modules"].append(copy.deepcopy(pin))
                        else:
                            pin["commit"] = "0" * 40
                    return value
                with mock.patch.object(checker.exporter, "load_json", side_effect=load):
                    with self.assertRaisesRegex(checker.exporter.ExportError, "external repository pin is stale"):
                        checker.snapshot()

    def test_malformed_or_missing_lock_fails(self):
        with self.assertRaises(checker.exporter.ExportError):
            checker.check(self.lock)
        self.lock.write_text("{", encoding="utf-8")
        with self.assertRaises(checker.exporter.ExportError):
            checker.check(self.lock)


if __name__ == "__main__":
    unittest.main()
