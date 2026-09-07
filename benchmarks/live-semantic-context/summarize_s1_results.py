#!/usr/bin/env python3
"""Summarize complete paired S1 evidence with frozen paired bootstrap intervals."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import random
import statistics
from typing import Any, Callable


BASE = Path(__file__).resolve().parent
CONTRACT = BASE / "s1-experiment-contract.json"
ARMS = ("production", "location_only", "batched_context")


def median(values: list[float]) -> float | None:
    return statistics.median(values) if values else None


def percentile(values: list[float], probability: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * probability
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1 - weight) + ordered[upper] * weight


def bootstrap_interval(values: list[Any], statistic: Callable[[list[Any]], float], *,
                       seed: int, replicates: int) -> dict[str, float] | None:
    if not values:
        return None
    generator = random.Random(seed)
    estimates = []
    for _ in range(replicates):
        sample = [values[generator.randrange(len(values))] for _ in values]
        estimates.append(statistic(sample))
    return {
        "estimate": statistic(values),
        "lower_95": percentile(estimates, 0.025),
        "upper_95": percentile(estimates, 0.975),
    }


def arm_summary(rows: list[dict[str, Any]]) -> dict[str, Any]:
    eligible = [row for row in rows if row.get("cell_eligible") and not row.get(
        "infrastructure_failure"
    )]
    exact = [
        row["grade"]["exact_target_correctness"] for row in eligible
        if row["grade"].get("exact_target_correctness") is not None
    ]
    usage = [row.get("usage") or {} for row in eligible]
    measurements = [row.get("measurement") or {} for row in eligible]
    return {
        "cells": len(rows),
        "eligible_cells": len(eligible),
        "task_success_rate": (
            sum(bool(row["grade"]["task_success"]) for row in eligible) / len(eligible)
            if eligible else None
        ),
        "exact_target_rate": sum(bool(value) for value in exact) / len(exact) if exact else None,
        "exact_target_denominator": len(exact),
        "median_wall_seconds": median([row["wall_seconds"] for row in eligible]),
        "median_agent_turns": median([row["agent_turns"] for row in eligible]),
        "median_tool_calls": median([row["tool_calls"] for row in eligible]),
        "median_input_tokens": median([item["input_tokens"] for item in usage]),
        "median_output_tokens": median([item["output_tokens"] for item in usage]),
        "median_preparation_bytes": median([
            item["preparation_bytes"] for item in measurements if "preparation_bytes" in item
        ]),
        "median_tool_input_bytes": median([
            item["tool_input_bytes"] for item in measurements if "tool_input_bytes" in item
        ]),
        "median_tool_output_bytes": median([
            item["tool_output_bytes"] for item in measurements if "tool_output_bytes" in item
        ]),
        "false_empty_count": sum(row["grade"].get("false_empty_count", 0) for row in eligible),
        "false_ok_empty_count": sum(
            row["grade"].get("false_ok_empty_count", 0) for row in eligible
        ),
        "stale_result_count": sum(row["grade"].get("stale_result_count", 0) for row in eligible),
        "authority_citation_rate": (
            sum(bool(row["grade"].get("authority_cited")) for row in eligible) / len(eligible)
            if eligible else None
        ),
    }


def eligible_pairs(rows: list[dict[str, Any]], semantic: bool | None) -> list[dict[str, Any]]:
    by_task: dict[str, dict[str, dict[str, Any]]] = {}
    for row in rows:
        if semantic is not None and row.get("semantic_eligible") is not semantic:
            continue
        if row.get("infrastructure_failure") or not row.get("cell_eligible"):
            continue
        by_task.setdefault(row["task_id"], {})[row["arm"]] = row
    return [arms for arms in by_task.values() if set(arms) == set(ARMS)]


def paired_summary(pairs: list[dict[str, Any]], *, seed: int,
                   replicates: int) -> dict[str, Any]:
    success_deltas = [
        int(pair["batched_context"]["grade"]["task_success"])
        - int(pair["production"]["grade"]["task_success"])
        for pair in pairs
    ]
    tool_reductions = [
        (pair["production"]["tool_calls"] - pair["batched_context"]["tool_calls"])
        / max(pair["production"]["tool_calls"], 1)
        for pair in pairs
    ]
    wall_reductions = [
        (pair["production"]["wall_seconds"] - pair["batched_context"]["wall_seconds"])
        / pair["production"]["wall_seconds"]
        for pair in pairs if pair["production"]["wall_seconds"] > 0
    ]
    input_reductions = [
        ((pair["production"].get("usage") or {}).get("input_tokens", 0)
         - (pair["batched_context"].get("usage") or {}).get("input_tokens", 0))
        / max((pair["production"].get("usage") or {}).get("input_tokens", 0), 1)
        for pair in pairs
    ]
    return {
        "complete_pairs": len(pairs),
        "task_success_absolute_delta": bootstrap_interval(
            success_deltas, statistics.mean, seed=seed, replicates=replicates
        ),
        "median_tool_call_reduction": bootstrap_interval(
            tool_reductions, statistics.median, seed=seed + 1, replicates=replicates
        ),
        "median_wall_time_reduction": bootstrap_interval(
            wall_reductions, statistics.median, seed=seed + 2, replicates=replicates
        ),
        "median_input_token_reduction": bootstrap_interval(
            input_reductions, statistics.median, seed=seed + 3, replicates=replicates
        ),
    }


def summarize(artifact: dict[str, Any], contract: dict[str, Any]) -> dict[str, Any]:
    rows = artifact.get("cells") or []
    analysis = contract["analysis_plan"]
    seed = analysis["bootstrap_seed"]
    replicates = analysis["bootstrap_replicates"]
    arm_metrics = {
        arm: arm_summary([row for row in rows if row.get("arm") == arm]) for arm in ARMS
    }
    semantic_pairs = eligible_pairs(rows, True)
    control_pairs = eligible_pairs(rows, False)
    all_pairs = eligible_pairs(rows, None)
    batched_semantic = [pair["batched_context"] for pair in semantic_pairs]
    adoption_denominator = len(batched_semantic)
    adoption = (
        sum(bool(row.get("candidate_used_before_decisive_edit")) for row in batched_semantic)
        / adoption_denominator if adoption_denominator else None
    )
    false_current = sum(
        (row.get("grade") or {}).get("false_current_results", 0) for row in rows
        if row.get("cell_eligible") and not row.get("infrastructure_failure")
    )
    typed_rows = [
        row for row in rows
        if row.get("cell_eligible") and not row.get("infrastructure_failure")
        and (row.get("grade") or {}).get("expected_behavior") == "typed_failure"
    ]
    semantic_comparison = paired_summary(
        semantic_pairs, seed=seed + 100, replicates=replicates
    )
    control_comparison = paired_summary(
        control_pairs, seed=seed + 200, replicates=replicates
    )
    success = semantic_comparison["task_success_absolute_delta"]
    tool = semantic_comparison["median_tool_call_reduction"]
    wall = semantic_comparison["median_wall_time_reduction"]
    choice_a = bool(
        success and success["estimate"] >= 0.05 and success["lower_95"] > 0
    )
    choice_b = bool(
        success and tool and wall and success["estimate"] >= 0
        and tool["estimate"] >= 0.20 and tool["lower_95"] > 0
        and wall["estimate"] >= 0.10 and wall["lower_95"] > 0
    )
    return {
        "schema_version": 1,
        "run_id": artifact.get("run_id"),
        "study_kind": artifact.get("study_kind", "paired_value"),
        "claim_status": artifact.get("claim_status"),
        "analysis": {
            "method": analysis["method"], "bootstrap_seed": seed,
            "bootstrap_replicates": replicates,
            "agent_turn_endpoint_eligible": False,
            "agent_turn_note": (
                "codex exec records one user turn per cell; tool calls are the frozen "
                "agentic round-trip endpoint"
            ),
        },
        "arm_metrics": arm_metrics,
        "paired": {
            "all": paired_summary(all_pairs, seed=seed, replicates=replicates),
            "semantic": semantic_comparison,
            "control": control_comparison,
        },
        "candidate_adoption": {
            "rate": adoption, "denominator": adoption_denominator,
        },
        "safety": {
            "false_current_results": false_current,
            "typed_failure_preservation_rate": (
                sum(bool(row["grade"].get("typed_failure_preserved")) for row in typed_rows)
                / len(typed_rows) if typed_rows else None
            ),
            "typed_failure_denominator": len(typed_rows),
            "false_ok_empty_count": sum(
                (row.get("grade") or {}).get("false_ok_empty_count", 0) for row in rows
                if row.get("cell_eligible") and not row.get("infrastructure_failure")
            ),
            "authority_citation_failures": sum(
                not bool((row.get("grade") or {}).get("authority_cited")) for row in rows
                if row.get("cell_eligible") and not row.get("infrastructure_failure")
            ),
        },
        "material_value_gate": {
            "choice_a_task_success": choice_a,
            "choice_b_round_trips_and_wall_time": choice_b,
            "passes_either_choice": choice_a or choice_b,
        },
        "unresolved_promotion_evidence": [
            "separate 12-cell adversarial failure artifact",
            "20 clean cold starts per provider and claimed platform",
            "checked fixture reference-quality aggregate",
        ],
        "promotion_decision": "incomplete",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("results", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    artifact = json.loads(args.results.read_text())
    contract = json.loads(CONTRACT.read_text())
    summary = summarize(artifact, contract)
    rendered = json.dumps(summary, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered)
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
