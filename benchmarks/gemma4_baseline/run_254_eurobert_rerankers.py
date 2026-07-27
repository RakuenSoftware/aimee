#!/usr/bin/env python3
"""Train and benchmark the pinned EuroBERT reranker extension on `.254`."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import subprocess
import time
import urllib.request
from pathlib import Path
from typing import Any

from smoke_eurobert_runtime import assert_completed_smoke, expected_smoke_provenance
from train_eurobert_reranker import assert_completed_training_dir, expected_provenance


def run(command: list[str], *, capture: bool = False, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=capture, check=check)


def docker_cmd(socket: str, *parts: str) -> list[str]:
    return ["docker", "-H", f"unix://{socket}", *parts]


def manifest_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inspect_running(socket: str, name: str) -> bool:
    result = run(docker_cmd(socket, "inspect", "--format", "{{.State.Running}}", name), capture=True, check=False)
    return result.returncode == 0 and result.stdout.strip() == "true"


def wait_container_gone(socket: str, name: str, timeout: int = 60) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if run(docker_cmd(socket, "inspect", name), capture=True, check=False).returncode != 0:
            return
        time.sleep(0.25)
    raise RuntimeError(f"container name was not released: {name}")


def wait_health(url: str, timeout: int = 900) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last_error = ""
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=5) as response:
                payload = json.load(response)
            if payload.get("status") == "ready":
                return payload
        except Exception as exc:  # noqa: BLE001 - retained for startup diagnosis
            last_error = f"{type(exc).__name__}: {exc}"
        time.sleep(2)
    raise RuntimeError(f"reranker did not become healthy: {last_error}")


def write_state(path: Path, **values: Any) -> None:
    current = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    current.update(values)
    path.write_text(json.dumps(current, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def build_image(socket: str, repo: Path, manifest: dict[str, Any]) -> None:
    runtime = manifest["runtime"]
    packages = runtime["packages"]
    command = docker_cmd(
        socket,
        "build",
        "--file",
        str(repo / "benchmarks/gemma4_baseline/Dockerfile.eurobert-reranker"),
        "--tag",
        runtime["image_tag"],
        "--build-arg",
        f"ROCM_PYTORCH_IMAGE={runtime['base_image']}",
        "--build-arg",
        f"SENTENCE_TRANSFORMERS_VERSION={packages['sentence-transformers']}",
        "--build-arg",
        f"TRANSFORMERS_VERSION={packages['transformers']}",
        "--build-arg",
        f"DATASETS_VERSION={packages['datasets']}",
        "--build-arg",
        f"ACCELERATE_VERSION={packages['accelerate']}",
        "--build-arg",
        f"SAFETENSORS_VERSION={packages['safetensors']}",
        str(repo),
    )
    run(command)


def gpu_container_prefix(
    socket: str,
    name: str,
    root: Path,
    repo: Path,
    image: str,
    *,
    detach: bool = False,
) -> list[str]:
    command = docker_cmd(socket, "run")
    if detach:
        command.append("--detach")
    command += [
        "--rm",
        "--name",
        name,
        "--network",
        "host",
        "--ipc",
        "host",
        "--device",
        "/dev/kfd:/dev/kfd",
        "--device",
        "/dev/dri:/dev/dri",
        "--security-opt",
        "seccomp=unconfined",
        "--volume",
        f"{root}:/bench",
        "--volume",
        f"{repo}:/repo:ro",
        "--env",
        "HF_HOME=/bench/hf-cache",
        "--env",
        "HSA_OVERRIDE_GFX_VERSION=11.0.0",
        image,
    ]
    return command


def train_model(
    socket: str,
    root: Path,
    repo: Path,
    image: str,
    manifest_path: Path,
    label: str,
    output_dir: Path,
) -> None:
    command = gpu_container_prefix(socket, f"eurobert-train-{label}", root, repo, image)
    command += [
        "/repo/benchmarks/gemma4_baseline/train_eurobert_reranker.py",
        "--manifest",
        f"/repo/{manifest_path.relative_to(repo)}",
        "--label",
        label,
        "--output-dir",
        f"/bench/{output_dir.relative_to(root)}",
        "--resume",
    ]
    run(command)


def smoke_model(
    socket: str,
    root: Path,
    repo: Path,
    image: str,
    manifest_path: Path,
    label: str,
    output_path: Path,
) -> None:
    command = gpu_container_prefix(socket, f"eurobert-smoke-{label}", root, repo, image)
    command += [
        "/repo/benchmarks/gemma4_baseline/smoke_eurobert_runtime.py",
        "--manifest",
        f"/repo/{manifest_path.relative_to(repo)}",
        "--label",
        label,
        "--output",
        f"/bench/{output_path.relative_to(root)}",
    ]
    run(command)


def start_server(
    socket: str,
    root: Path,
    repo: Path,
    image: str,
    label: str,
    model_dir: Path,
    serving: dict[str, Any],
) -> tuple[str, dict[str, Any]]:
    name = "eurobert-reranker-server"
    run(docker_cmd(socket, "stop", name), capture=True, check=False)
    wait_container_gone(socket, name)
    command = gpu_container_prefix(socket, name, root, repo, image, detach=True)
    command += [
        "/repo/benchmarks/gemma4_baseline/serve_cross_encoder.py",
        "--model",
        f"/bench/{model_dir.relative_to(root)}",
        "--port",
        str(serving["port"]),
        "--batch-size",
        str(serving["batch_size"]),
        "--maximum-wait-ms",
        str(serving["maximum_batch_wait_ms"]),
        "--max-length",
        str(serving["max_length"]),
    ]
    result = run(command, capture=True)
    container_id = result.stdout.strip()
    try:
        health = wait_health(f"http://127.0.0.1:{serving['port']}/health/rerank")
    except Exception:
        logs = run(docker_cmd(socket, "logs", container_id), capture=True, check=False)
        raise RuntimeError(f"{label} server startup failed:\n{logs.stdout}{logs.stderr}")
    return container_id, health


def stop_server(socket: str, logs_path: Path) -> None:
    logs = run(docker_cmd(socket, "logs", "eurobert-reranker-server"), capture=True, check=False)
    logs_path.write_text(logs.stdout + logs.stderr, encoding="utf-8")
    run(docker_cmd(socket, "stop", "eurobert-reranker-server"), capture=True, check=False)
    wait_container_gone(socket, "eurobert-reranker-server")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("/mnt/media/gemma4-baseline"))
    parser.add_argument("--repo", type=Path, default=Path("/mnt/media/gemma4-baseline/repo"))
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--socket", default="/run/smoothnas-runtime/docker.sock")
    parser.add_argument("--production-container", default="aimee-llm-llm")
    parser.add_argument("--labels", help="Comma-separated EuroBERT labels")
    parser.add_argument("--max-cases", type=int, default=0)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-training", action="store_true")
    args = parser.parse_args()

    manifest_path = args.manifest or args.repo / "benchmarks/gemma4_baseline/eurobert_rerankers.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    by_label = {model["label"]: model for model in manifest["models"]}
    selected = args.labels.split(",") if args.labels else list(by_label)
    unknown = set(selected) - set(by_label)
    if unknown:
        parser.error(f"unknown labels: {', '.join(sorted(unknown))}")

    args.root.mkdir(parents=True, exist_ok=True)
    results = args.root / (f"results_smoke_{args.max_cases}" if args.max_cases else "results")
    logs = args.root / "logs"
    results.mkdir(exist_ok=True)
    logs.mkdir(exist_ok=True)
    lock_handle = (args.root / "sweep.lock").open("w", encoding="utf-8")
    fcntl.flock(lock_handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
    state_path = args.root / "eurobert_sweep_state.json"
    suite_sha = hashlib.sha256(
        (args.repo / "benchmarks/fixtures/gemma4-unified/ab-v1/manifest.json").read_bytes()
    ).hexdigest()
    extension_sha = manifest_sha256(manifest_path)
    models_root = args.root / "eurobert-rerankers" / extension_sha[:16]
    models_root.mkdir(parents=True, exist_ok=True)

    production_was_running = inspect_running(args.socket, args.production_container)
    write_state(
        state_path,
        status="starting",
        extension_manifest_sha256=extension_sha,
        suite_manifest_sha256=suite_sha,
        selected_labels=selected,
        production_was_running=production_was_running,
    )
    if production_was_running:
        run(docker_cmd(args.socket, "stop", args.production_container))

    try:
        if not args.skip_build:
            write_state(state_path, status="building_runtime")
            build_image(args.socket, args.repo, manifest)
        for label in selected:
            model_root = models_root / label
            final_dir = model_root / "final"
            smoke_path = model_root / "runtime_smoke.json"
            smoke_expected = expected_smoke_provenance(manifest_path, manifest, by_label[label])
            try:
                assert_completed_smoke(smoke_path, smoke_expected)
                smoke_complete = True
            except (OSError, RuntimeError, ValueError, json.JSONDecodeError):
                smoke_complete = False
            if not smoke_complete:
                write_state(state_path, status="runtime_smoke", active_label=label)
                smoke_model(
                    args.socket,
                    args.root,
                    args.repo,
                    manifest["runtime"]["image_tag"],
                    manifest_path,
                    label,
                    smoke_path,
                )
            assert_completed_smoke(smoke_path, smoke_expected)
            expected = expected_provenance(manifest_path, manifest, by_label[label])
            try:
                assert_completed_training_dir(model_root, expected)
                training_complete = True
            except (OSError, RuntimeError, ValueError, json.JSONDecodeError):
                training_complete = False
            if not training_complete:
                if args.skip_training:
                    raise RuntimeError(f"{label}: no verified complete model exists at {final_dir}")
                write_state(state_path, status="training", active_label=label)
                train_model(
                    args.socket,
                    args.root,
                    args.repo,
                    manifest["runtime"]["image_tag"],
                    manifest_path,
                    label,
                    model_root,
                )
            assert_completed_training_dir(model_root, expected)
            write_state(state_path, status="reranking", active_label=label)
            _, health = start_server(
                args.socket,
                args.root,
                args.repo,
                manifest["runtime"]["image_tag"],
                label,
                final_dir,
                manifest["serving"],
            )
            try:
                command = [
                    "python3",
                    str(args.repo / "benchmarks/gemma4_baseline/run_reranking_ab.py"),
                    "--endpoint",
                    f"http://127.0.0.1:{manifest['serving']['port']}",
                    "--label",
                    label,
                    "--bundle",
                    str(args.repo / "benchmarks/fixtures/gemma4-unified/ab-v1"),
                    "--output-dir",
                    str(results / label),
                    "--workers",
                    str(manifest["serving"]["workers"]),
                    "--pair-batch-size",
                    str(manifest["serving"]["pairs_per_request"]),
                    "--query-char-cap",
                    str(manifest["serving"]["query_char_cap"]),
                    "--candidate-char-cap",
                    str(manifest["serving"]["candidate_char_cap"]),
                    "--environment-note",
                    json.dumps({"extension_manifest_sha256": extension_sha, "health": health}, sort_keys=True),
                ]
                if args.max_cases:
                    command += ["--max-cases", str(args.max_cases)]
                run(command)
            finally:
                stop_server(args.socket, logs / f"server_{label}.log")
        write_state(state_path, status="complete", active_label=None)
    except Exception as exc:
        write_state(state_path, status="failed", error=f"{type(exc).__name__}: {exc}")
        raise
    finally:
        run(docker_cmd(args.socket, "stop", "eurobert-reranker-server"), capture=True, check=False)
        if production_was_running and not inspect_running(args.socket, args.production_container):
            run(docker_cmd(args.socket, "start", args.production_container))
        write_state(
            state_path,
            production_restored=production_was_running and inspect_running(args.socket, args.production_container),
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
