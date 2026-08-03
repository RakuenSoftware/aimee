#!/usr/bin/env python3
"""Validate the core/process carving, placement, identities, and stage events."""

from __future__ import annotations

import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
CONTRACTS = ROOT / "src/modules/process-contracts.json"
INVENTORY = ROOT / "tests/baselines/modules/canonical-inventory.yaml"
CORE = {
    "module-runtime", "config", "ir", "translation", "protocols", "gateway",
    "vault", "execution-policy", "audit",
}
PROCESS_REQUIRED = {
    "memory", "learning", "routing", "delegates", "tools", "workspace", "git",
    "skills", "response-composition",
}
GO_PROCESSES = {
    "memory", "learning", "routing", "delegates", "tools", "workspace", "git",
    "skills", "response-composition", "roundtable", "benchmarks",
}
STAGE_RE = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")


class ContractError(ValueError):
    pass


def load(path: Path) -> dict[str, object]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ContractError(f"{path}: expected object")
    return value


def validate() -> dict[str, dict[str, object]]:
    inventory = load(INVENTORY)
    ordered = inventory.get("required", []) + inventory.get("optional", [])
    if not all(isinstance(item, str) for item in ordered):
        raise ContractError("canonical inventory is not a string array")
    contract = load(CONTRACTS)
    if set(contract) != {"schema_version", "principal_class", "components"}:
        raise ContractError("top-level keys differ from v1")
    if contract["schema_version"] != 2 or contract["principal_class"] != 1:
        raise ContractError("schema_version must equal 2 and principal_class must equal 1")
    components = contract["components"]
    if not isinstance(components, list) or len(components) != len(ordered):
        raise ContractError("component count differs from canonical inventory")
    if [item.get("id") for item in components if isinstance(item, dict)] != ordered:
        raise ContractError("components must exactly follow canonical inventory order")

    result: dict[str, dict[str, object]] = {}
    refs: set[int] = set()
    kinds: set[int] = set()
    stage_names: set[str] = set()
    optional = set(inventory.get("optional", []))
    for ordinal, raw in enumerate(components, start=1):
        if not isinstance(raw, dict):
            raise ContractError(f"component {ordinal}: expected object")
        component_id = raw.get("id")
        execution = raw.get("execution")
        placements = raw.get("placements")
        if not isinstance(component_id, str) or execution not in {"core", "process"}:
            raise ContractError(f"component {ordinal}: invalid id/execution")
        placement_order = {"server": 0, "kb": 1}
        if (not isinstance(placements, list) or not placements or
                placements != sorted(set(placements), key=placement_order.get)):
            raise ContractError(f"{component_id}: placements must be sorted and unique")
        if not set(placements) <= {"kb", "server"}:
            raise ContractError(f"{component_id}: unknown placement")
        should_be_core = component_id in CORE
        if (execution == "core") != should_be_core:
            raise ContractError(f"{component_id}: execution violates the C core carving")
        if component_id in PROCESS_REQUIRED and execution != "process":
            raise ContractError(f"{component_id}: required feature must be a process")

        if execution == "core":
            if set(raw) != {"id", "execution", "placements"}:
                raise ContractError(f"{component_id}: core component has process fields")
        else:
            if set(raw) != {"id", "execution", "runtime", "principal_ref", "placements", "stages"}:
                raise ContractError(f"{component_id}: process keys differ from v2")
            runtime = raw["runtime"]
            if runtime not in {"c", "go"}:
                raise ContractError(f"{component_id}: process runtime must be c or go")
            if (component_id in GO_PROCESSES) != (runtime == "go"):
                raise ContractError(f"{component_id}: runtime differs from the migration set")
            principal_ref = raw["principal_ref"]
            if type(principal_ref) is not int or principal_ref != ordinal or principal_ref in refs:
                raise ContractError(f"{component_id}: principal_ref must equal inventory ordinal")
            refs.add(principal_ref)
            stages = raw["stages"]
            if not isinstance(stages, list) or not stages:
                raise ContractError(f"{component_id}: process must serve at least one stage")
            for stage_ordinal, stage in enumerate(stages, start=1):
                if not isinstance(stage, dict) or set(stage) != {"id", "name", "event_kind"}:
                    raise ContractError(f"{component_id}: invalid stage shape")
                name, stage_id, kind = stage["name"], stage["id"], stage["event_kind"]
                expected_kind = 4096 + ordinal * 256 + stage_ordinal
                if type(stage_id) is not int or stage_id != stage_ordinal:
                    raise ContractError(f"{component_id}: stage IDs must be dense from one")
                if not isinstance(name, str) or not STAGE_RE.fullmatch(name) or name in stage_names:
                    raise ContractError(f"{component_id}: invalid or duplicate stage name {name!r}")
                if type(kind) is not int or kind != expected_kind or kind in kinds:
                    raise ContractError(f"{component_id}/{name}: event_kind must equal {expected_kind}")
                stage_names.add(name)
                kinds.add(kind)
            if component_id not in optional and component_id not in PROCESS_REQUIRED:
                raise ContractError(f"{component_id}: unexpected required process")
        result[component_id] = raw
    return result


def main() -> int:
    try:
        components = validate()
    except (OSError, UnicodeError, json.JSONDecodeError, ContractError) as exc:
        print(f"validate_module_process_contracts: error: {exc}", file=sys.stderr)
        return 1
    process_count = sum(item["execution"] == "process" for item in components.values())
    go_count = sum(item.get("runtime") == "go" for item in components.values())
    print(f"validate_module_process_contracts: ok ({len(components)} components, "
          f"{process_count} processes: {go_count} Go, {process_count - go_count} C)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
