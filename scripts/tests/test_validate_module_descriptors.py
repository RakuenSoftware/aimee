#!/usr/bin/env python3
"""Tests for the descriptor-v1 envelope and taxonomy convergence."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "scripts/validate_module_descriptors.py"
SPEC = importlib.util.spec_from_file_location("validate_module_descriptors", CHECKER)
assert SPEC and SPEC.loader
validator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(validator)


class DescriptorTests(unittest.TestCase):
    def required(self) -> dict[str, object]:
        return {
            "descriptor_version": 1,
            "id": "memory",
            "dependencies": ["config", "ir"],
            "runtime_toggle": {"supported": False},
        }

    def optional(self) -> dict[str, object]:
        return {
            "descriptor_version": 1,
            "id": "runtime-web",
            "dependencies": ["config", "gateway"],
            "enabled_by_default": True,
            "runtime_toggle": {"supported": True},
        }

    def taxonomy(self) -> tuple[set[str], set[str]]:
        return validator.load_inventory(REPO_ROOT)

    def assert_rule(self, value: object, rule: str) -> None:
        required, optional = self.taxonomy()
        with self.assertRaisesRegex(validator.DescriptorError, f"rule={rule}"):
            validator.validate_descriptor(value, required, optional)

    def write_raw(self, raw: bytes) -> tuple[tempfile.TemporaryDirectory[str], Path]:
        tmp = tempfile.TemporaryDirectory()
        path = Path(tmp.name) / "value.json"
        path.write_bytes(raw)
        return tmp, path

    def test_positive_fixture_root_and_exact_count(self) -> None:
        count = validator.validate_roots(
            REPO_ROOT, [Path("tests/fixtures/modules/positive")], allow_empty=False
        )
        self.assertEqual(count, 3)

    def test_schema_is_generated_byte_for_byte(self) -> None:
        validator.check_schema(REPO_ROOT)
        parsed = json.loads(validator.schema_bytes())
        self.assertEqual(parsed, validator.schema())
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            path = repo / validator.SCHEMA_PATH
            path.parent.mkdir(parents=True)
            path.write_text("{}\n", encoding="utf-8")
            with self.assertRaisesRegex(validator.DescriptorError, "rule=schema-drift"):
                validator.check_schema(repo)

    def test_schema_keyword_subset_is_closed_and_exercised(self) -> None:
        allowed = {
            "$schema", "$id", "$comment", "type", "additionalProperties", "required",
            "properties", "const", "pattern", "items", "uniqueItems",
        }

        def keys(value: object) -> set[str]:
            if isinstance(value, dict):
                found = set(value) - set(value.get("properties", {}))
                for key, item in value.items():
                    if key == "properties":
                        for subschema in item.values():
                            found |= keys(subschema)
                        continue
                    found |= keys(item)
                return found
            if isinstance(value, list):
                found: set[str] = set()
                for item in value:
                    found |= keys(item)
                return found
            return set()

        self.assertEqual(keys(validator.schema()), allowed)

    def test_inventory_is_the_only_taxonomy(self) -> None:
        required, optional = self.taxonomy()
        raw = json.loads((REPO_ROOT / validator.INVENTORY_PATH).read_text())
        self.assertEqual(required, set(raw["required"]))
        self.assertEqual(optional, set(raw["optional"]))
        self.assertFalse(required & optional)
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            path = repo / validator.INVENTORY_PATH
            path.parent.mkdir(parents=True)
            path.write_text(
                json.dumps({"schema_version": True, "required": ["memory"], "optional": ["x"]}),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(validator.DescriptorError, "rule=inventory-version"):
                validator.load_inventory(repo)

    def test_descriptor_key_and_version_mutations(self) -> None:
        missing = self.required()
        del missing["dependencies"]
        self.assert_rule(missing, "descriptor-keys")
        extra = self.required()
        extra["routes"] = []
        self.assert_rule(extra, "descriptor-keys")
        version = self.required()
        version["descriptor_version"] = 2
        self.assert_rule(version, "descriptor-version")

    def test_module_identity_mutations(self) -> None:
        for value in ("Memory", "memory_", "memory/child", "a" * 65):
            descriptor = self.required()
            descriptor["id"] = value
            with self.subTest(value=value):
                self.assert_rule(descriptor, "module-id")
        unknown = self.required()
        unknown["id"] = "unknown-module"
        self.assert_rule(unknown, "module-unknown")

    def test_dependency_mutations(self) -> None:
        cases = (
            (["ir", "config"], "dependency-order"),
            (["config", "config"], "dependency-duplicate"),
            (["memory"], "dependency-self"),
            (["unknown-module"], "dependency-unknown"),
        )
        for dependencies, rule in cases:
            descriptor = self.required()
            descriptor["dependencies"] = dependencies
            with self.subTest(rule=rule):
                self.assert_rule(descriptor, rule)

    def test_selection_and_runtime_toggle_mutations(self) -> None:
        required_default = self.required()
        required_default["enabled_by_default"] = True
        self.assert_rule(required_default, "descriptor-keys")
        required_toggle = self.required()
        required_toggle["runtime_toggle"]["supported"] = True
        self.assert_rule(required_toggle, "required-runtime-toggle")
        optional_missing = self.optional()
        del optional_missing["enabled_by_default"]
        self.assert_rule(optional_missing, "descriptor-keys")
        optional_type = self.optional()
        optional_type["enabled_by_default"] = None
        self.assert_rule(optional_type, "default-type")
        runtime_empty = self.required()
        runtime_empty["runtime_toggle"] = {}
        self.assert_rule(runtime_empty, "runtime-toggle-shape")

    def test_duplicate_module_id_fails_across_roots(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ("one", "two"):
                path = root / name / "module.yaml"
                path.parent.mkdir()
                path.write_text(json.dumps(self.required()), encoding="utf-8")
            with self.assertRaisesRegex(validator.DescriptorError, "rule=module-duplicate"):
                validator.validate_roots(REPO_ROOT, [root], allow_empty=False)

    def test_empty_root_requires_explicit_allow_empty(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(validator.DescriptorError, "rule=no-descriptors-found"):
                validator.validate_roots(REPO_ROOT, [root], allow_empty=False)
            with self.assertRaisesRegex(validator.DescriptorError, "rule=allow-empty-scope"):
                validator.validate_roots(REPO_ROOT, [root], allow_empty=True)
        self.assertEqual(
            validator.validate_roots(REPO_ROOT, [Path("src/modules")], allow_empty=True), 0
        )

    def test_parser_resource_limits(self) -> None:
        tmp, path = self.write_raw(b" " * (validator.MAX_BYTES + 1))
        try:
            with self.assertRaisesRegex(validator.DescriptorError, "rule=input-size"):
                validator.load_json(path)
        finally:
            tmp.cleanup()
        nested: object = True
        for _ in range(validator.MAX_DEPTH + 1):
            nested = [nested]
        with self.assertRaisesRegex(validator.DescriptorError, "rule=json-depth"):
            validator._check_domain(nested)
        with self.assertRaisesRegex(validator.DescriptorError, "rule=json-array-size"):
            validator._check_domain([None] * (validator.MAX_ARRAY + 1))

    def test_strict_json_rejections(self) -> None:
        cases = (
            (b"{\"a\":1,\"a\":2}", "json-duplicate-key"),
            (b"{\"a\":{\"b\":1,\"b\":2}}", "json-duplicate-key"),
            (b"\xef\xbb\xbf{}", "json-bom"),
            (b"{# comment\n}", "json-parse"),
            (b"{a: 1}", "json-parse"),
            (b"{\"a\": .NaN}", "json-parse"),
            (b"{\"a\": NaN}", "json-number-domain"),
            (b"{\"a\": 1.0}", "json-number-domain"),
            (b"{\"a\": 1e2}", "json-number-domain"),
            (b"{} trailing", "json-parse"),
            (b"---\na: on\n", "json-parse"),
        )
        for raw, rule in cases:
            tmp, path = self.write_raw(raw)
            try:
                with self.subTest(raw=raw), self.assertRaisesRegex(
                    validator.DescriptorError, f"rule={rule}"
                ):
                    validator.load_json(path)
            finally:
                tmp.cleanup()

    def test_crlf_is_valid_and_surrogate_is_rejected(self) -> None:
        tmp, path = self.write_raw(b'{\r\n  "value": true\r\n}\r\n')
        try:
            self.assertEqual(validator.load_json(path), {"value": True})
        finally:
            tmp.cleanup()
        tmp, path = self.write_raw(b'{"value":"\\ud800"}')
        try:
            with self.assertRaisesRegex(validator.DescriptorError, "json-surrogate"):
                validator.load_json(path)
        finally:
            tmp.cleanup()


if __name__ == "__main__":
    unittest.main()
