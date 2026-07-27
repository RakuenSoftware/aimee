#!/usr/bin/env python3
"""Fail-closed validation for one completed benchmark result checkpoint."""

from __future__ import annotations

import argparse
import json
import math
import statistics
from pathlib import Path
from typing import Any

from build_254_fixtures import assert_no_obvious_secrets
from run_reranking_ab import percentile, ranking_metrics
from run_synthesis_ab import summarize as summarize_synthesis
from validate_fixtures import file_sha256, load_jsonl


RANK_METRICS = ("recall_at_1", "recall_at_5", "recall_at_10", "mrr_at_10", "ndcg_at_10")


def load_object(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise RuntimeError(f"expected JSON object in {path}")
    return value


def latest_by_case(path: Path) -> tuple[list[dict[str, Any]], dict[str, dict[str, Any]]]:
    raw = load_jsonl(path)
    latest: dict[str, dict[str, Any]] = {}
    for row in raw:
        case_id = str(row.get("case_id", ""))
        if not case_id:
            raise RuntimeError(f"result row without case_id in {path}")
        latest[case_id] = row
    return raw, latest


def assert_population(latest: dict[str, dict[str, Any]], cases: list[dict[str, Any]], label: str) -> None:
    expected = {str(case["case_id"]) for case in cases}
    actual = set(latest)
    if actual != expected:
        raise RuntimeError(f"{label}: case population differs: missing={len(expected-actual)}, extra={len(actual-expected)}")


def assert_close(actual: float, expected: float, context: str) -> None:
    if not math.isclose(actual, expected, rel_tol=0.0, abs_tol=1e-15):
        raise RuntimeError(f"{context}: expected {expected}, received {actual}")


def validate_synthesis(
    bundle: Path,
    result_dir: Path,
    label: str,
    suite_sha: str,
) -> tuple[Path, Path, Path, dict[str, Any]]:
    raw_path = result_dir / f"raw_{label}.jsonl"
    summary_path = result_dir / f"summary_{label}.json"
    hardware_path = result_dir / "hardware_synthesis.json"
    cases = load_jsonl(bundle / "synthesis.jsonl")
    raw, latest = latest_by_case(raw_path)
    assert_population(latest, cases, label)
    failed = [case_id for case_id, row in latest.items() if not row.get("ok")]
    if failed:
        raise RuntimeError(f"{label}: {len(failed)} latest synthesis rows are unsuccessful")
    rows = [latest[str(case["case_id"])] for case in cases]
    summary = load_object(summary_path)
    recomputed = summarize_synthesis(rows, label, str(summary.get("model", label)), suite_sha)
    if recomputed != summary:
        raise RuntimeError(f"{label}: synthesis summary does not reproduce exactly")
    return raw_path, summary_path, hardware_path, {
        "raw_rows": len(raw),
        "latest_rows": len(latest),
        "metrics": summary["overall"],
    }


def validate_embedding(
    bundle: Path,
    result_dir: Path,
    label: str,
    suite_sha: str,
) -> tuple[Path, Path, Path, dict[str, Any]]:
    raw_path = result_dir / f"raw_embedding_{label}.jsonl"
    summary_path = result_dir / f"summary_embedding_{label}.json"
    hardware_path = result_dir / "hardware_embedding.json"
    cases = load_jsonl(bundle / "embedding.jsonl")
    reranking_cases = load_jsonl(bundle / "reranking.jsonl")
    raw, latest = latest_by_case(raw_path)
    if len(raw) != len(latest):
        raise RuntimeError(f"{label}: embedding raw file has duplicate case rows")
    assert_population(latest, cases, label)
    summary = load_object(summary_path)
    if summary.get("suite_manifest_sha256") != suite_sha:
        raise RuntimeError(f"{label}: embedding suite hash mismatch")
    if summary.get("cases") != len(cases):
        raise RuntimeError(f"{label}: embedding case count mismatch")
    candidates = {doc_id for case in reranking_cases for doc_id in case["candidate_doc_ids"]}
    if summary.get("candidate_documents") != len(candidates):
        raise RuntimeError(f"{label}: candidate-document count mismatch")
    groups = set(summary.get("dimensions", {}))
    if not groups or any(set(row.get("metrics", {})) != groups for row in raw):
        raise RuntimeError(f"{label}: embedding metric groups differ")
    for group in sorted(groups):
        expected_group = summary["dimensions"][group]
        for metric in RANK_METRICS:
            actual = statistics.fmean(float(row["metrics"][group][metric]) for row in raw)
            assert_close(actual, float(expected_group[metric]), f"{label}:{group}:{metric}")
    native = summary["dimensions"].get("native")
    if not native or native.get("dimensions") != summary.get("native_dimensions"):
        raise RuntimeError(f"{label}: native embedding width mismatch")
    telemetry = summary.get("telemetry", {})
    if float(telemetry.get("vectors_per_second", 0)) <= 0:
        raise RuntimeError(f"{label}: embedding throughput is missing")
    return raw_path, summary_path, hardware_path, {
        "raw_rows": len(raw),
        "latest_rows": len(latest),
        "metrics": summary["dimensions"],
        "vectors_per_second": telemetry["vectors_per_second"],
    }


def validate_reranking(
    bundle: Path,
    result_dir: Path,
    label: str,
    suite_sha: str,
) -> tuple[Path, Path, Path, dict[str, Any]]:
    raw_path = result_dir / f"raw_reranking_{label}.jsonl"
    summary_path = result_dir / f"summary_reranking_{label}.json"
    hardware_path = result_dir / "hardware_reranking.json"
    cases = load_jsonl(bundle / "reranking.jsonl")
    raw, latest = latest_by_case(raw_path)
    assert_population(latest, cases, label)
    failed = [case_id for case_id, row in latest.items() if not row.get("ok")]
    if failed:
        raise RuntimeError(f"{label}: {len(failed)} latest reranking rows are unsuccessful")
    case_by_id = {str(case["case_id"]): case for case in cases}
    rows = []
    for case_id in sorted(latest):
        row = latest[case_id]
        case = case_by_id[case_id]
        candidates = list(case["candidate_doc_ids"])
        if len(row.get("scores", [])) != len(candidates) or set(row.get("ranked_doc_ids", [])) != set(candidates):
            raise RuntimeError(f"{label}:{case_id}: reranking output is not aligned to candidates")
        expected_metrics = ranking_metrics(row["ranked_doc_ids"], case["relevance"])
        if row.get("metrics") != expected_metrics:
            raise RuntimeError(f"{label}:{case_id}: reranking metrics do not reproduce")
        rows.append(row)
    summary = load_object(summary_path)
    if summary.get("suite_manifest_sha256") != suite_sha:
        raise RuntimeError(f"{label}: reranking suite hash mismatch")
    if summary.get("cases") != len(cases) or summary.get("candidates_per_case") != 20:
        raise RuntimeError(f"{label}: reranking shape mismatch")
    if summary.get("success_rate") != 1.0:
        raise RuntimeError(f"{label}: reranking summary is not fully successful")
    for metric in RANK_METRICS:
        actual = statistics.fmean(float(row["metrics"][metric]) for row in rows)
        assert_close(actual, float(summary[metric]), f"{label}:{metric}")
    latencies = [float(row["latency_s"]) for row in rows]
    for name, quantile in (("p50", 0.50), ("p95", 0.95), ("p99", 0.99)):
        assert_close(percentile(latencies, quantile), float(summary["latency_s"][name]), f"{label}:latency:{name}")
    return raw_path, summary_path, hardware_path, {
        "raw_rows": len(raw),
        "latest_rows": len(latest),
        "metrics": {metric: summary[metric] for metric in RANK_METRICS},
    }


def validate_checkpoint(bundle: Path, result_dir: Path, label: str, view: str) -> dict[str, Any]:
    suite_sha = file_sha256(bundle / "manifest.json")
    validators = {
        "synthesis": validate_synthesis,
        "embedding": validate_embedding,
        "reranking": validate_reranking,
    }
    raw_path, summary_path, hardware_path, details = validators[view](bundle, result_dir, label, suite_sha)
    for path in (raw_path, summary_path, hardware_path):
        if not path.is_file():
            raise RuntimeError(f"required checkpoint artifact is missing: {path}")
    assert_no_obvious_secrets((raw_path, summary_path, hardware_path))
    return {
        "label": label,
        "view": view,
        "suite_manifest_sha256": suite_sha,
        "secret_scan": "pass",
        "artifacts": {
            path.name: {"bytes": path.stat().st_size, "sha256": file_sha256(path)}
            for path in (raw_path, summary_path, hardware_path)
        },
        **details,
    }


def write_json_atomic(path: Path, payload: dict[str, Any]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def validate_and_write_checkpoint(
    bundle: Path,
    result_dir: Path,
    label: str,
    view: str,
    output: Path,
) -> dict[str, Any]:
    evidence = validate_checkpoint(bundle, result_dir, label, view)
    write_json_atomic(output, evidence)
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--result-dir", type=Path, required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--view", choices=("synthesis", "embedding", "reranking"), required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    evidence = validate_checkpoint(args.bundle, args.result_dir, args.label, args.view)
    serialized = json.dumps(evidence, indent=2, sort_keys=True) + "\n"
    if args.output:
        write_json_atomic(args.output, evidence)
    print(serialized, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
