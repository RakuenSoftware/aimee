#!/usr/bin/env python3
"""Score the E6 retrieval and paired-agent promotion gates."""

from __future__ import annotations

import argparse
import json
import math
import statistics
from pathlib import Path


ARMS = ("standard", "observe", "on", "ceiling")
CORPUS_PATH = Path(__file__).with_name("e6-corpus.json")


def require_fields(row: dict, fields: tuple[str, ...], label: str) -> None:
    if not isinstance(row, dict) or any(field not in row for field in fields):
        raise ValueError(f"{label} is missing required fields")


def is_finite_nonnegative(value: object) -> bool:
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def wilson_lower(successes: int, total: int, z: float = 1.959963984540054) -> float:
    if total == 0:
        return 0.0
    p = successes / total
    denominator = 1 + z * z / total
    centre = p + z * z / (2 * total)
    spread = z * math.sqrt((p * (1 - p) + z * z / (4 * total)) / total)
    return (centre - spread) / denominator


def percentile(values: list[float], percentile_value: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    return ordered[math.ceil(percentile_value * len(ordered)) - 1]


def score(path: Path) -> dict:
    document = json.loads(path.read_text())
    pinned_commit = document.get("pinned_commit")
    retrieval_pinned_commit = document.get("retrieval_pinned_commit", pinned_commit)
    if retrieval_pinned_commit != pinned_commit:
        reuse = document.get("retrieval_reuse")
        if not isinstance(reuse, dict) or reuse.get("allowed") is not True or not reuse.get("rationale"):
            raise ValueError("commit-mismatched retrieval evidence requires explicit reuse rationale")
    corpus = json.loads(CORPUS_PATH.read_text())
    retrieval = document.get("retrieval_cells", [])
    coding = document.get("coding_cells", [])
    if not isinstance(retrieval, list) or not isinstance(coding, list):
        raise ValueError("result cell collections must be arrays")
    expected_retrieval = {row["id"]: row["expected"] for row in corpus["retrieval_cases"]}
    expected_tasks = {row["id"] for row in corpus["coding_tasks"]}
    retrieval_ids = []
    for row in retrieval:
        require_fields(row, ("id", "score_eligible", "expected", "observed", "duplicate", "scope_leak",
                             "retrieval_latency_s", "packet_tokens"), "retrieval row")
        if row["id"] not in expected_retrieval or row["expected"] != expected_retrieval[row["id"]]:
            raise ValueError("retrieval row has an unknown ID or mismatched expected label")
        if type(row["score_eligible"]) is not bool or type(row["duplicate"]) is not bool or type(row["scope_leak"]) is not bool:
            raise ValueError("retrieval booleans must be explicit")
        if row["observed"] not in ("answer", "abstain"):
            raise ValueError("retrieval observed label is invalid")
        if not is_finite_nonnegative(row["retrieval_latency_s"]) or not is_finite_nonnegative(row["packet_tokens"]):
            raise ValueError("retrieval timing and packet evidence must be finite and non-negative")
        retrieval_ids.append(row["id"])
    if len(retrieval_ids) != len(set(retrieval_ids)):
        raise ValueError("retrieval case IDs must be unique")
    coding_keys = []
    for row in coding:
        require_fields(row, ("arm", "task", "score_eligible"), "coding row")
        if row["arm"] not in ARMS or row["task"] not in expected_tasks or type(row["score_eligible"]) is not bool:
            raise ValueError("coding row has an unknown arm/task or invalid eligibility")
        if row["score_eligible"]:
            require_fields(row, ("task_success", "answerable", "consumed_before_edit",
                                 "uncached_input_tokens", "total_wall_s"), "eligible coding row")
            if any(type(row[field]) is not bool for field in ("task_success", "answerable", "consumed_before_edit")):
                raise ValueError("eligible coding decisions must be explicit booleans")
            if not is_finite_nonnegative(row["uncached_input_tokens"]) or not is_finite_nonnegative(row["total_wall_s"]):
                raise ValueError("eligible coding token and wall evidence must be finite and non-negative")
        coding_keys.append((row["arm"], row["task"]))
    if len(coding_keys) != len(set(coding_keys)):
        raise ValueError("coding arm/task pairs must be unique")
    for field in ("python_edge_precision", "python_edge_recall"):
        value = document.get(field)
        if type(value) not in (int, float) or not math.isfinite(value) or not 0 <= value <= 1:
            raise ValueError(f"{field} must be a finite rate in [0,1]")
    eligible_retrieval = [row for row in retrieval if row.get("score_eligible") is True]
    answerable = [row for row in eligible_retrieval if row["expected"] == "answer"]
    unanswerable = [row for row in eligible_retrieval if row["expected"] == "abstain"]
    predicted = [row for row in eligible_retrieval if row["observed"] == "answer"]
    true_positive = sum(row["expected"] == "answer" for row in predicted)
    retrieval_metrics = {
        "eligible": len(eligible_retrieval),
        "excluded": len(retrieval) - len(eligible_retrieval),
        "current_project_duplicate_rate": (sum(row["duplicate"] for row in eligible_retrieval) / len(eligible_retrieval)) if eligible_retrieval else None,
        "scope_leakage_rate": (sum(row["scope_leak"] for row in eligible_retrieval) / len(eligible_retrieval)) if eligible_retrieval else None,
        "answerable_precision": true_positive / len(predicted) if predicted else 0.0,
        "answerable_recall": sum(row["observed"] == "answer" for row in answerable) / len(answerable) if answerable else 0.0,
        "unanswerable_abstention": sum(row["observed"] == "abstain" for row in unanswerable) / len(unanswerable) if unanswerable else 0.0,
        "python_edge_precision": document.get("python_edge_precision"),
        "python_edge_recall": document.get("python_edge_recall"),
        "retrieval_latency_p95_s": percentile([float(row["retrieval_latency_s"]) for row in eligible_retrieval], .95),
        "packet_tokens_p95": percentile([float(row["packet_tokens"]) for row in eligible_retrieval], .95),
    }
    by_arm = {arm: [row for row in coding if row.get("arm") == arm and row.get("score_eligible") is True] for arm in ARMS}
    coding_metrics = {}
    for arm, rows in by_arm.items():
        successes = sum(row.get("task_success") is True for row in rows)
        coding_metrics[arm] = {
            "eligible": len(rows),
            "excluded": sum(row.get("arm") == arm for row in coding) - len(rows),
            "success_rate": successes / len(rows) if rows else None,
            "success_lcb95": wilson_lower(successes, len(rows)) if rows else None,
            "median_uncached_input_tokens": statistics.median([row["uncached_input_tokens"] for row in rows]) if rows else None,
            "median_total_wall_s": statistics.median([row["total_wall_s"] for row in rows]) if rows else None,
        }
    on_answerable = [row for row in by_arm["on"] if row.get("answerable") is True]
    actuation = sum(row.get("consumed_before_edit") is True for row in on_answerable) / len(on_answerable) if on_answerable else None
    coding_metrics["on"]["answerable_actuation_rate"] = actuation
    required_retrieval_complete = set(retrieval_ids) == set(expected_retrieval) and len(eligible_retrieval) == len(expected_retrieval)
    required_arms_complete = set(coding_keys) == {(arm, task) for arm in ARMS for task in expected_tasks} and all(
        len(by_arm[arm]) == len(expected_tasks) for arm in ARMS
    )
    retrieval_pass = required_retrieval_complete and all((
        retrieval_metrics["current_project_duplicate_rate"] == 0,
        retrieval_metrics["scope_leakage_rate"] == 0,
        retrieval_metrics["answerable_precision"] >= .90,
        retrieval_metrics["answerable_recall"] >= .80,
        retrieval_metrics["unanswerable_abstention"] >= .90,
        (retrieval_metrics["python_edge_precision"] or 0) >= .95,
        (retrieval_metrics["python_edge_recall"] or 0) >= .95,
        (retrieval_metrics["retrieval_latency_p95_s"] or math.inf) <= 2,
        (retrieval_metrics["packet_tokens_p95"] or math.inf) <= 1200,
    ))
    standard, on = coding_metrics["standard"], coding_metrics["on"]
    paired_pass = bool(required_arms_complete and actuation is not None and actuation >= .80 and
                       on["success_lcb95"] >= standard["success_lcb95"] - .02 and
                       ((on["median_uncached_input_tokens"] <= standard["median_uncached_input_tokens"] * .90) or
                        (on["median_total_wall_s"] <= standard["median_total_wall_s"] * .90)))
    return {
        "schema_version": 1,
        "pinned_commit": pinned_commit,
        "retrieval_pinned_commit": retrieval_pinned_commit,
        "prompt_fixture": document.get("prompt_fixture"),
        "retrieval": retrieval_metrics,
        "coding": coding_metrics,
        "required_arms_complete": required_arms_complete,
        "required_retrieval_complete": required_retrieval_complete,
        "retrieval_gate_pass": retrieval_pass,
        "paired_agent_gate_pass": paired_pass,
        "promotion_decision": "promote-on" if retrieval_pass and paired_pass else "retain-observe",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("results", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = score(args.results)
    encoded = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(encoded)
    print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
