#!/usr/bin/env python3
"""Create and verify the pinned EuroBERT Python environment on shared storage."""

from __future__ import annotations

import argparse
import hashlib
import json
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


def cuda_identity(python: Path) -> dict[str, Any]:
    code = (
        "import json,platform,torch; "
        "print(json.dumps({'python': platform.python_version(), 'torch': torch.__version__, "
        "'cuda': torch.version.cuda, 'cuda_available': torch.cuda.is_available(), "
        "'bf16_supported': torch.cuda.is_bf16_supported(), "
        "'device': torch.cuda.get_device_name(0) if torch.cuda.is_available() else None, "
        "'compute_capability': list(torch.cuda.get_device_capability(0)) "
        "if torch.cuda.is_available() else None}))"
    )
    result = subprocess.run(
        [str(python), "-c", code],
        text=True,
        capture_output=True,
        check=True,
    )
    identity = json.loads(result.stdout)
    driver = subprocess.run(
        ["nvidia-smi", "--query-gpu=driver_version", "--format=csv,noheader"],
        text=True,
        capture_output=True,
        check=True,
    ).stdout.strip()
    identity["nvidia_driver"] = driver
    return identity


def verify_cuda_identity(identity: dict[str, Any], runtime: dict[str, Any]) -> None:
    expected = {
        "torch": runtime["torch"]["version"],
        "cuda": runtime["torch"]["cuda"],
        "device": runtime["platform"]["gpu"],
        "nvidia_driver": runtime["platform"]["nvidia_driver"],
    }
    actual = {key: identity.get(key) for key in expected}
    if (
        actual != expected
        or identity.get("cuda_available") is not True
        or identity.get("bf16_supported") is not True
    ):
        raise RuntimeError(f"CUDA runtime identity mismatch: expected {expected}, received {identity}")


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
    runtime = manifest["runtime"]
    packages = runtime["packages"]
    expected_versions = {"torch": runtime["torch"]["version"], **packages}
    expected = expected_provenance(args.manifest, manifest)
    python = args.venv_dir / "bin/python3"
    provenance_path = args.venv_dir.parent / "runtime_provenance.json"

    if provenance_path.exists() and python.exists():
        actual = json.loads(provenance_path.read_text(encoding="utf-8"))
        comparable = {key: actual.get(key) for key in expected}
        if comparable == expected and actual.get("status") == "complete":
            observed = installed_versions(python, expected_versions)
            identity = cuda_identity(python)
            if observed == expected_versions:
                verify_cuda_identity(identity, runtime)
                print(json.dumps(actual, indent=2, sort_keys=True))
                return 0

    args.venv_dir.parent.mkdir(parents=True, exist_ok=True)
    venv.EnvBuilder(with_pip=True, system_site_packages=False, clear=True).create(args.venv_dir)
    torch = runtime["torch"]
    subprocess.run(
        [
            str(python),
            "-m",
            "pip",
            "install",
            "--no-cache-dir",
            "--disable-pip-version-check",
            "--index-url",
            torch["index_url"],
            f"torch=={torch['version']}",
        ],
        check=True,
    )
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
    observed = installed_versions(python, expected_versions)
    if observed != expected_versions:
        raise RuntimeError(
            f"runtime package identity mismatch: expected {expected_versions}, received {observed}"
        )
    identity = cuda_identity(python)
    verify_cuda_identity(identity, runtime)

    completed = {
        **expected,
        "status": "complete",
        "environment": {
            "packages": observed,
            **identity,
        },
        "venv_python": str(python),
    }
    write_json_atomic(provenance_path, completed)
    print(json.dumps(completed, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
