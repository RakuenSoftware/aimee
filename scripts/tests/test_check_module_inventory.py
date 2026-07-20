#!/usr/bin/env python3
"""Focused tests for the canonical module-inventory gate."""

from __future__ import annotations

import copy
import json
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "scripts" / "check_module_inventory.sh"
BASELINE = ROOT / "tests" / "baselines" / "modules" / "canonical-inventory.yaml"


class ModuleInventoryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.baseline = json.loads(BASELINE.read_text(encoding="utf-8"))

    def run_checker(self, content: str | dict[str, object], *, cwd: Path | None = None):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inventory = root / "inventory.yaml"
            if isinstance(content, str):
                inventory.write_text(content, encoding="utf-8")
            else:
                inventory.write_text(json.dumps(content, indent=2), encoding="utf-8")
            return subprocess.run(
                [str(CHECKER), "--inventory", str(inventory)],
                cwd=cwd or ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

    def changed(self) -> dict[str, object]:
        return copy.deepcopy(self.baseline)

    def assert_failed(self, result: subprocess.CompletedProcess[str], *needles: str) -> None:
        self.assertNotEqual(result.returncode, 0, result.stdout)
        for needle in needles:
            self.assertIn(needle, result.stderr)

    def test_valid_baseline_from_unrelated_cwd(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = subprocess.run(
                [str(CHECKER)],
                cwd=temporary,
                text=True,
                capture_output=True,
                check=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("18 required, 8 optional", result.stdout)

    def test_missing_inventory_fails_closed(self):
        result = subprocess.run(
            [str(CHECKER), "--inventory", "/definitely/missing/inventory.yaml"],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assert_failed(result, "rule=input", "cannot read inventory")

    def test_malformed_inventory_fails_closed(self):
        self.assert_failed(self.run_checker("{not-json"), "rule=parse")

    def test_unsafe_yaml_tag_is_not_executed(self):
        with tempfile.TemporaryDirectory() as temporary:
            marker = Path(temporary) / "executed"
            payload = f"!!python/object/apply:os.system ['touch {marker}']\n"
            self.assert_failed(self.run_checker(payload), "rule=parse", "safe JSON-compatible YAML")
            self.assertFalse(marker.exists())

    def test_required_count_drift(self):
        data = self.changed()
        data["required"].append("unexpected-module")
        self.assert_failed(self.run_checker(data), "rule=required-count", "expected 18", "actual 19")

    def test_optional_count_drift(self):
        data = self.changed()
        data["optional"].pop()
        self.assert_failed(self.run_checker(data), "rule=optional-count", "expected 8", "actual 7")

    def test_duplicate_id(self):
        data = self.changed()
        data["required"][-1] = data["required"][0]
        self.assert_failed(self.run_checker(data), "rule=unique-ids", data["required"][0])

    def test_required_optional_overlap(self):
        data = self.changed()
        data["optional"][0] = data["required"][0]
        self.assert_failed(self.run_checker(data), "rule=disjoint-sets", data["required"][0])

    def test_git_missing(self):
        data = self.changed()
        index = data["required"].index("git")
        data["required"][index] = "replacement-module"
        self.assert_failed(self.run_checker(data), "rule=required-classification", "git", "actual absent")

    def test_git_reclassified_optional(self):
        data = self.changed()
        required_index = data["required"].index("git")
        optional_index = 0
        data["required"][required_index], data["optional"][optional_index] = (
            data["optional"][optional_index],
            "git",
        )
        self.assert_failed(self.run_checker(data), "rule=required-classification", "git", "actual optional")

    def test_invalid_module_id(self):
        data = self.changed()
        data["optional"][0] = "Bad(Module)"
        self.assert_failed(self.run_checker(data), "rule=module-id-syntax", "Bad(Module)")

    def test_unknown_key(self):
        data = self.changed()
        data["historical"] = ["git"]
        self.assert_failed(self.run_checker(data), "rule=structure", "unknown keys", "historical")


if __name__ == "__main__":
    unittest.main()
