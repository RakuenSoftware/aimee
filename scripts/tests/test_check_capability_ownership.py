#!/usr/bin/env python3
"""Mutation tests for the governance capability-ownership contract."""

from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
CHECKER = REPO / "scripts/check_capability_ownership.py"
SPEC = importlib.util.spec_from_file_location("capability_ownership", CHECKER)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = checker
SPEC.loader.exec_module(checker)


class CapabilityOwnershipTests(unittest.TestCase):
    def fixture(self, root: Path) -> None:
        (root / ".git").mkdir()
        for relative in (checker.POLICY, checker.INVENTORY, checker.PROPOSAL):
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / relative, target)
        for source in (REPO / checker.DESCRIPTORS).glob("*/module.yaml"):
            target = root / source.relative_to(REPO)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)

    def json_value(self, root: Path, relative: Path) -> dict[str, object]:
        value = json.loads((root / relative).read_text())
        assert isinstance(value, dict)
        return value

    def write_json(self, root: Path, relative: Path, value: object) -> None:
        (root / relative).write_text(json.dumps(value, indent=2) + "\n")

    def mutate_policy(self, root: Path, change) -> None:
        value = self.json_value(root, checker.POLICY)
        change(value)
        self.write_json(root, checker.POLICY, value)

    def rejected(self, mutate, rule: str) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            mutate(root)
            with self.assertRaisesRegex(checker.OwnershipError, f"rule={rule}"):
                checker.validate(root)

    def policy_rejected(self, change, rule: str) -> None:
        self.rejected(lambda root: self.mutate_policy(root, change), rule)

    def test_repository_fixture_and_real_cwd_independence_pass(self) -> None:
        checker.validate(REPO)
        with tempfile.TemporaryDirectory() as tmp:
            completed = subprocess.run(
                [sys.executable, "-I", "-S", str(CHECKER)], cwd=tmp,
                text=True, capture_output=True, check=False,
            )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stdout.strip(),
                         "check_capability_ownership: ok (13 governance capabilities)")

    def test_policy_key_order_is_irrelevant_but_normative_list_order_is_not(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            value = self.json_value(root, checker.POLICY)
            self.write_json(root, checker.POLICY, dict(reversed(list(value.items()))))
            checker.validate(root)
        for field in ("forbidden_core_shadows", "required_dependencies"):
            with self.subTest(field=field):
                self.policy_rejected(lambda v, name=field: v[name].reverse(),
                                     "semantic-equality")

    def test_proposal_is_the_authority(self) -> None:
        def mutate(root: Path) -> None:
            path = root / checker.PROPOSAL
            path.write_text(path.read_text().replace(
                "  oidc-federation: governance", "  oidc-federation: workflows", 1
            ))
        self.rejected(mutate, "capability-owner")

    def test_missing_extra_and_duplicate_capabilities_fail(self) -> None:
        self.policy_rejected(lambda v: v["capabilities"].pop("oidc-federation"),
                             "capability-missing")
        self.policy_rejected(lambda v: v["capabilities"].update(
            {"provider-mode": "governance"}), "capability-extra")
        def duplicate(root: Path) -> None:
            path = root / checker.POLICY
            path.write_text(path.read_text().replace(
                '"oidc-federation": "governance",',
                '"oidc-federation": "governance",\n    "oidc-federation": "governance",',
            ))
        self.rejected(duplicate, "duplicate-key")

    def test_unknown_core_and_optional_owners_fail_distinctly(self) -> None:
        self.policy_rejected(lambda v: v["capabilities"].update(
            {"oidc-federation": "unknown"}), "unknown-module")
        self.policy_rejected(lambda v: v["capabilities"].update(
            {"oidc-federation": "config"}), "core-shadow")
        self.policy_rejected(lambda v: v["capabilities"].update(
            {"oidc-federation": "workflows"}), "capability-owner")

    def test_normative_lists_require_membership_order_and_uniqueness(self) -> None:
        for field in ("forbidden_core_shadows", "required_dependencies",
                      "forbidden_dependencies_from_core"):
            with self.subTest(field=field, mutation="missing"):
                self.policy_rejected(lambda v, name=field: v[name].pop(),
                                     "semantic-equality")
            with self.subTest(field=field, mutation="duplicate"):
                self.policy_rejected(lambda v, name=field: v[name].append(v[name][0]),
                                     "duplicate-entry")

    def test_schema_module_and_classification_failures(self) -> None:
        self.policy_rejected(lambda v: v.update({"schema_version": 2}), "schema-version")
        self.policy_rejected(lambda v: v.update({"module": "workflows"}), "module")
        def classification(root: Path) -> None:
            value = self.json_value(root, checker.INVENTORY)
            value["optional"].remove("governance")
            self.write_json(root, checker.INVENTORY, value)
        self.rejected(classification, "classification")

    def test_descriptor_structure_unknown_edge_and_missing_descriptor_fail(self) -> None:
        def descriptor_mutation(module: str, change):
            def mutate(root: Path) -> None:
                relative = checker.DESCRIPTORS / module / "module.yaml"
                value = self.json_value(root, relative)
                change(value)
                self.write_json(root, relative, value)
            return mutate
        self.rejected(descriptor_mutation("config", lambda v: v.pop("dependencies")),
                      "structure")
        self.rejected(descriptor_mutation("config", lambda v: v["dependencies"].append(1)),
                      "structure")
        self.rejected(descriptor_mutation("config", lambda v: v["dependencies"].append(
            "unknown-module")), "unknown-module")
        self.rejected(lambda root: (root / checker.DESCRIPTORS /
                                    "config/module.yaml").unlink(), "missing-descriptor")

    def test_required_governance_and_forbidden_core_edges_fail(self) -> None:
        def missing(root: Path) -> None:
            relative = checker.DESCRIPTORS / "governance/module.yaml"
            value = self.json_value(root, relative)
            value["dependencies"].remove("vault")
            self.write_json(root, relative, value)
        self.rejected(missing, "required-dependency")
        def core_edge(root: Path) -> None:
            relative = checker.DESCRIPTORS / "config/module.yaml"
            value = self.json_value(root, relative)
            value["dependencies"].append("governance")
            self.write_json(root, relative, value)
        self.rejected(core_edge, "core-to-governance")

    def test_unknown_keys_non_json_constants_and_yaml_aliases_fail(self) -> None:
        self.policy_rejected(lambda v: v.update({"aliases": []}), "structure")
        def nan(root: Path) -> None:
            path = root / checker.POLICY
            path.write_text(path.read_text().replace('"schema_version": 1',
                                                     '"schema_version": NaN'))
        self.rejected(nan, "parse")
        def alias(root: Path) -> None:
            path = root / checker.POLICY
            path.write_text(
                "schema_version: 1\nmodule: governance\ncapabilities:\n"
                "  oidc-federation: &owner governance\n"
                "  sso-federation: *owner\n"
            )
        self.rejected(alias, "parse")

    def test_cli_failure_is_nonzero_and_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            self.mutate_policy(root, lambda v: v["capabilities"].pop("oidc-federation"))
            command = [sys.executable, "-I", "-S", str(CHECKER),
                       "--config-root", str(root)]
            first = subprocess.run(command, text=True, capture_output=True, check=False)
            second = subprocess.run(command, text=True, capture_output=True, check=False)
        self.assertEqual(first.returncode, 1)
        self.assertEqual(first.stdout, "")
        self.assertEqual(first.stderr, second.stderr)
        self.assertIn("rule=capability-missing: oidc-federation", first.stderr)

    @unittest.skipIf(os.name == "nt", "symlink permissions vary on Windows")
    def test_symlinked_descriptor_escape_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as outside:
            root = Path(tmp)
            self.fixture(root)
            target = root / checker.DESCRIPTORS / "config/module.yaml"
            target.unlink()
            target.symlink_to(Path(outside) / "module.yaml")
            (Path(outside) / "module.yaml").write_text('{}')
            with self.assertRaisesRegex(checker.OwnershipError, "rule=path-escape"):
                checker.validate(root)


if __name__ == "__main__":
    unittest.main()
