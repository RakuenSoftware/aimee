#!/usr/bin/env python3
"""Focused tests for the canonical module-inventory gate."""

from __future__ import annotations

import copy
import json
import os
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

    def run_checker(self, content: bytes | str | dict[str, object], *, cwd: Path | None = None):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inventory = root / "inventory.yaml"
            if isinstance(content, bytes):
                inventory.write_bytes(content)
            elif isinstance(content, str):
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
        self.assertIn(f"{len(self.baseline['required'])} required", result.stdout)
        self.assertIn(f"{len(self.baseline['optional'])} optional", result.stdout)

    def test_config_root_precedence_and_relative_inventory(self):
        with tempfile.TemporaryDirectory() as cli_temp, tempfile.TemporaryDirectory() as env_temp:
            cli_root = Path(cli_temp)
            env_root = Path(env_temp)
            relative = Path("nested") / "inventory.yaml"
            (cli_root / relative.parent).mkdir()
            (env_root / relative.parent).mkdir()
            text = json.dumps(self.baseline)
            (cli_root / relative).write_text(text, encoding="utf-8")
            (env_root / relative).write_text("invalid", encoding="utf-8")
            environment = os.environ.copy()
            environment["AIMEE_CONFIG_ROOT"] = str(env_root)
            result = subprocess.run(
                [str(CHECKER), "--config-root", str(cli_root), "--inventory", str(relative)],
                cwd=env_root,
                env=environment,
                text=True,
                capture_output=True,
                check=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_parent_traversal_is_resolved_from_config_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            child = root / "child"
            child.mkdir()
            inventory = root / "inventory.yaml"
            inventory.write_text(json.dumps(self.baseline), encoding="utf-8")
            result = subprocess.run(
                [str(CHECKER), "--config-root", str(child), "--inventory", "../inventory.yaml"],
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr)

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

    def test_invalid_utf8_fails_closed(self):
        self.assert_failed(self.run_checker(b"{\xff}"), "rule=input", "cannot read inventory")

    def test_duplicate_object_key_fails_closed(self):
        raw = '{"schema_version": 1, "schema_version": 1, "required": [], "optional": []}'
        self.assert_failed(self.run_checker(raw), "rule=structure", "duplicate object key")

    def test_schema_version_requires_exact_integer(self):
        for value in (True, 1.0, "1", None):
            with self.subTest(value=value):
                data = self.changed()
                data["schema_version"] = value
                self.assert_failed(self.run_checker(data), "rule=schema-version")

    def test_missing_key(self):
        data = self.changed()
        del data["optional"]
        self.assert_failed(self.run_checker(data), "rule=structure", "missing keys", "optional")

    def test_non_array_group(self):
        data = self.changed()
        data["required"] = None
        self.assert_failed(self.run_checker(data), "rule=structure", "actual NoneType")

    def test_non_string_entry(self):
        data = self.changed()
        data["optional"][0] = 7
        self.assert_failed(self.run_checker(data), "rule=structure", "every optional entry")

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
        data["required"].append(data["required"][0])
        self.assert_failed(self.run_checker(data), "rule=unique-ids", data["required"][0])

    def test_required_optional_overlap(self):
        data = self.changed()
        data["optional"].append(data["required"][0])
        self.assert_failed(self.run_checker(data), "rule=disjoint-sets", data["required"][0])

    def test_id_syntax_precedes_forbidden_module_rule(self):
        data = self.changed()
        data["optional"][0] = "Bad(Module)"
        result = self.run_checker(data)
        self.assert_failed(result, "rule=module-id-syntax")
        self.assertNotIn("rule=forbidden-module", result.stderr)

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
