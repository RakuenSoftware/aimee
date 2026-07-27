#!/usr/bin/env python3
"""Create and verify the pinned EuroBERT Python environment on shared storage."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import subprocess
import venv
from pathlib import Path
from typing import Any


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expected_provenance(manifest_path: Path, manifest: dict[str, Any]) -> dict[str, Any]:
    return {
        "bootstrap_sha256": sha256(Path(__file__).resolve()),
        "manifest_sha256": sha256(manifest_path),
        "runtime": manifest["runtime"],
    }


def installed_versions(python: Path, packages: dict[str, str]) -> dict[str, str]:
    code = (
        "import importlib.metadata,json,sys; "
        "print(json.dumps({name: importlib.metadata.version(name) for name in sys.argv[1:]}))"
    )
    result = subprocess.run(
        [str(python), "-c", code, *packages],
        text=True,
        capture_output=True,
        check=True,
    )
    return json.loads(result.stdout)


def write_json_atomic(path: Path, payload: dict[str, Any]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--venv-dir", type=Path, required=True)
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    packages = manifest["runtime"]["packages"]
    expected = expected_provenance(args.manifest, manifest)
    python = args.venv_dir / "bin/python3"
    provenance_path = args.venv_dir.parent / "runtime_provenance.json"

    if provenance_path.exists() and python.exists():
        actual = json.loads(provenance_path.read_text(encoding="utf-8"))
        comparable = {key: actual.get(key) for key in expected}
        if comparable == expected and actual.get("status") == "complete":
            observed = installed_versions(python, packages)
            if observed == packages:
                print(json.dumps(actual, indent=2, sort_keys=True))
                return 0

    args.venv_dir.parent.mkdir(parents=True, exist_ok=True)
    venv.EnvBuilder(with_pip=True, system_site_packages=True, clear=True).create(args.venv_dir)
    requirements = [f"{name}=={version}" for name, version in packages.items()]
    subprocess.run(
        [
            str(python),
            "-m",
            "pip",
            "install",
            "--no-cache-dir",
            "--disable-pip-version-check",
            *requirements,
        ],
        check=True,
    )
    observed = installed_versions(python, packages)
    if observed != packages:
        raise RuntimeError(f"runtime package identity mismatch: expected {packages}, received {observed}")

    completed = {
        **expected,
        "status": "complete",
        "environment": {
            "python": platform.python_version(),
            "packages": observed,
        },
        "venv_python": str(python),
    }
    write_json_atomic(provenance_path, completed)
    print(json.dumps(completed, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
