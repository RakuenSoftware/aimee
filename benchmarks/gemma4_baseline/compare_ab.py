#!/usr/bin/env python3
"""Create deterministic paired A:B deltas and bootstrap confidence intervals."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Callable


def load(path: Path) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            if line.strip():
                row = json.loads(line)
                result[row["case_id"]] = row
    failed = [case_id for case_id, row in result.items() if not row.get("ok", True)]
    if failed:
        raise RuntimeError(f"{len(failed)} latest case rows are unsuccessful in {path}")
    return result


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise RuntimeError(f"expected JSON object in {path}")
    return value


def interval(differences: Any, seed: int, replicates: int) -> tuple[float, float]:
    import numpy as np

    rng = np.random.default_rng(seed)
    means = np.empty(replicates, dtype=np.float64)
    batch = 100
    for offset in range(0, replicates, batch):
        count = min(batch, replicates - offset)
        indices = rng.integers(0, len(differences), size=(count, len(differences)))
        means[offset : offset + count] = differences[indices].mean(axis=1)
    low, high = np.percentile(means, [2.5, 97.5])
    return float(low), float(high)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kind", required=True, choices=("synthesis", "embedding", "reranking"))
    parser.add_argument("--left", type=Path, required=True)
    parser.add_argument("--right", type=Path, required=True)
    parser.add_argument("--left-label", required=True)
    parser.add_argument("--right-label", required=True)
    parser.add_argument("--left-summary", type=Path, required=True)
    parser.add_argument("--right-summary", type=Path, required=True)
    parser.add_argument("--left-environment", type=Path, required=True, help="RUN_STATE.json from the left sweep")
    parser.add_argument("--right-environment", type=Path, required=True, help="RUN_STATE.json from the right sweep")
    parser.add_argument("--dimension", default="native", help="Embedding metric group")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--replicates", type=int, default=10000)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    import numpy as np

    left, right = load(args.left), load(args.right)
    left_summary, right_summary = load_json(args.left_summary), load_json(args.right_summary)
    left_state, right_state = load_json(args.left_environment), load_json(args.right_environment)
    left_env, right_env = left_state.get("environment", {}), right_state.get("environment", {})
    suite_hash = left_summary.get("suite_manifest_sha256")
    if not suite_hash or right_summary.get("suite_manifest_sha256") != suite_hash:
        raise RuntimeError("summary fixture-manifest hashes differ")
    if left_env.get("fixtures_manifest_sha256") != suite_hash or right_env.get("fixtures_manifest_sha256") != suite_hash:
        raise RuntimeError("run environment does not match the result fixture-manifest hash")
    common_identity_keys = ("host", "kernel", "hardware_identity")
    runtime_identity_keys = ("container_image", "llama_cpp_build", "base_image", "packages")
    identity_keys = common_identity_keys if args.kind == "reranking" else (
        *common_identity_keys,
        "container_image",
        "llama_cpp_build",
    )
    identity_mismatches = [key for key in identity_keys if left_env.get(key) != right_env.get(key)]
    if identity_mismatches:
        raise RuntimeError(f"run environments differ: {identity_mismatches}")
    if set(left) != set(right):
        raise RuntimeError(f"case populations differ: left-only={len(set(left)-set(right))}, right-only={len(set(right)-set(left))}")
    case_ids = sorted(left)
    if args.kind == "reranking":
        for key in ("cases", "candidates_per_case", "load_profile", "input_bounds"):
            if left_summary.get(key) != right_summary.get(key):
                raise RuntimeError(f"reranking summaries differ in {key}")
        if left_summary.get("success_rate") != 1.0 or right_summary.get("success_rate") != 1.0:
            raise RuntimeError("reranking summaries are not fully successful")

    getters: dict[str, Callable[[dict[str, Any]], float]]
    if args.kind == "synthesis":
        getters = {
            "content_f1": lambda row: float(row["metrics"]["content_f1"]),
            "schema_valid_rate": lambda row: float(row["metrics"]["schema_valid"]),
            "required_field_recall": lambda row: float(row["metrics"]["required_field_recall"]),
            "raw_parse_rate": lambda row: float(row["raw_parse"]),
            "truncated_rate": lambda row: float(row["truncated"]),
            "latency_s": lambda row: float(row["latency_s"]),
        }
    else:
        group = (lambda row: row["metrics"][args.dimension]) if args.kind == "embedding" else (lambda row: row["metrics"])
        getters = {
            name: lambda row, name=name: float(group(row)[name])
            for name in ("recall_at_1", "recall_at_5", "recall_at_10", "mrr_at_10", "ndcg_at_10")
        }
        if args.kind == "reranking":
            getters["latency_s"] = lambda row: float(row["latency_s"])

    metrics = {}
    for metric_index, (name, getter) in enumerate(getters.items()):
        left_values = np.asarray([getter(left[case_id]) for case_id in case_ids], dtype=np.float64)
        right_values = np.asarray([getter(right[case_id]) for case_id in case_ids], dtype=np.float64)
        differences = right_values - left_values
        low, high = interval(differences, args.seed + metric_index, args.replicates)
        metrics[name] = {
            "left_mean": float(left_values.mean()),
            "right_mean": float(right_values.mean()),
            "right_minus_left": float(differences.mean()),
            "paired_bootstrap_95_ci": [low, high],
        }
    output = {
        "kind": args.kind,
        "left": args.left_label,
        "right": args.right_label,
        "cases": len(case_ids),
        "dimension": args.dimension if args.kind == "embedding" else None,
        "bootstrap": {"replicates": args.replicates, "seed": args.seed, "method": "paired_percentile"},
        "suite_manifest_sha256": suite_hash,
        "pinned_environment": {
            "common": {key: left_env.get(key) for key in common_identity_keys},
            "left_runtime": {key: left_env.get(key) for key in runtime_identity_keys},
            "right_runtime": {key: right_env.get(key) for key in runtime_identity_keys},
        } if args.kind == "reranking" else {key: left_env.get(key) for key in identity_keys},
        "metrics": metrics,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(output, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
