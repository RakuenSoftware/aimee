#!/usr/bin/env python3
"""Verify that every module process contract produces a Go binary, never C."""

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
        # The ring's telemetry exporter is the same module runtime under its own
        # name, spawned in every placement, though it serves no stage.
        expected_spawned.append(json.loads(exporter.process_contracts.CONTRACTS.read_text(
            encoding="utf-8"))["telemetry"]["id"])
        # Every module process is Go; the C core is only their caller. A
        # contract naming any other runtime is a regression to the retired C
        # module processes, which the exporter no longer knows how to build.
        other = sorted(name for name, row in contracts.items()
                       if row["execution"] == "process" and row.get("runtime") != "go")
        if other:
            return fail(f"module processes not in Go: {other!r}")
        with tempfile.TemporaryDirectory(prefix="aimee-go-module-bundle-") as temporary:
            temporary_root = Path(temporary)
            bundle = temporary_root / "bundle"
            exporter.export_runtime_bundle(bundle)
            actual_go = (bundle / "go.modules").read_text(encoding="utf-8").splitlines()
            if actual_go != expected_spawned:
                return fail(f"Go module list {actual_go!r} differs from {expected_spawned!r}")
            if (bundle / "src").exists() or (bundle / "c-build.json").exists():
                return fail("the runtime bundle carries C module process sources")
            manifest = json.loads((bundle / "MANIFEST.json").read_text(encoding="utf-8"))
            if manifest.get("runtimes") != {name: row["runtime"] for name, row in contracts.items()
                                             if row["execution"] == "process"}:
                return fail("runtime manifest differs from the canonical contracts")

            repositories = temporary_root / "repositories"
            repositories.mkdir()
            version = exporter.CORE_VERSION_FILE.read_text(encoding="utf-8").strip()
            timestamp = exporter.source_timestamp()
            for module_id in expected_go:
                descriptor = exporter.load_json(ROOT / f"src/modules/{module_id}/module.yaml")
                if "external_source" in descriptor:
                    continue
                exporter.export_module(repositories, module_id, "required",
                                       contracts[module_id], timestamp, version)
                repository = repositories / f"aimee-module-{module_id}"
                if not (repository / "runtime/main.go").is_file() or \
                        (repository / "runtime/main.c").exists():
                    return fail(f"{module_id}: isolated repository is not Go-only at runtime")
                # The ring is a dependency, never a copy: an export that carried
                # its own bus drifted from every later ring fix (aimee-module-config
                # did until 2026-10-27).
                if (repository / "server-go/bus").exists():
                    return fail(f"{module_id}: isolated repository carries a private ring copy")
                ring_pin = exporter.go_dependency_version("github.com/RakuenSoftware/ouroboros-ring/go")
                go_mod = (repository / "go.mod").read_text(encoding="utf-8")
                if f"github.com/RakuenSoftware/ouroboros-ring/go {ring_pin}" not in go_mod:
                    return fail(f"{module_id}: isolated go.mod does not require the ring release aimee requires")
                shared_sources = exporter.go_process_shared_sources(module_id)
                for relative in [*shared_sources, *descriptor.get("go_sources", []),
                                 *descriptor.get("go_assets", [])]:
                    if (repository / relative).read_bytes() != (ROOT / relative).read_bytes():
                        return fail(f"{module_id}: isolated repository changed {relative}")
                repository_manifest = exporter.load_json(repository / "SOURCE_MANIFEST.json")
                if repository_manifest.get("runtime") != "go":
                    return fail(f"{module_id}: isolated manifest is not marked Go")
                cmake = (repository / "CMakeLists.txt").read_text(encoding="utf-8")
                for relative in descriptor.get("go_assets", []):
                    if "${CMAKE_CURRENT_SOURCE_DIR}/" + relative not in cmake:
                        return fail(f"{module_id}: embedded asset is not a build dependency: {relative}")
                if "CGO_ENABLED=0" not in cmake or "./runtime" not in cmake:
                    return fail(f"{module_id}: isolated Go build boundary is incomplete")

        for dockerfile in (ROOT / "Dockerfile", ROOT / "Dockerfile.server"):
            text = dockerfile.read_text(encoding="utf-8")
            for required in ("AS module-go-build", "./cmd/aimee-module",
                             "/module-runtime/go.modules", "/tmp/aimee-module-go"):
                if required not in text:
                    return fail(f"{dockerfile.name}: missing {required!r}")
            for forbidden in ("for source in /module-runtime/src/*.c",
                              "src/core/event_bus/bus_attach.c src/core/event_bus/bus_client.c",
                              "build_c_module_runtime_bundle.py"):
                if forbidden in text:
                    return fail(f"{dockerfile.name}: retains legacy C build fragment {forbidden!r}")
    except (OSError, UnicodeError, json.JSONDecodeError, exporter.ExportError) as exc:
        return fail(str(exc))
    print(f"check_go_module_runtime_bundle: ok ({len(expected_go)} Go module processes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
