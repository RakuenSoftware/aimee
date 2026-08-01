#!/usr/bin/env python3
"""Durable, non-splicing runner for code-intelligence experiment cells."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import stat
import subprocess
import time
import uuid
from pathlib import Path


def canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def fsync_dir(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def durable_mkdir(path: Path) -> None:
    path.mkdir()
    fsync_dir(path)
    fsync_dir(path.parent)


def write_new(path: Path, value: object) -> None:
    with path.open("xb") as stream:
        stream.write(canonical(value))
        stream.flush()
        os.fsync(stream.fileno())
    fsync_dir(path.parent)


def replace(path: Path, value: object) -> None:
    if path.parent.is_symlink() or path.parent.resolve() != path.parent.absolute():
        raise ValueError("checkpoint directory was replaced or symlinked")
    temporary = path.with_name(f"{path.name}.{uuid.uuid4().hex}.new")
    with temporary.open("xb") as stream:
        stream.write(canonical(value))
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    fsync_dir(path.parent)


def load(path: Path) -> object:
    return json.loads(path.read_text())


def file_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def safe_name(value: object, label: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}", value):
        raise ValueError(f"{label} must be a bounded path-safe string")
    return value


def require_plain_path(root: Path, path: Path) -> None:
    relative = path.absolute().relative_to(root.absolute())
    current = root.absolute()
    for index, component in enumerate(relative.parts):
        current /= component
        mode = current.lstat().st_mode
        if stat.S_ISLNK(mode):
            raise ValueError("artifact evidence path contains a symlink")
        if index < len(relative.parts) - 1 and not stat.S_ISDIR(mode):
            raise ValueError("artifact evidence parent is not a directory")
        if index == len(relative.parts) - 1 and not stat.S_ISREG(mode):
            raise ValueError("artifact evidence is not a regular file")


def start(plan_path: Path, run_dir: Path, checkpoint_name: str) -> tuple[dict, dict, Path]:
    safe_name(checkpoint_name, "checkpoint name")
    plan = load(plan_path)
    cells = plan.get("cells") if isinstance(plan, dict) else None
    if not isinstance(cells, list) or not cells:
        raise ValueError("plan must contain a nonempty cells array")
    cell_ids = [cell.get("id") for cell in cells if isinstance(cell, dict)]
    if len(cell_ids) != len(cells) or len(set(cell_ids)) != len(cell_ids) or any(
        not isinstance(cell_id, str) or safe_name(cell_id, "cell id") != cell_id for cell_id in cell_ids
    ):
        raise ValueError("cell ids must be unique, bounded, path-safe strings")
    if any(
        not isinstance(cell.get("command"), list)
        or not cell["command"]
        or any(not isinstance(part, str) or not part for part in cell["command"])
        for cell in cells
    ):
        raise ValueError("every cell command must be a nonempty string array")
    if not run_dir.parent.is_dir() or run_dir.parent.is_symlink() or run_dir.parent.resolve() != run_dir.parent.absolute():
        raise ValueError("run directory parent must exist and contain no symlink components")
    durable_mkdir(run_dir)
    artifacts_root = run_dir / "artifacts"
    durable_mkdir(artifacts_root)
    checkpoints_root = run_dir / "checkpoints"
    durable_mkdir(checkpoints_root)
    plan_sha = hashlib.sha256(canonical(plan)).hexdigest()
    run_id = f"cie-{uuid.uuid4().hex}"
    manifest = {
        "schema_version": 1,
        "run_id": run_id,
        "checkpoint_name": checkpoint_name,
        "plan_sha256": plan_sha,
        "plan": plan,
        "created_unix_ms": int(time.time() * 1000),
    }
    write_new(run_dir / "manifest.json", manifest)
    checkpoint = {
        "schema_version": 1, "run_id": run_id, "plan_sha256": plan_sha,
        "attempts": [], "completed": {}
    }
    checkpoint_path = run_dir / "checkpoints" / f"{checkpoint_name}.json"
    write_new(checkpoint_path, checkpoint)
    return manifest, checkpoint, checkpoint_path


def resume(run_dir: Path, checkpoint_name: str) -> tuple[dict, dict, Path]:
    safe_name(checkpoint_name, "checkpoint name")
    manifest_path = run_dir / "manifest.json"
    artifacts_path = run_dir / "artifacts"
    checkpoints_path = run_dir / "checkpoints"
    if any(path.is_symlink() or path.resolve() != path.absolute() for path in
           (run_dir, artifacts_path, checkpoints_path, manifest_path)):
        raise ValueError("run artifact tree was replaced or symlinked")
    manifest = load(manifest_path)
    if hashlib.sha256(canonical(manifest.get("plan"))).hexdigest() != manifest.get("plan_sha256"):
        raise ValueError("manifest plan digest mismatch; refusing result splicing")
    checkpoint_path = run_dir / "checkpoints" / f"{checkpoint_name}.json"
    if checkpoint_path.is_symlink() or checkpoint_path.resolve() != checkpoint_path.absolute():
        raise ValueError("checkpoint was replaced or symlinked")
    checkpoint = load(checkpoint_path)
    if manifest["checkpoint_name"] != checkpoint_name:
        raise ValueError("checkpoint name does not match immutable run manifest")
    if checkpoint["run_id"] != manifest["run_id"] or checkpoint["plan_sha256"] != manifest["plan_sha256"]:
        raise ValueError("checkpoint provenance mismatch; refusing result splicing")
    completed, attempts = checkpoint.get("completed"), checkpoint.get("attempts")
    if not isinstance(completed, dict) or not isinstance(attempts, list):
        raise ValueError("checkpoint evidence collections are malformed")
    artifacts_root = (run_dir / "artifacts").resolve()
    plan_cell_ids = {cell["id"] for cell in manifest["plan"]["cells"]}
    def verify_evidence(cell_id: str, evidence: object, require_valid: bool) -> None:
        safe_name(cell_id, "completed cell id")
        if cell_id not in plan_cell_ids:
            raise ValueError("checkpoint cell is absent from immutable plan; refusing result splicing")
        if not isinstance(evidence, dict) or not isinstance(evidence.get("files"), dict):
            raise ValueError("completed artifact evidence is malformed")
        resolved = {}
        for name in ("result", "stdout", "stderr"):
            item = evidence["files"].get(name)
            if not isinstance(item, dict) or not isinstance(item.get("path"), str) or not isinstance(item.get("sha256"), str):
                raise ValueError("completed artifact evidence is malformed")
            lexical_path = (run_dir / item["path"]).absolute()
            require_plain_path(artifacts_path, lexical_path)
            path = lexical_path.resolve()
            if not path.is_relative_to(artifacts_root) or file_sha256(path) != item["sha256"]:
                raise ValueError("completed artifact digest mismatch; refusing result splicing")
            resolved[name] = path
        result_path = resolved["result"]
        expected = (manifest["run_id"], manifest["plan_sha256"], checkpoint_name, cell_id)
        for name, path in resolved.items():
            artifact = load(path)
            observed = (artifact.get("run_id"), artifact.get("plan_sha256"), artifact.get("checkpoint_name"), artifact.get("cell_id"))
            if observed != expected:
                raise ValueError("completed artifact provenance mismatch; refusing result splicing")
            if require_valid and name == "result" and (artifact.get("status") != "valid" or artifact.get("score_eligible") is not True):
                raise ValueError("completed artifact provenance mismatch; refusing result splicing")
    for attempt in attempts:
        if not isinstance(attempt, dict):
            raise ValueError("attempt evidence is malformed")
        verify_evidence(attempt.get("cell_id"), attempt.get("evidence"), False)
    for cell_id, evidence in completed.items():
        verify_evidence(cell_id, evidence, True)
    return manifest, checkpoint, checkpoint_path


def run(plan_path: Path | None, run_dir: Path, checkpoint_name: str) -> int:
    if plan_path:
        manifest, checkpoint, checkpoint_path = start(plan_path, run_dir, checkpoint_name)
    else:
        manifest, checkpoint, checkpoint_path = resume(run_dir, checkpoint_name)
    completed = set(checkpoint["completed"])
    invalid = False
    for cell in manifest["plan"]["cells"]:
        cell_id = cell["id"]
        if cell_id in completed:
            continue
        artifacts_path = run_dir / "artifacts"
        artifacts_root = artifacts_path.resolve()
        if artifacts_path.is_symlink() or artifacts_root != artifacts_path.absolute():
            raise ValueError("artifact root was replaced or symlinked")
        cell_dir = artifacts_path / cell_id
        if cell_dir.exists():
            if cell_dir.is_symlink() or not cell_dir.is_dir() or not cell_dir.resolve().is_relative_to(artifacts_root):
                raise ValueError("cell artifact directory was replaced or symlinked")
        else:
            durable_mkdir(cell_dir)
        attempt_dir = cell_dir / f"attempt-{uuid.uuid4().hex}"
        if cell_dir.is_symlink() or not attempt_dir.parent.resolve().is_relative_to(artifacts_root):
            raise ValueError("attempt directory escapes run directory")
        durable_mkdir(attempt_dir)
        started = int(time.time() * 1000)
        infrastructure_error = None
        try:
            result = subprocess.run(cell["command"], capture_output=True, timeout=cell.get("timeout_seconds", 900))
            status = "valid" if result.returncode == 0 else "infrastructure-invalid"
            returncode = result.returncode
            stdout, stderr = result.stdout, result.stderr
        except subprocess.TimeoutExpired as error:
            def captured(value: str | bytes | None) -> bytes:
                return value.encode() if isinstance(value, str) else (value or b"")
            status, returncode = "infrastructure-invalid", None
            stdout, stderr = captured(error.stdout), captured(error.stderr)
            infrastructure_error = str(error)
        except OSError as error:
            status, returncode, stdout, stderr = "infrastructure-invalid", None, b"", b""
            infrastructure_error = str(error)
        artifact = {
            "schema_version": 1,
            "run_id": manifest["run_id"],
            "plan_sha256": manifest["plan_sha256"],
            "checkpoint_name": checkpoint_name,
            "cell_id": cell_id,
            "status": status,
            "score_eligible": status == "valid",
            "returncode": returncode,
            "infrastructure_error": infrastructure_error,
            "started_unix_ms": started,
            "finished_unix_ms": int(time.time() * 1000),
            "command": cell["command"],
        }
        write_new(attempt_dir / "result.json", artifact)
        output_provenance = {
            "schema_version": 1,
            "run_id": manifest["run_id"],
            "plan_sha256": manifest["plan_sha256"],
            "checkpoint_name": checkpoint_name,
            "cell_id": cell_id,
        }
        write_new(attempt_dir / "stdout.json", {**output_provenance, "encoding": "base64", "data": base64.b64encode(stdout).decode()})
        write_new(attempt_dir / "stderr.json", {**output_provenance, "encoding": "base64", "data": base64.b64encode(stderr).decode()})
        evidence = {
            "files": {
                name: {
                    "path": str((attempt_dir / f"{name}.json").relative_to(run_dir)),
                    "sha256": file_sha256(attempt_dir / f"{name}.json"),
                }
                for name in ("result", "stdout", "stderr")
            }
        }
        checkpoint["attempts"].append({"cell_id": cell_id, "evidence": evidence})
        replace(checkpoint_path, checkpoint)
        invalid |= status != "valid"
        if invalid:
            return 2
        checkpoint["completed"][cell_id] = evidence
        replace(checkpoint_path, checkpoint)
    return 2 if invalid else 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan", type=Path, help="new run plan; omit only when resuming")
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--checkpoint", required=True)
    args = parser.parse_args()
    try:
        return run(args.plan, args.run_dir, args.checkpoint)
    except (FileExistsError, FileNotFoundError, KeyError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
