#!/usr/bin/env python3
"""Build the frozen real Go memory owner as an explicitly selected test fixture."""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "tests/fixtures/legacy-memory-owner.json"
SOURCE_COMMIT = "0e29dd14f8c96de8f8366d763dfe56992aedc2b0"
SOURCE_TREE = "2ed04ea2d796ba20c61b2d104582d6224942cb0d"


def output_paths(output: Path) -> tuple[Path, Path]:
    if not output.is_absolute() or output.name != "aimee-module-memory":
        raise ValueError("--output must be absolute with basename aimee-module-memory")
    output = output.parent.resolve(strict=True) / output.name
    if output.is_relative_to(ROOT):
        raise ValueError("the reference fixture must stay outside the working checkout")
    sidecar = output.with_name(output.name + ".json")
    if os.path.lexists(output) or os.path.lexists(sidecar):
        raise ValueError("reference output and provenance sidecar must both be fresh")
    return output, sidecar


def extract_source(blob: bytes, directory: Path) -> None:
    # This public Git tree contains ordinary files/directories. Refuse links or
    # traversal rather than letting an archive write outside the temporary tree.
    with tarfile.open(fileobj=io.BytesIO(blob), mode="r:") as archive:
        for member in archive:
            parts = Path(member.name).parts
            if not parts or parts[0] != "server-go" or ".." in parts:
                raise ValueError("unexpected path in frozen source archive")
            target = directory.joinpath(*parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                source = archive.extractfile(member)
                assert source is not None
                with source, target.open("xb") as destination:
                    shutil.copyfileobj(source, destination)
            else:
                raise ValueError("frozen source archive contains a non-regular entry")


def fixture_manifest() -> dict:
    manifest = json.loads(MANIFEST.read_text())
    if manifest != {
        "schema_version": 1, "source_commit": SOURCE_COMMIT,
        "source_tree": SOURCE_TREE, "source_path": "server-go",
        "build_package": "./cmd/aimee-module", "binary_basename": "aimee-module-memory",
        "go_version": "go1.26.10", "purpose": "external-reference-not-shipped",
    }:
        raise ValueError("reference fixture manifest differs from its reviewed frozen pin")
    return manifest


def verify_reference_fixture(path: Path) -> dict:
    if not path.is_absolute() or path.name != "aimee-module-memory":
        raise ValueError("reference owner must be an absolute aimee-module-memory path")
    sidecar = path.with_name(path.name + ".json")
    if (path.is_symlink() or not path.is_file() or not os.access(path, os.X_OK)
            or sidecar.is_symlink() or not sidecar.is_file()):
        raise ValueError("reference owner requires a regular executable and provenance sidecar")
    manifest = fixture_manifest()
    receipt = json.loads(sidecar.read_text())
    if (not isinstance(receipt, dict) or
            any(receipt.get(key) != value for key, value in manifest.items()) or
            receipt.get("binary_sha256") != hashlib.sha256(path.read_bytes()).hexdigest()):
        raise ValueError("reference owner does not match its frozen provenance")
    return receipt


def build(output: Path) -> dict:
    output, sidecar = output_paths(output)
    manifest = fixture_manifest()
    actual_tree = subprocess.check_output(
        ["git", "rev-parse", f"{SOURCE_COMMIT}:server-go"], cwd=ROOT, text=True).strip()
    if actual_tree != SOURCE_TREE:
        raise ValueError("frozen server-go source tree differs from its pin")
    env = dict(os.environ, CGO_ENABLED="0", GOTOOLCHAIN=manifest["go_version"], GOWORK="off",
               GOFLAGS="-mod=readonly")
    version = subprocess.check_output(["go", "version"], cwd=ROOT, env=env, text=True).split()
    if len(version) < 3 or version[2] != manifest["go_version"]:
        raise ValueError("reference fixture requires the pinned Go 1.26.10 toolchain")

    # 1. Export only the exact public frozen module, never the working owner.
    archive = subprocess.check_output(
        ["git", "archive", "--format=tar", SOURCE_COMMIT, "server-go"], cwd=ROOT)
    # 2. Build the actual complete module in an external, disposable checkout.
    with tempfile.TemporaryDirectory(prefix="aimee-memory-reference-", dir=output.parent) as temporary:
        directory = Path(temporary)
        extract_source(archive, directory)
        binary = directory / "aimee-module-memory"
        subprocess.run(["go", "build", "-o", str(binary), "./cmd/aimee-module"],
                       cwd=directory / "server-go", env=env, check=True)
        if not stat.S_ISREG(binary.lstat().st_mode):
            raise ValueError("Go did not produce a regular reference executable")
        receipt = {**manifest, "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
        # 3. Publish fresh files without replacing any existing executable.
        os.link(binary, output)
        try:
            with sidecar.open("x") as stream:
                json.dump(receipt, stream, indent=2)
                stream.write("\n")
        except BaseException:
            output.unlink()
            raise
    return receipt


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--output", type=Path)
    action.add_argument("--verify", type=Path,
                        help="verify an existing explicit reference without building or installing")
    args = parser.parse_args()
    try:
        receipt = verify_reference_fixture(args.verify) if args.verify else build(args.output)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"reference-memory-fixture: {error}\n")
    print(json.dumps(receipt, sort_keys=True))


if __name__ == "__main__":
    main()
