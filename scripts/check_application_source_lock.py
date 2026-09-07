#!/usr/bin/env python3
"""Freeze/check bundled application sources without publishing module repositories."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import export_c_repositories as exporter  # noqa: E402

LOCK = ROOT / "dependencies/aimee-application-sources.lock.json"


def snapshot() -> dict[str, object]:
    inventory = exporter.load_json(exporter.INVENTORY)
    contracts = exporter.process_contracts.validate()
    repository_lock = exporter.load_json(exporter.LOCK)
    pins = repository_lock.get("modules", [])
    modules = []
    for classification in ("required", "optional"):
        for module_id in inventory[classification]:
            descriptor = exporter.load_json(ROOT / f"src/modules/{module_id}/module.yaml")
            contract = contracts[module_id]
            external = exporter.external_module_pin(module_id, classification, descriptor, contract)
            if external is not None:
                # Real external dependencies still use their existing exact pins.
                matches = [pin for pin in pins if pin.get("id") == module_id]
                if matches != [external]:
                    raise exporter.ExportError(f"{module_id}: external repository pin is stale")
                modules.append({"source": "external", **external})
                continue
            files = exporter.module_repository_files(module_id, descriptor)
            entry = {
                "id": module_id,
                "source": "bundled",
                "classification": classification,
                "execution": contract["execution"],
                "placements": contract["placements"],
                "source_sha256": exporter.digest_files([ROOT / path for path in files]),
            }
            if contract["execution"] == "process":
                entry.update(
                    runtime=contract["runtime"],
                    principal_class=exporter.PRINCIPAL_CLASS,
                    principal_ref=contract["principal_ref"],
                    serve=[stage["event_kind"] for stage in contract["stages"]],
                )
            modules.append(entry)
    return {
        "schema_version": 1,
        "source_repository": "https://github.com/RakuenSoftware/aimee.git",
        "core": {
            "source": "bundled",
            "version": exporter.CORE_VERSION_FILE.read_text(encoding="utf-8").strip(),
            "source_sha256": exporter.digest_files(exporter.core_files()),
        },
        "modules": modules,
    }


def check(path: Path = LOCK) -> None:
    if exporter.load_json(path) != snapshot():
        raise exporter.ExportError(
            "application source snapshot is stale; review the release diff, then run "
            "scripts/check_application_source_lock.py freeze"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", nargs="?", choices=("check", "freeze"), default="check")
    args = parser.parse_args()
    try:
        if args.command == "freeze":
            LOCK.write_text(json.dumps(snapshot(), indent=2) + "\n", encoding="utf-8")
        else:
            check()
    except (OSError, ValueError, exporter.ExportError) as exc:
        print(f"check_application_source_lock: error: {exc}", file=sys.stderr)
        return 1
    print(f"check_application_source_lock: {args.command} ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
