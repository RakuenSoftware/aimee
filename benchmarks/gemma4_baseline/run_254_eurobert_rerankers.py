#!/usr/bin/env python3
"""Train and benchmark the pinned EuroBERT reranker extension on `.254`."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import itertools
import json
import subprocess
import time
import urllib.request
from pathlib import Path
from typing import Any

from smoke_eurobert_runtime import assert_completed_smoke, expected_smoke_provenance
from train_eurobert_reranker import assert_completed_training_dir, expected_provenance
from validate_result_checkpoint import validate_and_write_checkpoint


def run(command: list[str], *, capture: bool = False, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=capture, check=check)


def docker_cmd(socket: str, *parts: str) -> list[str]:
    return ["docker", "-H", f"unix://{socket}", *parts]


def sweep_lock_flags(wait_for_lock: bool) -> int:
    return fcntl.LOCK_EX if wait_for_lock else fcntl.LOCK_EX | fcntl.LOCK_NB


def assert_restored_handoff(path: Path) -> dict[str, Any]:
    if not path.exists():
        raise RuntimeError(f"handoff state is missing: {path}")
    state = json.loads(path.read_text(encoding="utf-8"))
    if state.get("status") != "complete" or state.get("production_restored") is not True:
        raise RuntimeError(f"prior sweep did not complete with production restored: {path}")
    return state


def manifest_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pairwise_report_command(
    root: Path,
    repo: Path,
    manifest_path: Path,
    main_state: Path,
    eurobert_state: Path,
) -> list[str]:
    return [
        "python3",
        str(repo / "benchmarks/gemma4_baseline/reranker_pairwise_reports.py"),
        "--results",
        str(root / "results"),
        "--manifest",
        str(manifest_path),
        "--main-state",
        str(main_state),
        "--eurobert-state",
        str(eurobert_state),
    ]


def evaluation_labels(manifest: dict[str, Any]) -> list[str]:
    return [label for model in manifest["models"] for label in (model["pretrained_label"], model["label"])]


def assert_completed_pairwise_index(path: Path, labels: list[str]) -> dict[str, Any]:
    index = json.loads(path.read_text(encoding="utf-8"))
    expected_pairs = list(itertools.combinations(["ettin68m", "ettin400m", *labels], 2))
    reports = index.get("reports", [])
    actual_pairs = [(report.get("left"), report.get("right")) for report in reports]
    if index.get("pair_count") != len(expected_pairs) or actual_pairs != expected_pairs:
        raise RuntimeError("pairwise report index does not contain the exact expected model pairs")
    for report in reports:
        output = path.parent / report["file"]
        if not output.is_file() or manifest_sha256(output) != report.get("sha256"):
            raise RuntimeError(f"pairwise report artifact verification failed: {output}")
    return index


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


def remove_container(socket: str, name: str) -> None:
    run(docker_cmd(socket, "rm", "--force", name), capture=True, check=False)
    wait_container_gone(socket, name)


def wait_health(url: str, timeout: int = 900, accepted_status: str = "ready") -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last_error = ""
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=5) as response:
                payload = json.load(response)
            if payload.get("status") == accepted_status:
                return payload
            last_error = f"unexpected status: {payload.get('status')!r}"
        except Exception as exc:  # noqa: BLE001 - retained for startup diagnosis
            last_error = f"{type(exc).__name__}: {exc}"
        time.sleep(2)
    raise RuntimeError(f"server did not become healthy: {last_error}")


def write_state(path: Path, **values: Any) -> None:
    current = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    current.update(values)
    path.write_text(json.dumps(current, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def restore_production(
    socket: str,
    container: str,
    health_url: str,
    state_path: Path,
) -> dict[str, Any]:
    if not inspect_running(socket, container):
        run(docker_cmd(socket, "start", container))
    health = wait_health(health_url, timeout=900, accepted_status="ok")
    write_state(state_path, production_restored=True, production_health=health)
    return health


def environment_identity(suite_sha: str, runtime: dict[str, Any]) -> dict[str, Any]:
    hardware = {
        "gpu_vendor": Path("/sys/class/drm/card0/device/vendor").read_text(encoding="utf-8").strip(),
        "gpu_device": Path("/sys/class/drm/card0/device/device").read_text(encoding="utf-8").strip(),
        "gpu_vram_total_bytes": int(
            Path("/sys/class/drm/card0/device/mem_info_vram_total").read_text(encoding="utf-8").strip()
        ),
    }
    return {
        "fixtures_manifest_sha256": suite_sha,
        "host": run(["hostname"], capture=True).stdout.strip(),
        "kernel": run(["uname", "-srmo"], capture=True).stdout.strip(),
        "hardware_identity": hardware,
        "container_image": runtime["image_tag"],
        "base_image": runtime["base_image"],
        "packages": runtime["packages"],
    }


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
        "--restart",
        "unless-stopped" if detach else "on-failure:1",
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
    name = f"eurobert-train-{label}"
    remove_container(socket, name)
    command = gpu_container_prefix(socket, name, root, repo, image)
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
    try:
        run(command)
    finally:
        remove_container(socket, name)


def smoke_model(
    socket: str,
    root: Path,
    repo: Path,
    image: str,
    manifest_path: Path,
    label: str,
    output_path: Path,
) -> None:
    name = f"eurobert-smoke-{label}"
    remove_container(socket, name)
    command = gpu_container_prefix(socket, name, root, repo, image)
    command += [
        "/repo/benchmarks/gemma4_baseline/smoke_eurobert_runtime.py",
        "--manifest",
        f"/repo/{manifest_path.relative_to(repo)}",
        "--label",
        label,
        "--output",
        f"/bench/{output_path.relative_to(root)}",
    ]
    try:
        run(command)
    finally:
        remove_container(socket, name)


def start_server(
    socket: str,
    root: Path,
    repo: Path,
    image: str,
    label: str,
    model: dict[str, Any],
    model_dir: Path | None,
    serving: dict[str, Any],
) -> tuple[str, dict[str, Any], float]:
    started = time.monotonic()
    name = "eurobert-reranker-server"
    remove_container(socket, name)
    command = gpu_container_prefix(socket, name, root, repo, image, detach=True)
    if model_dir is None:
        command += [
            "/repo/benchmarks/gemma4_baseline/serve_eurobert_biencoder.py",
            "--model",
            model["repository"],
            "--revision",
            model["revision"],
            "--expected-model-type",
            model["expected_model_type"],
            "--expected-hidden-size",
            str(model["expected_hidden_size"]),
            "--expected-layers",
            str(model["expected_layers"]),
        ]
    else:
        command += [
            "/repo/benchmarks/gemma4_baseline/serve_cross_encoder.py",
            "--model",
            f"/bench/{model_dir.relative_to(root)}",
        ]
    command += [
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
    return container_id, health, time.monotonic() - started


def stop_server(socket: str, logs_path: Path) -> None:
    logs = run(docker_cmd(socket, "logs", "eurobert-reranker-server"), capture=True, check=False)
    logs_path.write_text(logs.stdout + logs.stderr, encoding="utf-8")
    remove_container(socket, "eurobert-reranker-server")


def benchmark_model(
    socket: str,
    root: Path,
    repo: Path,
    image: str,
    manifest: dict[str, Any],
    model: dict[str, Any],
    label: str,
    model_dir: Path | None,
    results: Path,
    logs: Path,
    extension_sha: str,
    max_cases: int,
) -> None:
    _, health, load_seconds = start_server(
        socket,
        root,
        repo,
        image,
        label,
        model,
        model_dir,
        manifest["serving"],
    )
    after_load = hardware_snapshot()
    try:
        command = [
            "python3",
            str(repo / "benchmarks/gemma4_baseline/run_reranking_ab.py"),
            "--endpoint",
            f"http://127.0.0.1:{manifest['serving']['port']}",
            "--label",
            label,
            "--bundle",
            str(repo / "benchmarks/fixtures/gemma4-unified/ab-v1"),
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
        if max_cases:
            command += ["--max-cases", str(max_cases)]
        run(command)
        after_run = hardware_snapshot()
        label_results = results / label
        label_results.mkdir(exist_ok=True)
        (label_results / "hardware_reranking.json").write_text(
            json.dumps(
                {
                    "cold_load_seconds": load_seconds,
                    "load_profile": manifest["serving"],
                    "health": health,
                    "after_load": after_load,
                    "after_run": after_run,
                },
                indent=2,
                sort_keys=True,
            ) + "\n",
            encoding="utf-8",
        )
    finally:
        stop_server(socket, logs / f"server_{label}.log")
    if not max_cases:
        validate_and_write_checkpoint(
            repo / "benchmarks/fixtures/gemma4-unified/ab-v1",
            results / label,
            label,
            "reranking",
            results / label / "validation_reranking.json",
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("/mnt/media/gemma4-baseline"))
    parser.add_argument("--repo", type=Path, default=Path("/mnt/media/gemma4-baseline/repo"))
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--socket", default="/run/smoothnas-runtime/docker.sock")
    parser.add_argument("--production-container", default="aimee-llm-llm")
    parser.add_argument("--production-health-url", default="http://192.168.1.254:8742/health")
    parser.add_argument("--labels", help="Comma-separated EuroBERT labels")
    parser.add_argument("--max-cases", type=int, default=0)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-training", action="store_true")
    parser.add_argument(
        "--wait-for-lock",
        action="store_true",
        help="Wait for another benchmark to restore production and release the shared sweep lock",
    )
    parser.add_argument(
        "--handoff-state",
        type=Path,
        help="Prior main RUN_STATE.json required to be complete/restored after a blocking lock wait",
    )
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
    fcntl.flock(lock_handle, sweep_lock_flags(args.wait_for_lock))
    if args.wait_for_lock:
        assert_restored_handoff(args.handoff_state or results / "RUN_STATE.json")
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
        environment=environment_identity(suite_sha, manifest["runtime"]),
    )
    if production_was_running:
        run(docker_cmd(args.socket, "stop", args.production_container))

    try:
        if not args.skip_build:
            write_state(state_path, status="building_runtime")
            build_image(args.socket, args.repo, manifest)
        for label in selected:
            model = by_label[label]
            model_root = models_root / label
            final_dir = model_root / "final"
            smoke_path = model_root / "runtime_smoke.json"
            pretrained_label = model["pretrained_label"]
            write_state(state_path, status="pretrained_reranking", active_label=pretrained_label)
            benchmark_model(
                args.socket,
                args.root,
                args.repo,
                manifest["runtime"]["image_tag"],
                manifest,
                model,
                pretrained_label,
                None,
                results,
                logs,
                extension_sha,
                args.max_cases,
            )
            smoke_expected = expected_smoke_provenance(manifest_path, manifest, model)
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
            expected = expected_provenance(manifest_path, manifest, model)
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
            write_state(state_path, status="trained_reranking", active_label=label)
            benchmark_model(
                args.socket,
                args.root,
                args.repo,
                manifest["runtime"]["image_tag"],
                manifest,
                model,
                label,
                final_dir,
                results,
                logs,
                extension_sha,
                args.max_cases,
            )
        write_state(state_path, status="complete", active_label=None)
    except Exception as exc:
        write_state(state_path, status="failed", error=f"{type(exc).__name__}: {exc}")
        raise
    finally:
        remove_container(args.socket, "eurobert-reranker-server")
        if production_was_running:
            try:
                restore_production(
                    args.socket,
                    args.production_container,
                    args.production_health_url,
                    state_path,
                )
            except Exception as exc:  # noqa: BLE001
                write_state(
                    state_path,
                    production_restored=False,
                    production_restore_error=f"{type(exc).__name__}: {exc}",
                )
                raise RuntimeError("production service restoration failed") from exc
    if not args.max_cases and set(selected) == set(by_label):
        try:
            write_state(state_path, pairwise_status="running")
            run(
                pairwise_report_command(
                    args.root,
                    args.repo,
                    manifest_path,
                    args.handoff_state or results / "RUN_STATE.json",
                    state_path,
                )
            )
            pairwise_index_path = results / "reranker_pairwise/INDEX.json"
            pairwise_index = assert_completed_pairwise_index(pairwise_index_path, evaluation_labels(manifest))
            write_state(
                state_path,
                pairwise_status="complete",
                pairwise_index={
                    "file": str(pairwise_index_path),
                    "sha256": manifest_sha256(pairwise_index_path),
                    "pair_count": pairwise_index["pair_count"],
                },
            )
        except Exception as exc:
            write_state(
                state_path,
                status="failed",
                pairwise_status="failed",
                error=f"{type(exc).__name__}: {exc}",
            )
            raise
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
