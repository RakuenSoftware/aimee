#!/usr/bin/env python3
"""Exercise the shipping LSP manager against pinned real providers."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).resolve().parent / "fixtures"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_tree(path: Path) -> str:
    digest = hashlib.sha256()
    for child in sorted(item for item in path.rglob("*") if item.is_file()):
        digest.update(child.relative_to(path).as_posix().encode())
        digest.update(b"\0")
        digest.update(child.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def process_tree_sample(root_pid: int) -> tuple[set[int], int]:
    """Return descendants and summed RSS using the portable POSIX process table."""
    completed = subprocess.run(
        ["ps", "-axo", "pid=,ppid=,rss="],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if completed.returncode:
        return {root_pid}, 0
    children: dict[int, list[int]] = {}
    rss_by_pid: dict[int, int] = {}
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) != 3:
            continue
        try:
            pid, parent, rss = (int(value) for value in fields)
        except ValueError:
            continue
        children.setdefault(parent, []).append(pid)
        rss_by_pid[pid] = rss
    found = {root_pid}
    pending = [root_pid]
    while pending:
        for child in children.get(pending.pop(), []):
            if child not in found:
                found.add(child)
                pending.append(child)
    return found, sum(rss_by_pid.get(pid, 0) for pid in found)


def run_observed(command: list[str], env: dict[str, str], timeout_seconds: float) -> dict:
    started = time.monotonic()
    process = subprocess.Popen(
        command,
        cwd=ROOT,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    peak_processes = 1
    peak_rss_kib = 0
    timed_out = False
    while process.poll() is None:
        descendants, rss = process_tree_sample(process.pid)
        peak_processes = max(peak_processes, len(descendants))
        peak_rss_kib = max(peak_rss_kib, rss)
        if time.monotonic() - started >= timeout_seconds:
            timed_out = True
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
            break
        time.sleep(0.05)
    stdout, stderr = process.communicate()
    elapsed_ms = round((time.monotonic() - started) * 1000)
    parsed = None
    if stdout.strip():
        try:
            parsed = json.loads(stdout.strip().splitlines()[-1])
        except json.JSONDecodeError:
            parsed = None
    return {
        "exit_code": process.returncode,
        "timed_out": timed_out,
        "elapsed_ms": elapsed_ms,
        "peak_process_tree_count": peak_processes,
        "peak_process_tree_rss_kib": peak_rss_kib,
        "stdout": stdout.strip(),
        "stderr": stderr.strip(),
        "probe": parsed,
    }


def command_version(command: list[str]) -> str:
    completed = subprocess.run(
        command, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=15, check=False
    )
    return completed.stdout.strip()


def provider_record(
    name: str,
    command: Path,
    server_arg: str,
    fixture: Path,
    file_name: str,
    line: int,
    column: int,
    expected_line: int,
    min_references: int,
    lsp_test: Path,
    env: dict[str, str],
    version_command: list[str],
    probe_mode: str = "--real-provider",
) -> dict:
    record = {
        "name": name,
        "configured_command": str(command),
        "available": command.is_file() and os.access(command, os.X_OK),
        "fixture_sha256": sha256_tree(fixture),
    }
    if not record["available"]:
        record["classification"] = "missing_binary"
        return record
    resolved = command.resolve()
    record["resolved_command"] = str(resolved)
    record["command_sha256"] = sha256_file(resolved)
    record["version"] = command_version(version_command)
    source = (fixture / file_name).resolve()
    invocation = [
        str(lsp_test), probe_mode, str(command), server_arg,
        str(fixture.resolve()), str(source), str(line), str(column),
        str(source), str(expected_line), str(min_references),
    ]
    record["observation"] = run_observed(invocation, env, 60)
    probe = record["observation"].get("probe") or {}
    if record["observation"]["timed_out"]:
        record["classification"] = "timeout"
    elif probe.get("definition_matched") and probe.get("reference_count", 0) >= min_references:
        record["classification"] = "ready"
    elif (
        name == "pyright"
        and probe.get("definition_count") == 0
        and probe.get("reference_count") in (0, min_references)
    ):
        record["classification"] = "location_link_unsupported_unsynchronized"
    else:
        record["classification"] = "semantic_probe_failed"
    return record


def baseline_matches(report: dict) -> tuple[bool, list[str]]:
    errors: list[str] = []
    providers = {item["name"]: item for item in report["providers"]}
    for name in ("gopls", "pyright"):
        if not providers[name]["available"]:
            errors.append(f"{name}: pinned binary is missing")
            continue
        observation = providers[name]["observation"]
        probe = observation.get("probe") or {}
        if observation["timed_out"]:
            errors.append(f"{name}: probe timed out")
        if probe.get("cold_diagnostics") != 0 or probe.get("cold_active_servers") != 0:
            errors.append(f"{name}: cold false-empty baseline changed")
        if probe.get("active_servers_after_query") != 1:
            errors.append(f"{name}: did not retain exactly one active provider")
        if not isinstance(probe.get("cold_definition_ms"), (int, float)):
            errors.append(f"{name}: cold definition latency was not recorded")
        if not isinstance(probe.get("warm_references_ms"), (int, float)):
            errors.append(f"{name}: warm reference latency was not recorded")
    if providers["gopls"].get("classification") != "ready":
        errors.append("gopls: definition/reference baseline is not ready")
    gopls_probe = providers["gopls"].get("observation", {}).get("probe") or {}
    if gopls_probe.get("reference_count", 0) < 3:
        errors.append("gopls: references were incomplete")
    pyright_probe = providers["pyright"].get("observation", {}).get("probe") or {}
    if pyright_probe.get("reference_count") not in (0, 3):
        errors.append("pyright: reference result was outside the observed unsynchronized states")
    if providers["pyright"].get("classification") != "location_link_unsupported_unsynchronized":
        errors.append("pyright: expected LocationLink and synchronization gaps were not reproduced")
    return not errors, errors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lsp-test", type=Path, required=True)
    parser.add_argument("--gopls", type=Path, required=True)
    parser.add_argument("--pyright-langserver", type=Path, required=True)
    parser.add_argument("--pyright", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--assert-baseline", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    env = os.environ.copy()
    report = {
        "schema_version": 1,
        "purpose": "shipping LSP manager S0 baseline; no S1 behavior enabled",
        "source_commit": subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True, capture_output=True, check=True
        ).stdout.strip(),
        "environment": {
            "platform": platform.platform(),
            "python": platform.python_version(),
        },
        "providers": [
            provider_record(
                "gopls", args.gopls, "-", FIXTURES / "go", "main.go", 8, 9, 3, 3,
                args.lsp_test.resolve(), env, [str(args.gopls), "version"],
            ),
            provider_record(
                "pyright", args.pyright_langserver, "--stdio", FIXTURES / "python",
                "sample.py", 6, 12, 1, 3, args.lsp_test.resolve(), env,
                [str(args.pyright), "--version"],
            ),
        ],
    }
    matched, errors = baseline_matches(report)
    report["baseline_matched"] = matched
    report["baseline_errors"] = errors
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered)
    else:
        sys.stdout.write(rendered)
    return 1 if args.assert_baseline and not matched else 0


if __name__ == "__main__":
    raise SystemExit(main())
