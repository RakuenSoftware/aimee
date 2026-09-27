#!/usr/bin/env python3
"""Shared output helpers for benchmark scripts."""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

from benchmarks.common.harness import (
    category_accuracy,
    print_breakdown,
    summarize_costs,
    summarize_latencies,
)
from benchmarks.common.result_schema import validate_coverage

# ---------------------------------------------------------------------------
# Abstention detection
# ---------------------------------------------------------------------------

_ABSTENTION_PATTERNS = re.compile(
    r"\b(i\s+don'?t\s+know|i\s+(don'?t|do\s+not)\s+have|"
    r"i'?m\s+(not\s+sure|unable\s+to)|"
    r"no\s+information|not\s+available|cannot\s+(find|determine|confirm)|"
    r"can'?t\s+(find|determine|confirm)|"
    r"the\s+provided\s+(context|information)\s+(does\s+not|doesn'?t)|"
    r"there\s+is\s+no\s+(information|mention|record|data)|"
    r"not\s+mentioned|not\s+in\s+the|no\s+record\s+of)",
    re.IGNORECASE,
)


def is_abstention(answer: str) -> bool:
    """Return True if *answer* is a refusal / abstention rather than a factual claim."""
    if not answer:
        return True
    text = answer.strip()
    # Short, vague non-answers
    if len(text) < 20 and re.search(r"\b(unknown|n/?a|none|unclear)\b", text, re.IGNORECASE):
        return True
    return bool(_ABSTENTION_PATTERNS.search(text))


# ---------------------------------------------------------------------------
# Derived LongMemEval metrics
# ---------------------------------------------------------------------------

def compute_longmemeval_derived(results: list[dict[str, Any]]) -> dict[str, Any]:
    """Mixed-answerability metrics; missing labels never become negative labels.

    Abstention detection is a declared text heuristic, not a citation-support judge.
    Undefined ratios are JSON null. Failures remain in the overall denominator.
    """
    counts = {"answerable_answered": 0, "answerable_abstained": 0,
              "unanswerable_answered": 0, "unanswerable_abstained": 0}
    labelled = correct = answered_wrong = unknown = failed = 0
    for row in results:
        label = row.get("answerable")
        if type(label) is not bool:
            unknown += 1
            continue
        if row.get("run_status", "ok") != "ok":
            failed += 1
            continue
        labelled += 1
        abstained = row.get("abstained")
        if type(abstained) is not bool:
            abstained = is_abstention(str(row.get("generated_answer") or ""))
        counts[("answerable" if label else "unanswerable") +
               ("_abstained" if abstained else "_answered")] += 1
        correct += int(label and not abstained and row.get("verdict") == "CORRECT")
        answered_wrong += int(not abstained and row.get("verdict") != "CORRECT")
    aa, az = counts["answerable_answered"], counts["answerable_abstained"]
    ua, uz = counts["unanswerable_answered"], counts["unanswerable_abstained"]
    def ratio(a: int, b: int) -> float | None:
        return round(a / b, 6) if b else None
    return {"confusion": counts, "total_cases": len(results), "labelled_cases": labelled,
            "missing_answerability_labels": unknown, "failed_labelled_cases": failed,
            "abstention_detection": "explicit-boolean-or-text-heuristic-v1",
            "factoid_recall": ratio(correct, aa + az),
            "abstention_precision": ratio(uz, uz + az),
            "abstention_recall": ratio(uz, uz + ua),
            "false_abstention_rate": ratio(az, aa + az),
            "unsupported_answer_rate": ratio(ua, ua + uz),
            "unsupported_answer_definition": "answered-unanswerable / unanswerable; not citation support",
            "risk_coverage": {"coverage": ratio(aa + ua, labelled),
                              "risk": ratio(answered_wrong, aa + ua)}}


# ---------------------------------------------------------------------------
# Summary builder
# ---------------------------------------------------------------------------

def build_summary(
    results: list[dict[str, Any]],
    *,
    label_field: str,
    include_llm: bool,
    dataset: str = "",
) -> dict[str, Any]:
    summary: dict[str, Any] = {
        "overall_accuracy": (
            sum(1 for row in results if row["verdict"] == "CORRECT") / len(results) if results else 0.0
        ),
        "breakdown": category_accuracy(results, label_field),
    }
    retrieval_values = [float(row["retrieval_latency_s"]) for row in results]
    summary["latency"] = {"retrieval": summarize_latencies(retrieval_values)}
    if include_llm:
        answer_values = [float(row["answer_latency_s"]) for row in results]
        judge_values = [float(row["judge_latency_s"]) for row in results]
        wall_values = [float(row["wall_clock_s"]) for row in results]
        costs = [float(row["cost"]["total_usd"]) for row in results]
        correct_count = sum(1 for row in results if row["verdict"] == "CORRECT")
        summary["latency"]["answer"] = summarize_latencies(answer_values)
        summary["latency"]["judge"] = summarize_latencies(judge_values)
        summary["latency"]["wall_clock"] = summarize_latencies(wall_values)
        summary["cost"] = summarize_costs(costs, correct_count)

        # Derived LongMemEval metrics (only populated for LME LLM track)
        if all("generated_answer" in r for r in results):
            summary["derived"] = compute_longmemeval_derived(results)

    return summary


def write_result_file(path: Path, payload: dict[str, Any]) -> None:
    # A malformed coverage block is worse than none: it would be read back as
    # proof of a complete run. Reject it at the point of writing, where the
    # producer that got it wrong is still on the stack.
    if "dataset_inventory" in payload:
        from benchmarks.common.dataset_inventory import validate_inventory_results
        validate_inventory_results(payload["dataset_inventory"], payload["results"])
        if "coverage" in payload:
            counts = payload["coverage"]["counts"]
            counts["excluded_questions"] = sum(r["disposition"] == "excluded" for r in payload["dataset_inventory"]["cases"])
            counts["failed_questions"] = sum(r.get("run_status", "ok") != "ok" for r in payload["results"])
            payload["coverage"]["complete"] = payload["coverage"]["complete"] and not (counts["excluded_questions"] or counts["failed_questions"])
    if "coverage" in payload:
        validate_coverage(payload["coverage"])
    path.parent.mkdir(parents=True, exist_ok=True)
    import os
    import tempfile
    data = json.dumps(payload, indent=2, allow_nan=False) + "\n"
    fd, temporary = tempfile.mkstemp(prefix="." + path.name, dir=path.parent)
    try:
        with os.fdopen(fd, "w") as out:
            out.write(data)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def print_summary(dataset_name: str, track: str, summary: dict[str, Any], label_field: str) -> None:
    print(f"{dataset_name} {track} benchmark")
    print(f"  overall_accuracy={summary['overall_accuracy']:.3f}")
    print_breakdown(f"  by_{label_field}", summary["breakdown"])
    retrieval = summary["latency"]["retrieval"]
    print(f"  retrieval_latency: avg={retrieval['avg_s']:.3f}s p95={retrieval['p95_s']:.3f}s")
    if track == "llm":
        for name in ("answer", "judge", "wall_clock"):
            bucket = summary["latency"][name]
            print(f"  {name}_latency: avg={bucket['avg_s']:.3f}s p95={bucket['p95_s']:.3f}s")
        cost = summary["cost"]
        print(
            f"  cost: total=${cost['total_usd']:.5f} "
            f"per_query=${cost['per_query_usd']:.5f} per_correct=${cost['per_correct_usd']:.5f}"
        )
        derived = summary.get("derived")
        if derived:
            print("  answerability=" + json.dumps(derived, sort_keys=True))
