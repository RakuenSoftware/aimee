#!/usr/bin/env python3
"""Run the remaining `.254` benchmark stages in a fail-closed sequence."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import time
from pathlib import Path
from typing import Any


def write_json_atomic(path: Path, payload: dict[str, Any]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def stage_plan(root: Path, repo: Path) -> list[dict[str, Any]]:
    python = "python3"
    modules = repo / "benchmarks/gemma4_baseline"
    return [
        {
            "name": "eurobert_rerankers",
            "command": [
                python,
                "-u",
                str(modules / "run_254_eurobert_rerankers.py"),
                "--root",
                str(root),
                "--repo",
                str(repo),
                "--wait-for-lock",
                "--handoff-state",
                str(root / "results/RUN_STATE.json"),
            ],
        },
        {
            "name": "gemma4_e4b_synthesis_recovery",
            "command": [
                python,
                "-u",
                str(modules / "run_254_sweep.py"),
                "--root",
                str(root),
                "--repo",
                str(repo),
                "--skip-ettin",
                "--labels",
                "gemma4_e4b",
                "--modes",
                "synthesis",
            ],
        },
        {
            "name": "remaining_model_views",
            "command": [
                python,
                "-u",
                str(modules / "run_254_sweep.py"),
                "--root",
                str(root),
                "--repo",
                str(repo),
                "--skip-ettin",
                "--labels",
                "gemma4_26b_a4b,gemma4_31b,qwen36_35b_a3b",
            ],
        },
    ]


def plan_sha256(plan: list[dict[str, Any]]) -> str:
    encoded = json.dumps(plan, separators=(",", ":"), sort_keys=True).encode()
    return hashlib.sha256(encoded).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("/mnt/media/gemma4-baseline"))
    parser.add_argument("--repo", type=Path, default=Path("/mnt/media/gemma4-baseline/repo"))
    args = parser.parse_args()

    logs = args.root / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    state_path = args.root / "remaining_chain_state.json"
    plan = stage_plan(args.root, args.repo)
    fingerprint = plan_sha256(plan)
    state: dict[str, Any] = {}
    if state_path.exists():
        state = json.loads(state_path.read_text(encoding="utf-8"))
        prior_fingerprint = state.get("plan_sha256")
        if prior_fingerprint and prior_fingerprint != fingerprint:
            raise RuntimeError("refusing to resume a different remaining-stage plan")
        if state.get("status") == "complete":
            return 0
    completed = list(state.get("completed", []))
    completed_names = {entry["name"] for entry in completed}
    started_unix = int(state.get("started_unix", time.time()))
    write_json_atomic(
        state_path,
        {
            "status": "running",
            "plan": plan,
            "plan_sha256": fingerprint,
            "completed": completed,
            "started_unix": started_unix,
            "resumed_unix": int(time.time()),
        },
    )

    active: str | None = None
    try:
        for stage in plan:
            if stage["name"] in completed_names:
                continue
            active = stage["name"]
            write_json_atomic(
                state_path,
                {
                    "status": "running",
                    "plan": plan,
                    "plan_sha256": fingerprint,
                    "completed": completed,
                    "active": active,
                    "stage_started_unix": int(time.time()),
                    "started_unix": started_unix,
                },
            )
            log_path = logs / f"chain_{active}.log"
            with log_path.open("a", encoding="utf-8") as log:
                process = subprocess.run(stage["command"], text=True, stdout=log, stderr=subprocess.STDOUT)
            if process.returncode:
                raise RuntimeError(f"{active} exited {process.returncode}; see {log_path}")
            completed.append({"name": active, "completed_unix": int(time.time()), "log": str(log_path)})
            completed_names.add(active)
        write_json_atomic(
            state_path,
            {
                "status": "complete",
                "plan": plan,
                "plan_sha256": fingerprint,
                "completed": completed,
                "active": None,
                "started_unix": started_unix,
                "completed_unix": int(time.time()),
            },
        )
        return 0
    except Exception as exc:
        write_json_atomic(
            state_path,
            {
                "status": "failed",
                "plan": plan,
                "plan_sha256": fingerprint,
                "completed": completed,
                "active": active,
                "error": f"{type(exc).__name__}: {exc}",
                "started_unix": started_unix,
                "failed_unix": int(time.time()),
            },
        )
        raise


if __name__ == "__main__":
    raise SystemExit(main())
