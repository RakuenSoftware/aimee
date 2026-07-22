#!/usr/bin/env python3
"""Evaluate latency_slo.v1 transport benchmark artifacts using stdlib only."""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "latency_slo.v1"
MIN_ELIGIBLE_ATTEMPTS = 10_000
MAX_P50_MS = 10.0
MAX_P99_MS = 20.0
MAX_COMBINED_FAILURE_TAIL_RATE = 0.01
MIN_CONFIDENCE = 0.95


class ContractError(ValueError):
    pass


def nearest_rank(values: list[float], quantile: float) -> float:
    if not values:
        raise ContractError("cannot calculate a percentile without eligible attempts")
    if not 0 < quantile <= 1:
        raise ContractError("quantile must be in (0, 1]")
    ordered = sorted(values)
    return ordered[math.ceil(quantile * len(ordered)) - 1]


def _binomial_cdf(k: int, n: int, probability: float) -> float:
    if probability <= 0:
        return 1.0
    if probability >= 1:
        return 1.0 if k == n else 0.0
    logs = [
        math.lgamma(n + 1)
        - math.lgamma(i + 1)
        - math.lgamma(n - i + 1)
        + i * math.log(probability)
        + (n - i) * math.log1p(-probability)
        for i in range(k + 1)
    ]
    peak = max(logs)
    return math.exp(peak) * math.fsum(math.exp(value - peak) for value in logs)


def one_sided_binomial_upper(k: int, n: int, confidence: float = 0.95) -> float:
    """Exact Clopper-Pearson upper confidence bound for a binomial rate."""
    if n <= 0 or k < 0 or k > n:
        raise ContractError("invalid binomial counts")
    if not 0 < confidence < 1:
        raise ContractError("confidence must be in (0, 1)")
    if k == n:
        return 1.0
    alpha = 1.0 - confidence
    low, high = k / n, 1.0
    for _ in range(80):
        mid = (low + high) / 2.0
        if _binomial_cdf(k, n, mid) > alpha:
            low = mid
        else:
            high = mid
    return high


def _number(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ContractError(f"{field} must be numeric")
    result = float(value)
    if not math.isfinite(result) or result < 0:
        raise ContractError(f"{field} must be finite and non-negative")
    return result


def evaluate(document: dict[str, Any]) -> dict[str, Any]:
    if document.get("schema_version") != SCHEMA_VERSION:
        raise ContractError(f"schema_version must be {SCHEMA_VERSION!r}")
    if document.get("eligibility_set_before_execution") is not True:
        raise ContractError("eligibility_set_before_execution must be true")
    profile = document.get("profile")
    path = document.get("path")
    if not isinstance(profile, str) or not profile:
        raise ContractError("profile must be a non-empty string")
    if not isinstance(path, str) or not path:
        raise ContractError("path must be a non-empty string")

    budget = document.get("budget")
    if not isinstance(budget, dict):
        raise ContractError("budget must be an object")
    p50_limit = _number(budget.get("p50_ms"), "budget.p50_ms")
    p99_limit = _number(budget.get("p99_ms"), "budget.p99_ms")
    rate_limit = _number(
        budget.get("combined_failure_tail_rate"), "budget.combined_failure_tail_rate"
    )
    confidence = _number(budget.get("confidence", 0.95), "budget.confidence")
    if rate_limit > 1 or not 0 < confidence < 1:
        raise ContractError("rate must be <= 1 and confidence must be in (0, 1)")
    if p50_limit > MAX_P50_MS or p99_limit > MAX_P99_MS:
        raise ContractError(
            f"transport budgets may not exceed p50={MAX_P50_MS:g}ms / p99={MAX_P99_MS:g}ms"
        )
    if rate_limit > MAX_COMBINED_FAILURE_TAIL_RATE:
        raise ContractError(
            "combined_failure_tail_rate may not exceed "
            f"{MAX_COMBINED_FAILURE_TAIL_RATE:g}"
        )
    if confidence < MIN_CONFIDENCE:
        raise ContractError(f"confidence may not be below {MIN_CONFIDENCE:g}")

    attempts = document.get("attempts")
    if not isinstance(attempts, list):
        raise ContractError("attempts must be an array")
    latencies: list[float] = []
    bad = 0
    for index, attempt in enumerate(attempts):
        if not isinstance(attempt, dict) or not isinstance(attempt.get("eligible"), bool):
            raise ContractError(f"attempts[{index}].eligible must be boolean")
        if not attempt["eligible"]:
            continue
        if not isinstance(attempt.get("success"), bool):
            raise ContractError(f"attempts[{index}].success must be boolean")
        latency = _number(attempt.get("latency_ms"), f"attempts[{index}].latency_ms")
        latencies.append(latency)
        if not attempt["success"] or latency > p99_limit:
            bad += 1

    if len(latencies) < MIN_ELIGIBLE_ATTEMPTS:
        raise ContractError(
            f"need at least {MIN_ELIGIBLE_ATTEMPTS} eligible attempts; got {len(latencies)}"
        )

    p50 = nearest_rank(latencies, 0.50)
    p99 = nearest_rank(latencies, 0.99)
    upper = one_sided_binomial_upper(bad, len(latencies), confidence)
    checks = {
        "p50": p50 <= p50_limit,
        "p99": p99 <= p99_limit,
        "combined_failure_tail_upper": upper <= rate_limit,
    }
    return {
        "schema_version": SCHEMA_VERSION,
        "profile": profile,
        "path": path,
        "eligible_attempts": len(latencies),
        "excluded_attempts": len(attempts) - len(latencies),
        "combined_failure_tail_events": bad,
        "p50_ms": p50,
        "p99_ms": p99,
        "combined_failure_tail_upper": upper,
        "confidence": confidence,
        "budget": budget,
        "checks": checks,
        "passed": all(checks.values()),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path)
    args = parser.parse_args()
    try:
        document = json.loads(args.artifact.read_text(encoding="utf-8"))
        if not isinstance(document, dict):
            raise ContractError("artifact root must be an object")
        result = evaluate(document)
    except (OSError, json.JSONDecodeError, ContractError) as error:
        print(json.dumps({"schema_version": SCHEMA_VERSION, "passed": False, "error": str(error)}))
        return 2
    print(json.dumps(result, sort_keys=True))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
