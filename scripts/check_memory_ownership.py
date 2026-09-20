#!/usr/bin/env python3
"""Check the reviewed external-host dispositions without weakening the native inventory.

The historical all-native-callers report deliberately remains failing while C
hosts exist. This separate gate requires every finding and original native API
to have an explicit owner, contract and conformance reference.
"""
from __future__ import annotations
import importlib.util
import json
import hashlib
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
LEDGER = Path("tests/baselines/modules/memory-ownership.json")
spec = importlib.util.spec_from_file_location("native_inventory", ROOT / "scripts/check_memory_go_only.py")
assert spec and spec.loader
inventory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory)


def findings(root: Path, manifest: dict) -> dict[str, list[str]]:
    patterns = [re.escape(s) if s not in inventory.CAPTURED_MEMBERS else inventory.CAPTURED_MEMBERS[s]
                for s in manifest["native_symbols"]]
    symbols = re.compile(r"\b(?:" + "|".join(patterns) +
                         r"|AIMEE_MEMORY_\w+|aimee_memory_\w+|server_module_memory_\w+|kb_module_memory_\w+|kb_client_memory_\w+)\b")
    files = {}
    for finding in inventory.violations(root, manifest):
        if finding["rule"] != "memory-native-api":
            raise ValueError(f"unclassified native violation: {finding}")
        path = finding["file"]
        files[path] = sorted(set(symbols.findall(inventory.without_comments((root / path).read_text()))))
    return files


def validate(root: Path) -> None:
    manifest = json.loads((root / inventory.MANIFEST).read_text())
    ledger = json.loads((root / LEDGER).read_text())
    if ledger["source_commit"] != manifest["source_commit"]:
        raise ValueError("original source reference changed")
    if set(ledger["original_files"]) != set(manifest["native_files"]):
        raise ValueError("missing original file disposition")
    if set(ledger["original_symbols"]) != set(manifest["native_symbols"]):
        raise ValueError("missing original symbol disposition")
    actual = findings(root, manifest)
    # Build products are absent in clean lint checkouts. They remain reviewed
    # findings when present; pin both output and generator/input content.
    generated = ledger.get("generated_files", {})
    for path, row in generated.items():
        if path in ledger["external_files"] or not row.get("inputs"):
            raise ValueError(f"invalid generated disposition: {path}")
        for source, expected in row["inputs"].items():
            if hashlib.sha256((root / source).read_bytes()).hexdigest() != expected:
                raise ValueError(f"generated input needs ownership review: {source}")
    reviewed = {**ledger["external_files"], **generated}
    present = {path: row for path, row in reviewed.items()
               if path not in generated or (root / path).exists()}
    recorded = {path: row["symbols"] for path, row in present.items()}
    if actual != recorded:
        changed = sorted(p for p in set(actual) | set(recorded) if actual.get(p) != recorded.get(p))
        raise ValueError(f"external memory ownership needs review: {changed}")
    for path, row in present.items():
        digest = hashlib.sha256((root / path).read_bytes()).hexdigest()
        if row.get("sha256") != digest:
            raise ValueError(f"external memory adapter changed without ownership review: {path}")
    for path, row in ledger["original_files"].items():
        for replacement in row["replacement_files"]:
            if not (root / replacement).is_file():
                raise ValueError(f"missing replacement owner: {path}: {replacement}")
        if (root / path).exists():
            raise ValueError(f"retired native file restored: {path}")
    for section in ("original_files", "original_symbols", "external_files", "generated_files"):
        for name, row in ledger.get(section, {}).items():
            group = ledger["dispositions"].get(row["disposition"])
            if not group or not group.get("owner") or not group.get("contract") or not group.get("reason") or not group.get("cutover_dependency"):
                raise ValueError(f"incomplete disposition: {name}")
            if not group.get("evidence"):
                raise ValueError(f"missing conformance evidence: {name}")
            for path in group["evidence"]:
                if not (root / path).is_file():
                    raise ValueError(f"missing conformance evidence: {name}: {path}")


if __name__ == "__main__":
    try:
        validate(ROOT)
    except (ValueError, KeyError, OSError) as exc:
        raise SystemExit(f"memory-ownership: {exc}")
    print("memory-ownership: all original files/APIs and external findings classified; C bus retained")
