#!/usr/bin/env python3
"""The principal ref is an identity, not a position.

A module's ref is permanent: grants match on it and event kinds are carved from
it. It used to be derived from position in the canonical inventory, which welded
"is this module essential" to "which event kinds does it own" -- promoting a
module to required renumbered every module after it, moving ten modules' event
kinds for a change that is nothing at runtime.

These tests pin the properties that make the ref an identity instead.
"""

from __future__ import annotations

import json
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INVENTORY = ROOT / "tests/baselines/modules/canonical-inventory.yaml"
CONTRACTS = ROOT / "src/modules/process-contracts.json"
VALIDATOR = ROOT / "scripts/validate_module_process_contracts.py"


def load(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def run_validator() -> subprocess.CompletedProcess:
    return subprocess.run(
        ["python3", "-I", "-S", str(VALIDATOR)], capture_output=True, text=True, cwd=str(ROOT)
    )


class PrincipalRefIsAnIdentity(unittest.TestCase):
    def setUp(self) -> None:
        self.original = INVENTORY.read_text(encoding="utf-8")
        self.addCleanup(lambda: INVENTORY.write_text(self.original, encoding="utf-8"))
        self.inventory = load(INVENTORY)

    def write(self, data) -> None:
        INVENTORY.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")

    def test_refs_are_declared_for_every_module(self) -> None:
        refs = self.inventory["principal_refs"]
        ids = self.inventory["required"] + self.inventory["optional"]
        self.assertEqual(set(refs), set(ids), "every module must declare a ref")
        self.assertEqual(len(set(refs.values())), len(refs), "refs must be unique")

    def test_event_kinds_are_carved_from_the_declared_ref(self) -> None:
        refs = self.inventory["principal_refs"]
        for component in load(CONTRACTS)["components"]:
            if component.get("execution") != "process":
                continue
            ref = refs[component["id"]]
            self.assertEqual(component["principal_ref"], ref)
            for stage in component["stages"]:
                self.assertEqual(
                    stage["event_kind"],
                    4096 + ref * 256 + stage["id"],
                    f"{component['id']}/{stage['name']} must be carved from its declared ref",
                )

    def test_classification_change_does_not_move_any_ref(self) -> None:
        """The regression this whole change exists to prevent.

        Promoting a module from optional to required is a policy decision. It
        must not be an event-kind migration for every module that follows it.
        """
        data = load(INVENTORY)
        before = dict(data["principal_refs"])
        moved = data["optional"].pop()
        data["required"].append(moved)
        self.write(data)

        after = load(INVENTORY)["principal_refs"]
        self.assertEqual(before, after, f"promoting {moved} must not renumber anything")

    def test_a_retired_ref_is_never_reissued(self) -> None:
        data = load(INVENTORY)
        victim = data["optional"][-1]
        retired_ref = data["principal_refs"][victim]
        data["retired_principal_refs"] = [retired_ref]
        self.write(data)

        result = run_validator()
        self.assertNotEqual(result.returncode, 0, "a retired ref in use must be rejected")
        self.assertIn("retired", (result.stdout + result.stderr).lower())

    def test_a_duplicate_ref_is_rejected(self) -> None:
        data = load(INVENTORY)
        a, b = data["optional"][0], data["optional"][1]
        data["principal_refs"][b] = data["principal_refs"][a]
        self.write(data)

        result = run_validator()
        self.assertNotEqual(result.returncode, 0, "two modules must not share an identity")

    def test_the_shipped_inventory_validates(self) -> None:
        self.assertEqual(run_validator().returncode, 0)

    def test_c_build_is_reserved_for_c_processes(self) -> None:
        for identifier in ("memory", "config"):
            descriptor_path = ROOT / f"src/modules/{identifier}/module.yaml"
            original = descriptor_path.read_text(encoding="utf-8")
            try:
                descriptor = json.loads(original)
                descriptor["c_build"] = {
                    "include_roots": ["src"],
                    "pkg_config": [],
                    "system_libraries": [],
                }
                descriptor_path.write_text(
                    json.dumps(descriptor, indent=2) + "\n", encoding="utf-8"
                )
                result = run_validator()
                with self.subTest(identifier=identifier):
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("c_build", result.stderr)
            finally:
                descriptor_path.write_text(original, encoding="utf-8")


if __name__ == "__main__":
    unittest.main()
