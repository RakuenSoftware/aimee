#!/usr/bin/env python3
"""Verify exact external-repository pins against the vendored source mirrors."""

from __future__ import annotations

import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import export_c_repositories as exporter  # noqa: E402


COMMIT = re.compile(r"^[0-9a-f]{40}$")


def fail(message: str) -> int:
    print(f"check_c_repository_lock: error: {message}", file=sys.stderr)
    return 1


def main() -> int:
    try:
        lock = exporter.load_json(exporter.LOCK)
        inventory = exporter.load_json(exporter.INVENTORY)
        core = lock.get("core")
        modules = lock.get("modules")
        if lock.get("schema_version") != 1 or not isinstance(core, dict) or not isinstance(modules, list):
            return fail("invalid lock structure")
        version = exporter.CORE_VERSION_FILE.read_text(encoding="utf-8").strip()
        if core.get("version") != version or core.get("ref") != f"v{version}":
            return fail("core semantic version pin does not match src/core/VERSION")
        if core.get("repository") != f"{exporter.REMOTE_ROOT}/aimee-core-c.git":
            return fail("unexpected core repository")
        if not isinstance(core.get("commit"), str) or not COMMIT.fullmatch(core["commit"]):
            return fail("core commit is not an exact SHA-1")
        if core.get("source_sha256") != exporter.digest_files(exporter.core_files()):
            return fail("core vendored mirror differs from its repository pin")

        expected_ids = inventory.get("required", []) + inventory.get("optional", [])
        # Declared, not positional: see validate_module_process_contracts. A
        # module's ref is its permanent identity, so classification and list
        # order must not move it.
        declared_refs = inventory.get("principal_refs")
        if not isinstance(declared_refs, dict) or set(declared_refs) != set(expected_ids):
            return fail("canonical inventory principal_refs must cover the inventory")
        if len(modules) != len(expected_ids):
            return fail(f"expected {len(expected_ids)} module pins, found {len(modules)}")
        by_id = {item.get("id"): item for item in modules if isinstance(item, dict)}
        if set(by_id) != set(expected_ids):
            return fail("module pin set differs from canonical inventory")
        required = set(inventory.get("required", []))
        contracts = exporter.process_contracts.validate()
        for module_id in expected_ids:
            principal_ref = declared_refs[module_id]
            item = by_id[module_id]
            classification = "required" if module_id in required else "optional"
            if item.get("classification") != classification:
                return fail(f"{module_id}: classification mismatch")
            contract = contracts[module_id]
            descriptor = exporter.load_json(ROOT / f"src/modules/{module_id}/module.yaml")
            external_pin = exporter.external_module_pin(
                module_id, classification, descriptor, contract
            )
            if external_pin is not None:
                if item != external_pin:
                    return fail(f"{module_id}: external repository pin is stale")
                continue
            if item.get("repository") != f"{exporter.REMOTE_ROOT}/aimee-module-{module_id}.git":
                return fail(f"{module_id}: unexpected repository")
            if item.get("version") != version or item.get("ref") != f"v{version}":
                return fail(f"{module_id}: version is not pinned to v{version}")
            if item.get("execution") != contract["execution"] or item.get("placements") != contract["placements"]:
                return fail(f"{module_id}: execution/placement mismatch")
            if contract["execution"] == "process":
                expected_serve = [stage["event_kind"] for stage in contract["stages"]]
                if (item.get("runtime") != contract["runtime"] or
                        item.get("principal_class") != exporter.PRINCIPAL_CLASS or
                        item.get("principal_ref") != principal_ref or
                        item.get("serve") != expected_serve):
                    return fail(f"{module_id}: process runtime/identity/grant mismatch")
            elif any(key in item for key in ("principal_class", "principal_ref", "serve")):
                return fail(f"{module_id}: core component has process identity")
            if not isinstance(item.get("commit"), str) or not COMMIT.fullmatch(item["commit"]):
                return fail(f"{module_id}: commit is not an exact SHA-1")
            repository_files = exporter.module_repository_files(module_id, descriptor)
            expected_digest = exporter.digest_files([ROOT / path for path in repository_files])
            if item.get("source_sha256") != expected_digest:
                return fail(f"{module_id}: vendored mirror differs from its repository pin")
    except (OSError, UnicodeError, json.JSONDecodeError, exporter.ExportError) as exc:
        return fail(str(exc))
    print(f"check_c_repository_lock: ok (1 core, {len(modules)} modules)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
