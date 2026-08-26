#!/usr/bin/env python3
"""One-task paired Codex calibration for current-stack economizer accounting."""
from __future__ import annotations

import argparse
import json
import random
import subprocess
import tempfile
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from benchmarks.roi.current_stack_pilot import (
    EconomizerProbe,
    SYSTEM_PROMPT,
    build_probe,
    build_tasks,
    exact_grade,
    sha256_json,
)


SEED = 20260826
INPUT_USD_PER_MILLION = 4.00
CACHED_INPUT_USD_PER_MILLION = 0.40
CACHE_WRITE_USD_PER_MILLION = 5.00
OUTPUT_USD_PER_MILLION = 20.00
LONG_CONTEXT_THRESHOLD = 272_000
MODEL_CONTEXT_CAP = 1_050_000
MODEL_OUTPUT_CAP = 128_000


def api_equivalent_cost(usage: dict[str, int]) -> float:
    long = usage["input_tokens"] > LONG_CONTEXT_THRESHOLD
    input_multiplier = 2.0 if long else 1.0
    output_multiplier = 1.5 if long else 1.0
    uncached = max(
        0,
        usage["input_tokens"]
        - usage["cached_input_tokens"]
        - usage["cache_write_input_tokens"],
    )
    return (
        uncached * INPUT_USD_PER_MILLION * input_multiplier
        + usage["cached_input_tokens"] * CACHED_INPUT_USD_PER_MILLION * input_multiplier
        + usage["cache_write_input_tokens"] * CACHE_WRITE_USD_PER_MILLION * input_multiplier
        + usage["output_tokens"] * OUTPUT_USD_PER_MILLION * output_multiplier
    ) / 1_000_000


def run_codex(prompt: str, model: str) -> dict[str, Any]:
    command = [
        "codex", "exec", "--ephemeral", "--json", "--ignore-user-config",
        "--ignore-rules", "--skip-git-repo-check", "--model", model,
        "--sandbox", "read-only", "-C", "/tmp", prompt,
    ]
    started = time.monotonic()
    completed = subprocess.run(
        command, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=600,
    )
    elapsed = time.monotonic() - started
    if completed.returncode:
        raise RuntimeError(f"Codex exited {completed.returncode}: {completed.stderr[:1000]}")
    events = []
    for line in completed.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        events.append(json.loads(line))

    thread_ids = [event.get("thread_id") for event in events if event.get("type") == "thread.started"]
    terminal = [event for event in events if event.get("type") == "turn.completed"]
    messages = [
        event["item"].get("text", "") for event in events
        if event.get("type") == "item.completed"
        and (event.get("item") or {}).get("type") == "agent_message"
    ]
    unexpected = [
        event for event in events
        if event.get("type") == "item.completed"
        and (event.get("item") or {}).get("type") != "agent_message"
    ]
    if len(thread_ids) != 1 or not thread_ids[0]:
        raise RuntimeError(f"expected one attributable Codex thread, got {thread_ids}")
    if len(terminal) != 1 or not isinstance(terminal[0].get("usage"), dict):
        raise RuntimeError(f"expected one terminal usage object, got {terminal}")
    if len(messages) != 1 or unexpected:
        raise RuntimeError("Codex used a tool or emitted a non-unique final message")

    raw_usage = terminal[0]["usage"]
    usage = {
        "input_tokens": int(raw_usage.get("input_tokens") or 0),
        "cached_input_tokens": int(raw_usage.get("cached_input_tokens") or 0),
        "cache_write_input_tokens": int(raw_usage.get("cache_write_input_tokens") or 0),
        "output_tokens": int(raw_usage.get("output_tokens") or 0),
        "reasoning_output_tokens": int(raw_usage.get("reasoning_output_tokens") or 0),
    }
    if usage["input_tokens"] < 1 or usage["output_tokens"] < 0:
        raise RuntimeError(f"invalid Codex usage: {usage}")
    if usage["cached_input_tokens"] + usage["cache_write_input_tokens"] > usage["input_tokens"]:
        raise RuntimeError(f"overlapping Codex input buckets: {usage}")
    return {
        "thread_id": thread_ids[0],
        "answer": messages[0],
        "usage": usage,
        "raw_usage": raw_usage,
        "wall_seconds": elapsed,
        "stderr": completed.stderr,
    }


