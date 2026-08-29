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

def compute_longmemeval_derived(results: list[dict[str, Any]]) -> dict[str, float]:
    """Compute factoid_recall, abstention_precision, and false_abstention_rate.

    All LongMemEval questions have gold answers (none are truly unanswerable),
    so:
      - factoid_recall       = fraction correctly answered (CORRECT and not abstained)
      - false_abstention_rate = fraction where the model abstained (all are false negatives)
      - abstention_precision  = fraction of abstentions that scored CORRECT anyway
                                (judge may still award CORRECT for partial credit)
    """
    if not results:
        return {"factoid_recall": 0.0, "abstention_precision": 0.0, "false_abstention_rate": 0.0}

    n = len(results)
    abstained = [r for r in results if is_abstention(str(r.get("generated_answer") or ""))]
    n_abstained = len(abstained)

    # Recall: answered correctly without abstaining
    n_correct_factoid = sum(
        1 for r in results
        if r.get("verdict") == "CORRECT" and not is_abstention(str(r.get("generated_answer") or ""))
    )
    factoid_recall = n_correct_factoid / n

    # False abstention: model abstained on an answerable question (all LME questions are answerable)
    false_abstention_rate = n_abstained / n

    # Abstention precision: of abstentions, how many scored CORRECT (partial credit)
    if n_abstained:
        n_abstained_correct = sum(1 for r in abstained if r.get("verdict") == "CORRECT")
        abstention_precision = n_abstained_correct / n_abstained
    else:
        abstention_precision = 1.0  # vacuously perfect if no abstentions

    return {
        "factoid_recall": round(factoid_recall, 4),
        "abstention_precision": round(abstention_precision, 4),
        "false_abstention_rate": round(false_abstention_rate, 4),
    }


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
        if dataset == "longmemeval" and all("generated_answer" in r for r in results):
            summary["derived"] = compute_longmemeval_derived(results)

    return summary


def write_result_file(path: Path, payload: dict[str, Any]) -> None:
    # A malformed coverage block is worse than none: it would be read back as
    # proof of a complete run. Reject it at the point of writing, where the
    # producer that got it wrong is still on the stack.
    if "coverage" in payload:
        validate_coverage(payload["coverage"])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n")


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
            print(
                f"  factoid_recall={derived['factoid_recall']:.3f} "
                f"abstention_precision={derived['abstention_precision']:.3f} "
                f"false_abstention_rate={derived['false_abstention_rate']:.3f}"
            )
