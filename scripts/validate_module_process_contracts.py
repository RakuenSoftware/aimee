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
    "skills", "response-composition", "governance", "workflows", "roundtable", "kb-synthesis",
    "runtime-web", "control-web", "benchmarks", "sandbox", "economizer",
}
# Executables that host a process other than the module runtime's multicall binary.
HOSTED_BY = {"wfe": "/usr/local/bin/aimee-wfe"}

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
    if set(contract) != {"schema_version", "principal_class", "components", "clients"}:
        raise ContractError("top-level keys differ from v3")
    if contract["schema_version"] != 3 or contract["principal_class"] != 1:
        raise ContractError("schema_version must equal 3 and principal_class must equal 1")
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
            keys = set(raw) - {"hosted_by"}
            if keys != {"id", "execution", "runtime", "principal_ref", "placements", "stages"}:
                raise ContractError(f"{component_id}: process keys differ from v2")
            # Most processes are multicall binaries the module runtime spawns and
            # pins by path. A process that is an already-supervised program instead
            # says so, because its grant must pin THAT executable and the runtime
            # must not spawn a second holder of the same principal -- a live
            # duplicate principal is denied admission.
            hosted_by = raw.get("hosted_by")
            if hosted_by is not None and hosted_by not in HOSTED_BY:
                raise ContractError(
                    f"{component_id}: hosted_by must be one of {sorted(HOSTED_BY)}")
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
    validate_clients(contract["clients"], kinds, refs)
    return result


def validate_clients(clients: object, served_kinds: set[int], module_refs: set[int]) -> None:
    """Bus principals that only request stages.

    A client is not a module: it serves nothing and has no inventory ordinal, so
    its principal_ref cannot be derived from one. Refs are therefore explicit and
    live above the module range, and a client may only request a kind some module
    actually serves -- a typo there is a grant that admits nothing, which shows
    up as an unexplained runtime refusal rather than a build failure.
    """
    if not isinstance(clients, list):
        raise ContractError("clients must be an array")
    seen_ids: set[str] = set()
    seen_refs: set[int] = set()
    for ordinal, client in enumerate(clients, start=1):
        if not isinstance(client, dict) or set(client) != {
                "id", "principal_ref", "executable", "placements", "request"}:
            raise ContractError(f"client {ordinal}: keys differ from v3")
        identifier = client["id"]
        if not isinstance(identifier, str) or not identifier or identifier in seen_ids:
            raise ContractError(f"client {ordinal}: invalid or duplicate id")
        seen_ids.add(identifier)
        ref = client["principal_ref"]
        if type(ref) is not int or ref <= max(module_refs, default=0):
            raise ContractError(f"{identifier}: principal_ref must sit above every module ref")
        if ref in seen_refs or ref in module_refs:
            raise ContractError(f"{identifier}: principal_ref {ref} collides")
        seen_refs.add(ref)
        executable = client["executable"]
        if not isinstance(executable, str) or not executable.startswith("/"):
            raise ContractError(f"{identifier}: executable must be an absolute path")
        placements = client["placements"]
        placement_order = {"server": 0, "kb": 1}
        if (not isinstance(placements, list) or not placements or
                placements != sorted(set(placements), key=placement_order.get)):
            raise ContractError(f"{identifier}: placements must be sorted and unique")
        if not set(placements) <= {"kb", "server"}:
            raise ContractError(f"{identifier}: unknown placement")
        request = client["request"]
        if not isinstance(request, list) or not request:
            raise ContractError(f"{identifier}: a client must request at least one kind")
        for kind in request:
            if type(kind) is not int or kind not in served_kinds:
                raise ContractError(f"{identifier}: requests kind {kind}, which no module serves")


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