def summarize(rows: list[dict[str, Any]]) -> dict[str, Any]:
    result = {}
    for condition in ("off", "full"):
        selected = [item for item in rows if item["condition"] == condition]
        result[condition] = {
            "calls": len(selected),
            "resolved": sum(bool(row["resolved"]) for row in selected),
            **{
                key: sum(row["usage"][key] for row in selected)
                for key in (
                    "input_tokens", "cached_input_tokens", "cache_write_input_tokens",
                    "output_tokens", "reasoning_output_tokens",
                )
            },
            "api_price_equivalent_usd": sum(
                row["api_price_equivalent_usd"] for row in selected
            ),
            "actual_marginal_cash_usd": 0.0,
        }
    off, full = result["off"], result["full"]
    return {
        "by_condition": result,
        "input_token_delta": full["input_tokens"] - off["input_tokens"],
        "output_token_delta": full["output_tokens"] - off["output_tokens"],
        "api_price_equivalent_delta_usd": (
            full["api_price_equivalent_usd"] - off["api_price_equivalent_usd"]
        ),
        "quality_gate_equal_resolved": (
            full["resolved"] == off["resolved"] == full["calls"] == off["calls"]
        ),
        "unique_threads": len({row["thread_id"] for row in rows}) == len(rows),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default="gpt-5.6-sol")
    parser.add_argument("--coordinate-density", choices=("low", "high"), default="high")
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--actual-marginal-budget-usd", required=True, type=float)
    parser.add_argument("--api-equivalent-budget-usd", required=True, type=float)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.repeats < 1:
        raise SystemExit("repeats must be positive")
    repo = Path(__file__).resolve().parents[2]
    dirty = subprocess.check_output(
        ["git", "status", "--porcelain", "--untracked-files=all"], cwd=repo, text=True,
    )
    if dirty:
        raise SystemExit("source worktree is dirty; commit the runner before recording lineage")
    login = subprocess.run(["codex", "login", "status"], capture_output=True, text=True, check=True)
    if "Logged in using ChatGPT" not in login.stdout + login.stderr:
        raise SystemExit("Codex is not using the preregistered ChatGPT contract")
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()

    calls = 2 * args.repeats
    expected_usage = {
        "input_tokens": 40_000 * args.repeats,
        "cached_input_tokens": 20_000 * args.repeats,
        "cache_write_input_tokens": 0,
        "output_tokens": 64 * args.repeats,
    }
    expected_api = api_equivalent_cost(expected_usage)
    hard_api_per_call = (
        MODEL_CONTEXT_CAP * CACHE_WRITE_USD_PER_MILLION * 2.0
        + MODEL_OUTPUT_CAP * OUTPUT_USD_PER_MILLION * 1.5
    ) / 1_000_000
    hard_api = calls * hard_api_per_call
    budget = {
        "calls": calls,
        "retries": 0,
        "actual_contract": "Codex CLI authenticated using ChatGPT; fixed-plan quota, no per-call cash charge",
        "expected_actual_marginal_cash_usd": 0.0,
        "hard_actual_marginal_cash_usd": 0.0,
        "actual_marginal_budget_usd": args.actual_marginal_budget_usd,
        "expected_api_price_equivalent_usd": expected_api,
        "hard_api_price_equivalent_usd": hard_api,
        "api_equivalent_budget_usd": args.api_equivalent_budget_usd,
        "pricing_generation": "official-gpt-5.6-sol-2026-08-26",
    }
    print(json.dumps({"budget_preflight": budget}, indent=2), flush=True)
    if args.actual_marginal_budget_usd < 0 or args.api_equivalent_budget_usd < hard_api:
        raise SystemExit("budget limit is below hard maximum; no Codex inference dispatched")

    task = build_tasks(1, "tail", args.coordinate_density)[0]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    preflight = args.output.with_suffix(".preflight.json")
    preflight.write_text(json.dumps({
        "schema_version": 1,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "commit": commit,
        "model": args.model,
        "coordinate_density": args.coordinate_density,
        "task_corpus_sha256": sha256_json([task.__dict__]),
        "budget": budget,
        "dispatch_started": False,
    }, indent=2, sort_keys=True) + "\n")

    temporary = tempfile.TemporaryDirectory(prefix="aimee-roi-codex-")
    probe_path = Path(temporary.name) / "aimee-economizer-probe"
    build_probe(repo, probe_path)
    probe = EconomizerProbe(probe_path)
    reduced = probe.reduce(task.messages)
    probe.close()
    temporary.cleanup()
    if not reduced.get("mutated") or reduced.get("reason") != "reduced":
        raise RuntimeError(f"economizer activation failed: {reduced}")

    plan = []
    for repeat in range(args.repeats):
        pair = [("off", task.messages), ("full", reduced["messages"])]
        random.Random(SEED + repeat).shuffle(pair)
        plan.extend((repeat, condition, messages) for condition, messages in pair)
    run_id = "roi-codex-pilot-" + uuid.uuid4().hex[:16]
    rows = []
    started = time.monotonic()
    for ordinal, (repeat, condition, messages) in enumerate(plan):
        cache_nonce = f"{run_id}:repeat-{repeat}"
        prompt = (
            SYSTEM_PROMPT
            + f"\n\nExperiment cache-isolation nonce: {cache_nonce}"
            + "\nThe message history is this JSON array:\n"
            + json.dumps(messages, separators=(",", ":"))
            + "\n\nDo not use tools. Return only the exact requested identifier."
        )
        outcome = run_codex(prompt, args.model)
        row = {
            "run_id": run_id,
            "call_id": f"{run_id}:{repeat}:{ordinal}:{condition}",
            "repeat": repeat,
            "cache_isolation_nonce": cache_nonce,
            "condition": condition,
            "task_id": task.task_id,
            "model": args.model,
            "thread_id": outcome["thread_id"],
            "answer": outcome["answer"],
            "expected": task.expected,
            "resolved": exact_grade(outcome["answer"], task.expected, {task.expected}),
            "usage": outcome["usage"],
            "raw_usage": outcome["raw_usage"],
            "api_price_equivalent_usd": api_equivalent_cost(outcome["usage"]),
            "actual_marginal_cash_usd": 0.0,
            "wall_seconds": outcome["wall_seconds"],
            "request_sha256": sha256_json(prompt),
            "economizer": reduced if condition == "full" else {
                "mutated": False, "reason": "off", "byte_identical": True,
            },
        }
        rows.append(row)
        print(json.dumps({
            "completed": ordinal + 1, "condition": condition,
            "resolved": row["resolved"], "usage": row["usage"],
        }), flush=True)

    artifact = {
        "schema_version": 1,
        "claim_status": "calibration_only",
        "execution_path": "production Go economizer handler in-process; ephemeral Codex CLI",
        "created_at": datetime.now(timezone.utc).isoformat(),
        "commit": commit,
        "run_id": run_id,
        "seed": SEED,
        "repeats": args.repeats,
        "model": args.model,
        "coordinate_density": args.coordinate_density,
        "task_corpus_sha256": sha256_json([task.__dict__]),
        "budget": budget,
        "calls": rows,
        "summary": summarize(rows),
        "wall_seconds": time.monotonic() - started,
    }
    args.output.write_text(json.dumps(artifact, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"artifact": str(args.output), "summary": artifact["summary"]}, indent=2))


if __name__ == "__main__":
    main()
