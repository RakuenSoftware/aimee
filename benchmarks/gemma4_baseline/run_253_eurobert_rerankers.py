#!/usr/bin/env python3
"""Train and benchmark the pinned EuroBERT reranker extension on `.253`."""

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


def encoder_pairwise_report_command(
    root: Path,
    repo: Path,
    manifest_path: Path,
    main_state: Path,
    eurobert_state: Path,
) -> list[str]:
    return [
        "python3",
        str(repo / "benchmarks/gemma4_baseline/encoder_pairwise_reports.py"),
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


def encoder_evaluation_labels(manifest: dict[str, Any]) -> list[str]:
    return [
        label
        for model in manifest["models"]
        for label in (model["pretrained_encoder_label"], model["encoder_label"])
    ]


def assert_completed_pairwise_index(
    path: Path,
    labels: list[str],
    controls: tuple[str, ...] = ("ettin68m", "ettin400m"),
) -> dict[str, Any]:
    index = json.loads(path.read_text(encoding="utf-8"))
    expected_pairs = list(itertools.combinations([*controls, *labels], 2))
    reports = index.get("reports", [])
    actual_pairs = [(report.get("left"), report.get("right")) for report in reports]
    if index.get("pair_count") != len(expected_pairs) or actual_pairs != expected_pairs:
        raise RuntimeError("pairwise report index does not contain the exact expected model pairs")
    for report in reports:
        output = path.parent / report["file"]
        if not output.is_file() or manifest_sha256(output) != report.get("sha256"):
            raise RuntimeError(f"pairwise report artifact verification failed: {output}")
    return index


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


def environment_identity(suite_sha: str, runtime: dict[str, Any]) -> dict[str, Any]:
    query = run(
        [
            "nvidia-smi",
            "--query-gpu=name,pci.device_id,driver_version,memory.total,compute_cap",
            "--format=csv,noheader,nounits",
        ],
        capture=True,
    ).stdout.strip()
    name, device_id, driver, memory_mib, compute_capability = [part.strip() for part in query.split(",")]
    hardware = {
        "gpu_name": name,
        "gpu_device_id": device_id,
        "nvidia_driver": driver,
        "gpu_vram_total_bytes": int(memory_mib) * 1024 * 1024,
        "compute_capability": compute_capability,
    }
    return {
        "fixtures_manifest_sha256": suite_sha,
        "host": run(["hostname"], capture=True).stdout.strip(),
        "kernel": run(["uname", "-srmo"], capture=True).stdout.strip(),
        "hardware_identity": hardware,
        "runtime": runtime,
    }


def hardware_snapshot() -> dict[str, int]:
    query = run(
        [
            "nvidia-smi",
            "--query-gpu=memory.used,memory.total,utilization.gpu,temperature.gpu",
            "--format=csv,noheader,nounits",
        ],
        capture=True,
    ).stdout.strip()
    used_mib, total_mib, busy, temperature = [int(part.strip()) for part in query.split(",")]
    result = {
        "vram_used_bytes": used_mib * 1024 * 1024,
        "vram_total_bytes": total_mib * 1024 * 1024,
        "gpu_busy_percent": busy,
        "gpu_temperature_celsius": temperature,
    }
    for line in Path("/proc/meminfo").read_text(encoding="utf-8").splitlines():
        key, _, raw = line.partition(":")
        if key in {"MemAvailable", "SwapTotal", "SwapFree"}:
            result[f"{key.lower()}_bytes"] = int(raw.strip().split()[0]) * 1024
    return result


def runtime_venv_dir(root: Path, runtime: dict[str, Any]) -> Path:
    identity = hashlib.sha256(json.dumps(runtime, sort_keys=True).encode("utf-8")).hexdigest()[:16]
    return root / "eurobert-runtime" / identity / "venv"


def venv_python(venv_dir: Path) -> str:
    return str(venv_dir / "bin/python3")


def build_runtime(
    repo: Path,
    manifest_path: Path,
    venv_dir: Path,
) -> None:
    run([
        "python3",
        str(repo / "benchmarks/gemma4_baseline/bootstrap_eurobert_runtime.py"),
        "--manifest",
        str(manifest_path),
        "--venv-dir",
        str(venv_dir),
    ])


def train_model(
    repo: Path,
    manifest_path: Path,
    label: str,
    output_dir: Path,
    python_executable: str,
) -> None:
    run([
        python_executable,
        str(repo / "benchmarks/gemma4_baseline/train_eurobert_reranker.py"),
        "--manifest",
        str(manifest_path),
        "--label",
        label,
        "--output-dir",
        str(output_dir),
        "--resume",
    ])


def smoke_model(
    repo: Path,
    manifest_path: Path,
    label: str,
    output_path: Path,
    python_executable: str,
) -> None:
    run([
        python_executable,
        str(repo / "benchmarks/gemma4_baseline/smoke_eurobert_runtime.py"),
        "--manifest",
        str(manifest_path),
        "--label",
        label,
        "--output",
        str(output_path),
    ])


def start_server(
    repo: Path,
    label: str,
    model: dict[str, Any],
    model_dir: Path | None,
    server_mode: str,
    serving: dict[str, Any],
    python_executable: str,
    logs_path: Path,
) -> tuple[subprocess.Popen[str], dict[str, Any], float]:
    started = time.monotonic()
    command = [python_executable]
    if server_mode == "embedding" or model_dir is None:
        model_reference = str(model_dir) if model_dir is not None else model["repository"]
        command += [
            str(repo / "benchmarks/gemma4_baseline/serve_eurobert_biencoder.py"),
            "--model",
            model_reference,
            "--expected-model-type",
            model["expected_model_type"],
            "--expected-hidden-size",
            str(model["expected_hidden_size"]),
            "--expected-layers",
            str(model["expected_layers"]),
            "--model-state",
            "ettin_teacher_score_finetuned" if model_dir is not None else "official_pretrained_base",
        ]
        if model_dir is None:
            command += ["--revision", model["revision"]]
    else:
        command += [
            str(repo / "benchmarks/gemma4_baseline/serve_cross_encoder.py"),
            "--model",
            str(model_dir),
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
    with logs_path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, text=True, stdout=log, stderr=subprocess.STDOUT)
    try:
        health = wait_health(f"http://127.0.0.1:{serving['port']}/health/rerank")
    except Exception as exc:
        stop_server(process)
        detail = logs_path.read_text(encoding="utf-8") if logs_path.exists() else ""
        raise RuntimeError(f"{label} server startup failed:\n{detail}") from exc
    return process, health, time.monotonic() - started


def stop_server(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=30)


def benchmark_model(
    repo: Path,
    manifest: dict[str, Any],
    model: dict[str, Any],
    label: str,
    model_dir: Path | None,
    results: Path,
    logs: Path,
    extension_sha: str,
    max_cases: int,
    python_executable: str,
) -> None:
    process, health, load_seconds = start_server(
        repo,
        label,
        model,
        model_dir,
        "reranking",
        manifest["serving"],
        python_executable,
        logs / f"server_{label}.log",
    )
    after_load = hardware_snapshot()
    try:
        command = [
            python_executable,
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
        stop_server(process)
    if not max_cases:
        validate_and_write_checkpoint(
            repo / "benchmarks/fixtures/gemma4-unified/ab-v1",
            results / label,
            label,
            "reranking",
            results / label / "validation_reranking.json",
        )


def benchmark_encoder(
    repo: Path,
    manifest: dict[str, Any],
    model: dict[str, Any],
    label: str,
    model_dir: Path | None,
    results: Path,
    logs: Path,
    extension_sha: str,
    max_cases: int,
    python_executable: str,
) -> None:
    process, health, load_seconds = start_server(
        repo,
        label,
        model,
        model_dir,
        "embedding",
        manifest["serving"],
        python_executable,
        logs / f"server_{label}.log",
    )
    after_load = hardware_snapshot()
    try:
        command = [
            python_executable,
            str(repo / "benchmarks/gemma4_baseline/run_embedding_ab.py"),
            "--endpoint",
            f"http://127.0.0.1:{manifest['serving']['port']}",
            "--model",
            label,
            "--label",
            label,
            "--bundle",
            str(repo / "benchmarks/fixtures/gemma4-unified/ab-v1"),
            "--output-dir",
            str(results / label),
            "--batch-size",
            str(manifest["serving"]["batch_size"]),
        ]
        if max_cases:
            command += ["--max-cases", str(max_cases)]
        run(command)
        after_run = hardware_snapshot()
        label_results = results / label
        label_results.mkdir(exist_ok=True)
        (label_results / "hardware_embedding.json").write_text(
            json.dumps(
                {
                    "cold_load_seconds": load_seconds,
                    "load_profile": manifest["serving"],
                    "health": health,
                    "extension_manifest_sha256": extension_sha,
                    "after_load": after_load,
                    "after_run": after_run,
                },
                indent=2,
                sort_keys=True,
            ) + "\n",
            encoding="utf-8",
        )
    finally:
        stop_server(process)
    if not max_cases:
        validate_and_write_checkpoint(
            repo / "benchmarks/fixtures/gemma4-unified/ab-v1",
            results / label,
            label,
            "embedding",
            results / label / "validation_embedding.json",
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("/srv/eurobert"))
    parser.add_argument("--repo", type=Path, default=Path("/srv/aimee"))
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--labels", help="Comma-separated EuroBERT labels")
    parser.add_argument("--max-cases", type=int, default=0)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-training", action="store_true")
    parser.add_argument(
        "--wait-for-lock",
        action="store_true",
        help="Wait for another EuroBERT process to release the local sweep lock",
    )
    parser.add_argument(
        "--handoff-state",
        type=Path,
        help="Copied `.254` RUN_STATE.json proving the Ettin source results completed safely",
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
    main_state_path = args.handoff_state or results / "RUN_STATE.json"
    assert_restored_handoff(main_state_path)
    state_path = args.root / "eurobert_sweep_state.json"
    suite_sha = hashlib.sha256(
        (args.repo / "benchmarks/fixtures/gemma4-unified/ab-v1/manifest.json").read_bytes()
    ).hexdigest()
    extension_sha = manifest_sha256(manifest_path)
    models_root = args.root / "eurobert-rerankers" / extension_sha[:16]
    models_root.mkdir(parents=True, exist_ok=True)
    venv_dir = runtime_venv_dir(args.root, manifest["runtime"])
    python_executable = venv_python(venv_dir)

    write_state(
        state_path,
        status="starting",
        extension_manifest_sha256=extension_sha,
        suite_manifest_sha256=suite_sha,
        selected_labels=selected,
        target_host="192.168.1.253/VM109",
        production_impacted=False,
        error=None,
        pairwise_status=None,
        reranker_pairwise_index=None,
        encoder_pairwise_index=None,
        environment=environment_identity(suite_sha, manifest["runtime"]),
    )
    try:
        if not args.skip_build:
            write_state(state_path, status="bootstrapping_runtime")
            build_runtime(args.repo, manifest_path, venv_dir)
        runtime_provenance_path = venv_dir.parent / "runtime_provenance.json"
        runtime_provenance = json.loads(runtime_provenance_path.read_text(encoding="utf-8"))
        if runtime_provenance.get("status") != "complete":
            raise RuntimeError("EuroBERT CUDA runtime provenance is not complete")
        write_state(state_path, runtime_provenance=runtime_provenance)
        for label in selected:
            model = by_label[label]
            model_root = models_root / label
            final_dir = model_root / "final"
            smoke_path = model_root / "runtime_smoke.json"
            pretrained_label = model["pretrained_label"]
            pretrained_encoder_label = model["pretrained_encoder_label"]
            write_state(state_path, status="pretrained_embedding", active_label=pretrained_encoder_label)
            benchmark_encoder(
                args.repo,
                manifest,
                model,
                pretrained_encoder_label,
                None,
                results,
                logs,
                extension_sha,
                args.max_cases,
                python_executable,
            )
            write_state(state_path, status="pretrained_reranking", active_label=pretrained_label)
            benchmark_model(
                args.repo,
                manifest,
                model,
                pretrained_label,
                None,
                results,
                logs,
                extension_sha,
                args.max_cases,
                python_executable,
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
                    args.repo,
                    manifest_path,
                    label,
                    smoke_path,
                    python_executable,
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
                    args.repo,
                    manifest_path,
                    label,
                    model_root,
                    python_executable,
                )
            assert_completed_training_dir(model_root, expected)
            encoder_label = model["encoder_label"]
            write_state(state_path, status="trained_embedding", active_label=encoder_label)
            benchmark_encoder(
                args.repo,
                manifest,
                model,
                encoder_label,
                final_dir,
                results,
                logs,
                extension_sha,
                args.max_cases,
                python_executable,
            )
            write_state(state_path, status="trained_reranking", active_label=label)
            benchmark_model(
                args.repo,
                manifest,
                model,
                label,
                final_dir,
                results,
                logs,
                extension_sha,
                args.max_cases,
                python_executable,
            )
        write_state(state_path, status="complete", active_label=None)
    except Exception as exc:
        write_state(state_path, status="failed", error=f"{type(exc).__name__}: {exc}")
        raise
    if not args.max_cases and set(selected) == set(by_label):
        try:
            write_state(state_path, pairwise_status="running")
            run(
                pairwise_report_command(
                    args.root,
                    args.repo,
                    manifest_path,
                    main_state_path,
                    state_path,
                )
            )
            pairwise_index_path = results / "reranker_pairwise/INDEX.json"
            pairwise_index = assert_completed_pairwise_index(pairwise_index_path, evaluation_labels(manifest))
            encoder_labels = encoder_evaluation_labels(manifest)
            run(
                encoder_pairwise_report_command(
                    args.root,
                    args.repo,
                    manifest_path,
                    main_state_path,
                    state_path,
                )
            )
            encoder_index_path = results / "encoder_pairwise/INDEX.json"
            encoder_index = assert_completed_pairwise_index(
                encoder_index_path,
                encoder_labels,
                controls=("gemma4_e2b", "gemma4_e4b"),
            )
            write_state(
                state_path,
                pairwise_status="complete",
                reranker_pairwise_index={
                    "file": str(pairwise_index_path),
                    "sha256": manifest_sha256(pairwise_index_path),
                    "pair_count": pairwise_index["pair_count"],
                },
                encoder_pairwise_index={
                    "file": str(encoder_index_path),
                    "sha256": manifest_sha256(encoder_index_path),
                    "pair_count": encoder_index["pair_count"],
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
