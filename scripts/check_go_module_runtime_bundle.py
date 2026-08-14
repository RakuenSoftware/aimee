#!/usr/bin/env python3
"""Verify that Go process contracts produce Go binaries, never C fallbacks."""

from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import export_c_repositories as exporter  # noqa: E402


def fail(message: str) -> int:
    print(f"check_go_module_runtime_bundle: error: {message}", file=sys.stderr)
    return 1


def main() -> int:
    try:
        contracts = exporter.process_contracts.validate()
        expected_go = [name for name, row in contracts.items()
                       if row.get("runtime") == "go"]
        # go.modules is the module runtime's SPAWN list, not the set of Go
        # processes. A process hosted by an already-supervised program is not
        # spawned there -- the bus denies a live duplicate of its principal --
        # so it is a Go module with source and a repository, but not a row here.
        expected_spawned = [name for name, row in contracts.items()
                            if row.get("runtime") == "go" and not row.get("hosted_by")]
        expected_c = [name for name, row in contracts.items()
                      if row.get("runtime") == "c"]
        with tempfile.TemporaryDirectory(prefix="aimee-go-module-bundle-") as temporary:
            temporary_root = Path(temporary)
            bundle = temporary_root / "bundle"
            exporter.export_runtime_bundle(bundle)
            actual_go = (bundle / "go.modules").read_text(encoding="utf-8").splitlines()
            if actual_go != expected_spawned:
                return fail(f"Go module list {actual_go!r} differs from {expected_spawned!r}")
            actual_c = sorted(path.stem.removeprefix("aimee-module-")
                              for path in (bundle / "src").glob("aimee-module-*.c"))
            if actual_c != sorted(expected_c):
                return fail(f"C runtime sources {actual_c!r} differ from {sorted(expected_c)!r}")
            if set(actual_go) & set(actual_c):
                return fail("a migrated module also has a generated C runtime")
            manifest = json.loads((bundle / "MANIFEST.json").read_text(encoding="utf-8"))
            if manifest.get("runtimes") != {name: row["runtime"] for name, row in contracts.items()
                                             if row["execution"] == "process"}:
                return fail("runtime manifest differs from the canonical contracts")

            repositories = temporary_root / "repositories"
            repositories.mkdir()
            version = exporter.CORE_VERSION_FILE.read_text(encoding="utf-8").strip()
            timestamp = exporter.source_timestamp()
            for module_id in expected_go:
                exporter.export_module(repositories, module_id, "required",
                                       contracts[module_id], timestamp, version)
                repository = repositories / f"aimee-module-{module_id}"
                if not (repository / "runtime/main.go").is_file() or \
                        (repository / "runtime/main.c").exists():
                    return fail(f"{module_id}: isolated repository is not Go-only at runtime")
                descriptor = exporter.load_json(ROOT / f"src/modules/{module_id}/module.yaml")
                canonical_bus = exporter.go_bus_sources(module_id)
                shared_sources = exporter.go_process_shared_sources(module_id)
                for relative in [*canonical_bus, *shared_sources, *descriptor.get("go_sources", [])]:
                    if (repository / relative).read_bytes() != (ROOT / relative).read_bytes():
                        return fail(f"{module_id}: isolated repository changed {relative}")
                repository_manifest = exporter.load_json(repository / "SOURCE_MANIFEST.json")
                if repository_manifest.get("runtime") != "go":
                    return fail(f"{module_id}: isolated manifest is not marked Go")
                cmake = (repository / "CMakeLists.txt").read_text(encoding="utf-8")
                if "CGO_ENABLED=0" not in cmake or "./runtime" not in cmake:
                    return fail(f"{module_id}: isolated Go build boundary is incomplete")

        for dockerfile in (ROOT / "Dockerfile", ROOT / "Dockerfile.server"):
            text = dockerfile.read_text(encoding="utf-8")
            for required in ("AS module-go-build", "./cmd/aimee-module",
                             "/module-runtime/go.modules", "/tmp/aimee-module-go"):
                if required not in text:
                    return fail(f"{dockerfile.name}: missing {required!r}")
    except (OSError, UnicodeError, json.JSONDecodeError, exporter.ExportError) as exc:
        return fail(str(exc))
    print(f"check_go_module_runtime_bundle: ok ({len(expected_go)} Go, {len(expected_c)} C)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
