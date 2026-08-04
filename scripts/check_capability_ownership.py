#!/usr/bin/env python3
"""Enforce the proposal-authored governance capability-ownership boundary."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
from typing import NoReturn


REPO = Path(__file__).resolve().parent.parent
POLICY = Path("tests/baselines/modules/governance-ownership.yaml")
INVENTORY = Path("tests/baselines/modules/canonical-inventory.yaml")
DESCRIPTORS = Path("src/modules")
PROPOSAL = Path("docs/proposals/done/module-runtime-source-ownership-and-build.md")
PROPOSAL_MARKER = "The following is the normative semantic content of that future artifact"
MODULE_ID = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")
CONTRACT_KEY = re.compile(r"^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)*$")
MAX_BYTES = 1_048_576
POLICY_KEYS = {
    "schema_version",
    "module",
    "capabilities",
    "forbidden_core_shadows",
    "required_dependencies",
    "forbidden_dependencies_from_core",
}


class OwnershipError(ValueError):
    """A deterministic, operator-readable ownership failure."""


def fail(rule: str, detail: str, path: Path | None = None) -> NoReturn:
    prefix = f"{path}: " if path else ""
    raise OwnershipError(f"{prefix}rule={rule}: {detail}")


def root_path(root: Path, relative: Path) -> Path:
    resolved_root = root.resolve()
    if not (resolved_root / ".git").exists():
        fail("config-root", f"not a repository root: {resolved_root}")
    candidate = (resolved_root / relative).resolve()
    try:
        candidate.relative_to(resolved_root)
    except ValueError:
        fail("path-escape", f"{relative} escapes {resolved_root}")
    return candidate


def read_bytes(path: Path) -> bytes:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        fail("input", f"cannot read: {exc}", path)
    if len(raw) > MAX_BYTES:
        fail("input-size", f"exceeds {MAX_BYTES} bytes", path)
    return raw


def decode(path: Path) -> str:
    try:
        return read_bytes(path).decode("utf-8", "strict")
    except UnicodeDecodeError as exc:
        fail("encoding", f"not strict UTF-8: {exc}", path)


def load_json(path: Path) -> object:
    def no_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                fail("duplicate-key", f"duplicate object key {key!r}", path)
            result[key] = value
        return result

    def reject_constant(value: str) -> NoReturn:
        fail("parse", f"non-JSON constant {value!r}", path)

    try:
        return json.loads(
            decode(path), object_pairs_hook=no_duplicates, parse_constant=reject_constant
        )
    except json.JSONDecodeError as exc:
        fail("parse", f"strict JSON-compatible YAML required at {exc.lineno}:{exc.colno}", path)


def string_list(value: object, name: str, path: Path) -> list[str]:
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        fail("structure", f"{name} must be an array of strings", path)
    if len(value) != len(set(value)):
        fail("duplicate-entry", f"{name} contains a duplicate", path)
    return value


def parse_proposal_contract(path: Path) -> dict[str, object]:
    text = decode(path)
    marker = text.find(PROPOSAL_MARKER)
    if marker < 0:
        fail("proposal-block", "normative marker is missing", path)
    start = text.find("```yaml\n", marker)
    if start < 0:
        fail("proposal-block", "normative YAML fence is missing", path)
    start += len("```yaml\n")
    end = text.find("\n```", start)
    if end < 0:
        fail("proposal-block", "normative YAML fence is unterminated", path)
    block = text[start:end]

    result: dict[str, object] = {}
    section = ""
    for line_no, line in enumerate(block.splitlines(), 1):
        if not line.strip():
            continue
        if line.startswith("  - "):
            if section not in {
                "forbidden_core_shadows",
                "required_dependencies",
                "forbidden_dependencies_from_core",
            }:
                fail("proposal-parse", f"unexpected list item at block line {line_no}", path)
            item = line[4:]
            if not CONTRACT_KEY.fullmatch(item):
                fail("proposal-parse", f"invalid list item {item!r} at block line {line_no}", path)
            assert isinstance(result[section], list)
            result[section].append(item)
            continue
        if line.startswith("  "):
            if section != "capabilities" or not line.startswith("  "):
                fail("proposal-parse", f"unexpected indentation at block line {line_no}", path)
            match = re.fullmatch(r"  ([a-z][a-z0-9]*(?:[.-][a-z0-9]+)*): ([a-z][a-z0-9-]*)", line)
            if not match:
                fail("proposal-parse", f"invalid capability at block line {line_no}", path)
            key, owner = match.groups()
            assert isinstance(result[section], dict)
            if key in result[section]:
                fail("proposal-parse", f"duplicate capability {key!r}", path)
            result[section][key] = owner
            continue
        scalar = re.fullmatch(r"([a-z_]+): (.+)", line)
        empty = re.fullmatch(r"([a-z_]+):", line)
        if empty:
            section = empty.group(1)
            if section in result:
                fail("proposal-parse", f"duplicate section {section!r}", path)
            result[section] = {} if section == "capabilities" else []
            continue
        if not scalar:
            fail("proposal-parse", f"invalid top-level line {line_no}", path)
        key, raw = scalar.groups()
        if key in result:
            fail("proposal-parse", f"duplicate key {key!r}", path)
        if key == "schema_version" and raw == "1":
            result[key] = 1
        elif key == "module" and MODULE_ID.fullmatch(raw):
            result[key] = raw
        else:
            fail("proposal-parse", f"invalid scalar {key!r} at block line {line_no}", path)
        section = ""
    if set(result) != POLICY_KEYS:
        fail("proposal-structure", f"keys mismatch: {sorted(result)}", path)
    return result


def inventory(root: Path) -> tuple[set[str], set[str]]:
    path = root_path(root, INVENTORY)
    value = load_json(path)
    if not isinstance(value, dict) or set(value) != {"schema_version", "required", "optional"}:
        fail("inventory-structure", "canonical inventory has an invalid envelope", path)
    if type(value["schema_version"]) is not int or value["schema_version"] != 1:
        fail("inventory-version", "canonical inventory version must be 1", path)
    required = string_list(value["required"], "required", path)
    optional = string_list(value["optional"], "optional", path)
    for identifier in required + optional:
        if not MODULE_ID.fullmatch(identifier):
            fail("module-id", f"invalid canonical module ID {identifier!r}", path)
    if set(required) & set(optional):
        fail("inventory-overlap", "required and optional modules overlap", path)
    return set(required), set(optional)


def descriptors(root: Path, canonical: set[str]) -> dict[str, set[str]]:
    resolved_root = root.resolve()
    base = root_path(root, DESCRIPTORS)
    result: dict[str, set[str]] = {}
    for discovered in sorted(base.glob("*/module.yaml")):
        path = discovered.resolve()
        try:
            path.relative_to(resolved_root)
        except ValueError:
            fail("path-escape", f"descriptor escapes repository: {discovered}")
        value = load_json(path)
        if not isinstance(value, dict):
            fail("descriptor-structure", "descriptor must be an object", path)
        identifier = value.get("id")
        dependencies = value.get("dependencies")
        if not isinstance(identifier, str) or not MODULE_ID.fullmatch(identifier):
            fail("descriptor-id", f"invalid module ID {identifier!r}", path)
        if identifier not in canonical:
            fail("unknown-module", f"descriptor references {identifier!r}", path)
        if identifier in result:
            fail("duplicate-module", f"duplicate descriptor for {identifier!r}", path)
        parsed = string_list(dependencies, "dependencies", path)
        unknown = sorted(set(parsed) - canonical)
        if unknown:
            fail("unknown-module", f"{identifier!r} depends on {unknown[0]!r}", path)
        result[identifier] = set(parsed)
    missing = sorted(canonical - set(result))
    if missing:
        fail("missing-descriptor", f"missing descriptor for {missing[0]!r}")
    return result


def validate(root: Path) -> None:
    expected = parse_proposal_contract(root_path(root, PROPOSAL))
    required, optional = inventory(root)
    if expected["module"] not in optional or expected["module"] in required:
        fail("classification", f"{expected['module']} must be canonical optional")
    canonical = required | optional
    graph = descriptors(root, canonical)
    path = root_path(root, POLICY)
    value = load_json(path)
    if not isinstance(value, dict):
        fail("structure", "policy must be an object", path)
    if set(value) != POLICY_KEYS:
        missing = sorted(POLICY_KEYS - set(value))
        unknown = sorted(set(value) - POLICY_KEYS)
        fail("structure", f"missing={missing}, unknown={unknown}", path)
    if value["schema_version"] != expected["schema_version"] or type(value["schema_version"]) is not int:
        fail("schema-version", f"expected {expected['schema_version']!r}", path)
    if value["module"] != expected["module"]:
        fail("module", f"expected owner module {expected['module']!r}", path)

    capabilities = value["capabilities"]
    expected_capabilities = expected["capabilities"]
    if not isinstance(capabilities, dict) or not isinstance(expected_capabilities, dict) or not all(
        isinstance(key, str) and isinstance(owner, str) for key, owner in capabilities.items()
    ):
        fail("capability-structure", "capabilities must map strings to module IDs", path)
    missing_caps = sorted(set(expected_capabilities) - set(capabilities))
    extra_caps = sorted(set(capabilities) - set(expected_capabilities))
    if missing_caps:
        fail("capability-missing", missing_caps[0], path)
    if extra_caps:
        fail("capability-extra", extra_caps[0], path)
    for capability in sorted(expected_capabilities):
        owner = capabilities[capability]
        if owner not in canonical:
            fail("unknown-module", f"{capability!r} owner {owner!r}", path)
        if owner != expected_capabilities[capability]:
            rule = "core-shadow" if owner in required else "capability-owner"
            fail(rule, f"{capability!r}: expected {expected_capabilities[capability]}, actual {owner}", path)

    list_fields = (
        "forbidden_core_shadows",
        "required_dependencies",
        "forbidden_dependencies_from_core",
    )
    for name in list_fields:
        actual = string_list(value[name], name, path)
        if actual != expected[name]:
            fail("semantic-equality", f"{name}: expected={expected[name]!r}, actual={actual!r}", path)

    owner = str(expected["module"])
    required_dependencies = string_list(expected["required_dependencies"], "required_dependencies", root_path(root, PROPOSAL))
    missing_edges = sorted(set(required_dependencies) - graph[owner])
    if missing_edges:
        fail("required-dependency", f"{owner} -> {missing_edges[0]}")
    forbidden_targets = set(string_list(expected["forbidden_dependencies_from_core"], "forbidden_dependencies_from_core", root_path(root, PROPOSAL)))
    for module in sorted(required):
        forbidden = sorted(graph[module] & forbidden_targets)
        if forbidden:
            fail("core-to-governance", f"{module} -> {forbidden[0]}")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--config-root", type=Path, default=REPO)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        validate(args.config_root)
    except OwnershipError as exc:
        print(f"check_capability_ownership: error: {exc}", file=sys.stderr)
        return 1
    expected = parse_proposal_contract(root_path(args.config_root, PROPOSAL))
    print(f"check_capability_ownership: ok ({len(expected['capabilities'])} governance capabilities)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
