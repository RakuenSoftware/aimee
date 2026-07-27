#!/usr/bin/env python3
"""Run the pinned six-model A:B baseline one model at a time on `.254`.

The controller deliberately stops the deployed LLM container during the sweep so
benchmark and production processes cannot share VRAM. It restores the exact
container in a finally block and all case runners are resumable.
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.request
from pathlib import Path
from typing import Any


ETTIN_CONTROLS = (
    {"label": "ettin68m", "tier": "cpu", "ngl": "0", "execution": "cpu"},
    {"label": "ettin400m", "tier": "mid", "ngl": "99", "execution": "gpu"},
)
ETTIN_LOAD_PROFILE = {
    "workers": 8,
    "pairs_per_request": 4,
    "parallel_slots": 32,
    "context_tokens": 65536,
    "logical_batch_tokens": 2048,
    "physical_batch_tokens": 2048,
}
MODEL_LOAD_PROFILES = {
    "gemma4_e2b": {
        "synthesis": {"workers": 64, "parallel_slots": 64, "context_tokens": 131072, "physical_batch_tokens": 2048},
        "embedding": {"parallel_slots": 64, "context_tokens": 131072, "physical_batch_tokens": 2048, "batch_size": 64},
    },
    "gemma4_e4b": {
        "synthesis": {"workers": 64, "parallel_slots": 64, "context_tokens": 131072, "physical_batch_tokens": 2048},
        "embedding": {"parallel_slots": 64, "context_tokens": 131072, "physical_batch_tokens": 2048, "batch_size": 64},
    },
    "gemma4_12b": {
        "synthesis": {"workers": 32, "parallel_slots": 32, "context_tokens": 65536, "physical_batch_tokens": 2048},
        "embedding": {"parallel_slots": 16, "context_tokens": 32768, "physical_batch_tokens": 2048, "batch_size": 16},
    },
    "gemma4_26b_a4b": {
        "synthesis": {"workers": 16, "parallel_slots": 16, "context_tokens": 32768, "physical_batch_tokens": 2048},
        "embedding": {"parallel_slots": 16, "context_tokens": 32768, "physical_batch_tokens": 2048, "batch_size": 16},
    },
    "gemma4_31b": {
        "synthesis": {"workers": 8, "parallel_slots": 8, "context_tokens": 16384, "physical_batch_tokens": 2048},
        "embedding": {"parallel_slots": 8, "context_tokens": 16384, "physical_batch_tokens": 2048, "batch_size": 8},
    },
    "qwen36_35b_a3b": {
        "synthesis": {"workers": 4, "parallel_slots": 4, "context_tokens": 8192, "physical_batch_tokens": 2048},
        "embedding": {"parallel_slots": 4, "context_tokens": 8192, "physical_batch_tokens": 2048, "batch_size": 4},
    },
}
MODEL_MODES = ("synthesis", "embedding")


def parse_modes(value: str) -> tuple[str, ...]:
    modes = tuple(part.strip() for part in value.split(",") if part.strip())
    if not modes:
        raise ValueError("at least one model mode is required")
    if len(set(modes)) != len(modes):
        raise ValueError("model modes must not be repeated")
    unknown = set(modes) - set(MODEL_MODES)
    if unknown:
        raise ValueError(f"unknown model modes: {sorted(unknown)}")
    return modes


def run(command: list[str], *, capture: bool = False, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=capture, check=check)


def sha256(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(8 * 1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def wait_health(url: str, timeout: int = 900, accepted_status: str = "ok") -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last_error = ""
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=5) as response:
                payload = json.load(response)
            if payload.get("status") == accepted_status:
                return payload
        except Exception as exc:  # noqa: BLE001 - retained for the server log
            last_error = f"{type(exc).__name__}: {exc}"
        time.sleep(2)
    raise RuntimeError(f"server did not become healthy: {last_error}")


def docker_cmd(socket: str, *parts: str) -> list[str]:
    return ["docker", "-H", f"unix://{socket}", *parts]


def inspect_running(socket: str, name: str) -> bool:
    result = run(docker_cmd(socket, "inspect", "--format", "{{.State.Running}}", name), capture=True, check=False)
    return result.returncode == 0 and result.stdout.strip() == "true"


def wait_container_gone(socket: str, name: str, timeout: int = 60) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = run(docker_cmd(socket, "inspect", name), capture=True, check=False)
        if result.returncode != 0:
            return
        time.sleep(0.25)
    raise RuntimeError(f"container name was not released: {name}")


def write_state(path: Path, **values: Any) -> None:
    current = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    current.update(values)
    path.write_text(json.dumps(current, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def hardware_snapshot() -> dict[str, int]:
    result: dict[str, int] = {}
    paths = {
        "vram_used_bytes": Path("/sys/class/drm/card0/device/mem_info_vram_used"),
        "vram_total_bytes": Path("/sys/class/drm/card0/device/mem_info_vram_total"),
        "gpu_busy_percent": Path("/sys/class/drm/card0/device/gpu_busy_percent"),
    }
    for name, path in paths.items():
        if path.exists():
            result[name] = int(path.read_text(encoding="utf-8").strip())
    for line in Path("/proc/meminfo").read_text(encoding="utf-8").splitlines():
        key, _, raw = line.partition(":")
        if key in {"MemAvailable", "SwapTotal", "SwapFree"}:
            result[f"{key.lower()}_bytes"] = int(raw.strip().split()[0]) * 1024
    return result


def start_server(
    socket: str,
    image: str,
    root: Path,
    model_file: str,
    mode: str,
    load_profile: dict[str, int],
    log_path: Path,
) -> float:
    started = time.monotonic()
    run(docker_cmd(socket, "stop", "gemma4-baseline-server"), check=False, capture=True)
    wait_container_gone(socket, "gemma4-baseline-server")
    args = [
        "run", "--detach", "--rm", "--name", "gemma4-baseline-server", "--network", "host",
        "--device", "/dev/dri:/dev/dri", "--volume", f"{root}:/bench", "--entrypoint", "/opt/llama/llama-server",
        image, "-m", f"/bench/models/{model_file}", "-ngl", "99", "-fa", "on", "--cache-ram", "512",
        "--host", "0.0.0.0", "--port", "8920",
    ]
    if mode == "synthesis":
        args += [
            "--jinja", "--ctx-size", str(load_profile["context_tokens"]),
            "-ub", str(load_profile["physical_batch_tokens"]),
            "-np", str(load_profile["parallel_slots"]),
        ]
    elif mode == "embedding":
        args += [
            "--embeddings", "--pooling", "last", "--ctx-size", str(load_profile["context_tokens"]),
            "-ub", str(load_profile["physical_batch_tokens"]),
            "-np", str(load_profile["parallel_slots"]),
        ]
    else:
        raise ValueError(mode)
    result = run(docker_cmd(socket, *args), capture=True)
    container_id = result.stdout.strip()
    try:
        wait_health("http://127.0.0.1:8920/health")
    except Exception:
        logs = run(docker_cmd(socket, "logs", container_id), capture=True, check=False)
        log_path.write_text(logs.stdout + logs.stderr, encoding="utf-8")
        raise
    return time.monotonic() - started


def stop_server(socket: str, log_path: Path) -> None:
    logs = run(docker_cmd(socket, "logs", "gemma4-baseline-server"), capture=True, check=False)
    log_path.write_text(logs.stdout + logs.stderr, encoding="utf-8")
    run(docker_cmd(socket, "stop", "gemma4-baseline-server"), capture=True, check=False)
    wait_container_gone(socket, "gemma4-baseline-server")


def start_ettin_server(
    socket: str,
    image: str,
    deployed_models: Path,
    repo: Path,
    tier: str,
    ngl: str,
    log_path: Path,
) -> float:
    started = time.monotonic()
    run(docker_cmd(socket, "stop", "gemma4-baseline-server"), check=False, capture=True)
    wait_container_gone(socket, "gemma4-baseline-server")
    command = docker_cmd(
        socket, "run", "--detach", "--rm", "--name", "gemma4-baseline-server", "--network", "host",
        "--device", "/dev/dri:/dev/dri", "--volume", f"{deployed_models}:/models",
        "--volume", f"{repo / 'scripts/aimee-llm-supervisor.sh'}:/opt/aimee/aimee-llm-supervisor.sh:ro",
        "--entrypoint", "/bin/bash",
        "--env", "AIMEE_LLM_EMBED_MODE=off", "--env", "AIMEE_LLM_SYNTH_MODE=off",
        "--env", "AIMEE_LLM_RERANK_MODE=local", "--env", f"AIMEE_LLM_RERANK_TIER={tier}",
        "--env", f"AIMEE_LLM_NGL={ngl}",
        "--env", f"AIMEE_LLM_RERANK_BATCH={ETTIN_LOAD_PROFILE['logical_batch_tokens']}",
        "--env", f"AIMEE_LLM_RERANK_UBATCH={ETTIN_LOAD_PROFILE['physical_batch_tokens']}",
        "--env", f"AIMEE_LLM_RERANK_CTX={ETTIN_LOAD_PROFILE['context_tokens']}",
        "--env", f"AIMEE_LLM_RERANK_PARALLEL={ETTIN_LOAD_PROFILE['parallel_slots']}",
        "--env", "AIMEE_LLM_PORT=8920", image, "/opt/aimee/aimee-llm-supervisor.sh",
    )
    result = run(command, capture=True)
    try:
        wait_health("http://127.0.0.1:8920/health/rerank", accepted_status="ready")
    except Exception:
        logs = run(docker_cmd(socket, "logs", result.stdout.strip()), capture=True, check=False)
        log_path.write_text(logs.stdout + logs.stderr, encoding="utf-8")
        raise
    return time.monotonic() - started


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("/mnt/media/gemma4-baseline"))
    parser.add_argument("--repo", type=Path, default=Path("/mnt/media/gemma4-baseline/repo"))
    parser.add_argument("--socket", default="/run/smoothnas-runtime/docker.sock")
    parser.add_argument("--production-container", default="aimee-llm-llm")
    parser.add_argument("--deployed-models", type=Path, default=Path("/mnt/media/.plugins/aimee-llm/llm/models"))
    parser.add_argument("--max-cases", type=int, default=0, help="Zero runs the complete 10,000-case suites")
    parser.add_argument("--labels", help="Comma-separated model labels for a resumable subset run")
    parser.add_argument(
        "--modes",
        default=",".join(MODEL_MODES),
        help="Comma-separated model modes; use synthesis alone for synthesis-only recovery",
    )
    parser.add_argument("--skip-ettin", action="store_true")
    args = parser.parse_args()
    try:
        selected_modes = parse_modes(args.modes)
    except ValueError as exc:
        parser.error(str(exc))
    args.root.mkdir(parents=True, exist_ok=True)
    results_dir = args.root / (f"results_smoke_{args.max_cases}" if args.max_cases else "results")
    logs_dir = args.root / "logs"
    results_dir.mkdir(exist_ok=True)
    logs_dir.mkdir(exist_ok=True)
    lock_handle = (args.root / "sweep.lock").open("w", encoding="utf-8")
    fcntl.flock(lock_handle, fcntl.LOCK_EX | fcntl.LOCK_NB)

    manifest = json.loads((args.root / "models.json").read_text(encoding="utf-8"))
    fixture_manifest = json.loads(
        (args.repo / "benchmarks/fixtures/gemma4-unified/ab-v1/manifest.json").read_text(encoding="utf-8")
    )
    selected_labels = set(args.labels.split(",")) if args.labels else {model["label"] for model in manifest["models"]}
    selected_models = [model for model in manifest["models"] if model["label"] in selected_labels]
    unknown_labels = selected_labels - {model["label"] for model in selected_models}
    if unknown_labels:
        raise RuntimeError(f"unknown model labels: {sorted(unknown_labels)}")
    view_matrix = fixture_manifest.get("baseline_model_views", {})
    for model in selected_models:
        if view_matrix.get(model["label"]) != {
            "synthesis": "required",
            "embedding": "required_native_width",
            "reranking": "excluded_instruction_base_not_cross_encoder",
        }:
            raise RuntimeError(f"fixture manifest does not require the expected views for {model['label']}")
    if not args.skip_ettin:
        for control in ETTIN_CONTROLS:
            if view_matrix.get(control["label"], {}).get("reranking") != "required_incumbent_control":
                raise RuntimeError(f"fixture manifest does not require the {control['label']} reranking control")
    artifact_rows = []
    for model in selected_models:
        path = args.root / "models" / model["file"]
        if not path.exists() or path.stat().st_size != model["bytes"]:
            raise RuntimeError(f"artifact not ready: {path}")
        digest = sha256(path)
        if model.get("sha256") and digest != model["sha256"]:
            raise RuntimeError(f"artifact SHA-256 mismatch: {path}")
        artifact_rows.append({"label": model["label"], "file": model["file"], "bytes": path.stat().st_size, "sha256": digest})
    (args.root / "models" / "ARTIFACTS.json").write_text(json.dumps(artifact_rows, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    state_path = results_dir / "RUN_STATE.json"
    environment = {
        "models_manifest_sha256": sha256(args.root / "models.json"),
        "fixtures_manifest_sha256": sha256(args.repo / "benchmarks/fixtures/gemma4-unified/ab-v1/manifest.json"),
        "container_image": manifest["runtime"]["container_image"],
        "llama_cpp_build": manifest["runtime"]["llama_cpp_build"],
        "max_cases": args.max_cases or 10000,
        "selected_labels": [model["label"] for model in selected_models],
        "selected_modes": list(selected_modes),
        "skip_ettin": args.skip_ettin,
        "ettin_load_profile": ETTIN_LOAD_PROFILE,
        "model_load_profiles": MODEL_LOAD_PROFILES,
        "hardware_identity": {
            "gpu_vendor": Path("/sys/class/drm/card0/device/vendor").read_text(encoding="utf-8").strip(),
            "gpu_device": Path("/sys/class/drm/card0/device/device").read_text(encoding="utf-8").strip(),
            "gpu_vram_total_bytes": hardware_snapshot().get("vram_total_bytes", 0),
        },
        "host": run(["hostname"], capture=True).stdout.strip(),
        "kernel": run(["uname", "-srmo"], capture=True).stdout.strip(),
        "started_unix": int(time.time()),
    }
    state_path.write_text(
        json.dumps({"status": "preparing", "environment": environment, "completed": []}, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    production_was_running = inspect_running(args.socket, args.production_container)
    current_log: Path | None = None
    completed: list[dict[str, str]] = []
    try:
        if production_was_running:
            run(docker_cmd(args.socket, "stop", args.production_container))
        write_state(state_path, status="running", production_was_running=production_was_running)
        if not args.skip_ettin:
            for control in ETTIN_CONTROLS:
                label = control["label"]
                ettin_results = results_dir / label
                ettin_results.mkdir(exist_ok=True)
                current_log = logs_dir / f"server_{label}_reranking.log"
                write_state(state_path, active={"label": label, "mode": "reranking"}, completed=completed)
                load_seconds = start_ettin_server(
                    args.socket,
                    manifest["runtime"]["container_image"],
                    args.deployed_models,
                    args.repo,
                    control["tier"],
                    control["ngl"],
                    current_log,
                )
                after_load = hardware_snapshot()
                command = [
                    "python3", str(args.repo / "benchmarks/gemma4_baseline/run_reranking_ab.py"),
                    "--endpoint", "http://127.0.0.1:8920", "--label", label,
                    "--bundle", str(args.repo / "benchmarks/fixtures/gemma4-unified/ab-v1"),
                    "--output-dir", str(ettin_results),
                    "--pair-batch-size", str(ETTIN_LOAD_PROFILE["pairs_per_request"]),
                    "--workers", str(ETTIN_LOAD_PROFILE["workers"]), "--timeout", "300",
                    "--environment-note",
                    f"isolated_{control['execution']}_rx_7900_xtx_host_concurrent_quality_sweep",
                ]
                if args.max_cases:
                    command += ["--max-cases", str(args.max_cases)]
                with (logs_dir / f"runner_{label}_reranking.log").open("a", encoding="utf-8") as runner_log:
                    process = subprocess.run(command, text=True, stdout=runner_log, stderr=subprocess.STDOUT)
                after_run = hardware_snapshot()
                (ettin_results / "hardware_reranking.json").write_text(
                    json.dumps(
                        {
                            "cold_load_seconds": load_seconds,
                            "load_profile": ETTIN_LOAD_PROFILE,
                            "after_load": after_load,
                            "after_run": after_run,
                        },
                        indent=2,
                        sort_keys=True,
                    ) + "\n",
                    encoding="utf-8",
                )
                stop_server(args.socket, current_log)
                current_log = None
                if process.returncode:
                    raise RuntimeError(f"{label} reranking runner exited {process.returncode}")
                completed.append({"label": label, "mode": "reranking"})
                write_state(state_path, completed=completed)
        for model in selected_models:
            label = model["label"]
            model_results = results_dir / label
            model_results.mkdir(exist_ok=True)
            for mode in selected_modes:
                load_profile = MODEL_LOAD_PROFILES[label][mode]
                current_log = logs_dir / f"server_{label}_{mode}.log"
                write_state(
                    state_path,
                    active={"label": label, "mode": mode, "load_profile": load_profile},
                    completed=completed,
                )
                load_seconds = start_server(
                    args.socket,
                    manifest["runtime"]["container_image"],
                    args.root,
                    model["file"],
                    mode,
                    load_profile,
                    current_log,
                )
                after_load = hardware_snapshot()
                if mode == "synthesis":
                    command = [
                        "python3", str(args.repo / "benchmarks/gemma4_baseline/run_synthesis_ab.py"),
                        "--endpoint", "http://127.0.0.1:8920", "--model", label, "--label", label,
                        "--bundle", str(args.repo / "benchmarks/fixtures/gemma4-unified/ab-v1"),
                        "--output-dir", str(model_results), "--workers", str(load_profile["workers"]), "--timeout", "300",
                    ]
                else:
                    command = [
                        "python3", str(args.repo / "benchmarks/gemma4_baseline/run_embedding_ab.py"),
                        "--endpoint", "http://127.0.0.1:8920", "--model", label, "--label", label,
                        "--bundle", str(args.repo / "benchmarks/fixtures/gemma4-unified/ab-v1"),
                        "--output-dir", str(model_results), "--batch-size", str(load_profile["batch_size"]), "--timeout", "300",
                    ]
                if args.max_cases:
                    command += ["--max-cases", str(args.max_cases)]
                with (logs_dir / f"runner_{label}_{mode}.log").open("a", encoding="utf-8") as runner_log:
                    process = subprocess.run(command, text=True, stdout=runner_log, stderr=subprocess.STDOUT)
                after_run = hardware_snapshot()
                (model_results / f"hardware_{mode}.json").write_text(
                    json.dumps(
                        {
                            "cold_load_seconds": load_seconds,
                            "load_profile": load_profile,
                            "after_load": after_load,
                            "after_run": after_run,
                        },
                        indent=2,
                        sort_keys=True,
                    ) + "\n",
                    encoding="utf-8",
                )
                stop_server(args.socket, current_log)
                current_log = None
                if process.returncode:
                    raise RuntimeError(f"{label} {mode} runner exited {process.returncode}")
                completed.append({"label": label, "mode": mode})
                write_state(state_path, completed=completed)
        write_state(state_path, status="complete", active=None, completed=completed, completed_unix=int(time.time()))
        return 0
    except Exception as exc:
        if current_log is not None:
            stop_server(args.socket, current_log)
        write_state(state_path, status="failed", error=f"{type(exc).__name__}: {exc}", completed=completed, failed_unix=int(time.time()))
        raise
    finally:
        if production_was_running and not inspect_running(args.socket, args.production_container):
            run(docker_cmd(args.socket, "start", args.production_container), check=False)
            try:
                wait_health("http://192.168.1.254:8742/health", timeout=900)
                write_state(state_path, production_restored=True)
            except Exception as exc:  # noqa: BLE001
                write_state(state_path, production_restored=False, production_restore_error=f"{type(exc).__name__}: {exc}")


if __name__ == "__main__":
    sys.exit(main())
