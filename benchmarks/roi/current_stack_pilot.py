#!/usr/bin/env python3
"""Run a bounded paired economizer pilot against an OpenAI-compatible endpoint.

The pilot is intentionally smaller than the confirmatory suite. It establishes
that the current production economizer handler activates, preserves planted
facts, and changes provider-reported billable token volume. Every provider
response is retained and reconciled one-to-one with an experiment call record.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import random
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import uuid
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SEED = 20260826
SYSTEM_PROMPT = (
    "You are checking an operations handoff. Answer the final user question "
    "with only the requested identifier and no explanation."
)


@dataclass(frozen=True)
class Task:
    task_id: str
    expected: str
    messages: list[dict[str, Any]]


def _filler(task_index: int, turn: int) -> str:
    lines = []
    for line in range(14):
        lines.append(
            f"check={task_index:02d}-{turn:02d}-{line:02d} subsystem=queue "
            f"status=healthy latency_ms={20 + line} retries=0 owner=platform"
        )
    return "\n".join(lines)


def build_tasks(count: int, fact_position: str = "tail") -> list[Task]:
    if fact_position not in {"folded", "tail"}:
        raise ValueError("fact_position must be 'folded' or 'tail'")
    fact_turn = 1 if fact_position == "folded" else 9
    tasks = []
    for task_index in range(count):
        expected = f"RBK-{task_index:02d}-7F39A2"
        messages: list[dict[str, Any]] = []
        for turn in range(10):
            messages.append({
                "role": "user",
                "content": f"Review deployment checkpoint {turn} for service svc-{task_index:02d}.",
            })
            detail = _filler(task_index, turn)
            if turn == fact_turn:
                detail += f"\nAUTHORITATIVE_ROLLBACK_TOKEN={expected}\n"
            messages.append({
                "role": "assistant",
                "content": "Checkpoint reviewed. The exact audit follows.\n" + detail,
            })
        messages.append({
            "role": "user",
            "content": (
                "Return only the exact value of AUTHORITATIVE_ROLLBACK_TOKEN "
                "from the handoff."
            ),
        })
        tasks.append(Task(f"ops-handoff-{task_index:02d}", expected, messages))
    return tasks


class EconomizerProbe:
    def __init__(self, binary: Path):
        self.process = subprocess.Popen(
            [str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1,
        )

    def reduce(self, messages: list[dict[str, Any]]) -> dict[str, Any]:
        request = {
            "messages": messages,
            "system_prompt": SYSTEM_PROMPT,
            "seam": "delegate",
            "history_fold": True,
            "compress": True,
            "retained_msgs": 8,
            "min_fold_msgs": 4,
            "excerpt_bytes": 256,
            "register_enabled": True,
            "compact_head_bytes": 192,
            "compact_tail_bytes": 192,
            "closet_enabled": True,
            "closet_budget_bytes": 4096,
            "closet_max_ratio_pct": 35,
        }
        assert self.process.stdin and self.process.stdout
        self.process.stdin.write(json.dumps(request, separators=(",", ":")) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            stderr = self.process.stderr.read() if self.process.stderr else ""
            raise RuntimeError(f"economizer probe exited without output: {stderr}")
        return json.loads(line)

    def close(self) -> None:
        if self.process.stdin:
            self.process.stdin.close()
        self.process.wait(timeout=10)
        if self.process.returncode:
            stderr = self.process.stderr.read() if self.process.stderr else ""
            raise RuntimeError(f"economizer probe failed: {stderr}")


def build_probe(repo: Path, output: Path) -> None:
    subprocess.run(
        ["go", "build", "-trimpath", "-o", str(output), "./cmd/aimee-economizer-probe"],
        cwd=repo / "server-go", check=True,
    )


def http_json(url: str, payload: dict[str, Any] | None = None, timeout: int = 300) -> dict[str, Any]:
    data = None if payload is None else json.dumps(payload).encode()
    request = urllib.request.Request(
        url, data=data, headers={"Content-Type": "application/json"},
        method="GET" if data is None else "POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        detail = error.read().decode(errors="replace")
        raise RuntimeError(f"HTTP {error.code} from {url}: {detail[:1000]}") from error


def exact_grade(text: str, expected: str, all_answers: set[str]) -> bool:
    normalized = text.strip().strip("`").strip()
    return normalized == expected and not any(
        answer != expected and answer in normalized for answer in all_answers
    )


def sha256_json(value: Any) -> str:
    body = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(body).hexdigest()


def usage_buckets(response: dict[str, Any]) -> dict[str, int]:
    usage = response.get("usage") or {}
    prompt = int(usage.get("prompt_tokens") or 0)
    completion = int(usage.get("completion_tokens") or 0)
    total = int(usage.get("total_tokens") or prompt + completion)
    if prompt < 1 or completion < 0 or total != prompt + completion:
        raise RuntimeError(f"unreconciled provider usage object: {usage}")
    return {"input_tokens": prompt, "output_tokens": completion, "total_tokens": total}


def summarize(calls: list[dict[str, Any]]) -> dict[str, Any]:
    by_condition: dict[str, dict[str, Any]] = {}
    for condition in ("off", "full"):
        rows = [row for row in calls if row["condition"] == condition]
        resolved = sum(bool(row["resolved"]) for row in rows)
        total = sum(row["usage"]["total_tokens"] for row in rows)
        by_condition[condition] = {
            "calls": len(rows),
            "resolved": resolved,
            "resolution_rate": resolved / len(rows) if rows else 0,
            "input_tokens": sum(row["usage"]["input_tokens"] for row in rows),
            "output_tokens": sum(row["usage"]["output_tokens"] for row in rows),
            "total_tokens": total,
            "tokens_per_resolved_task": total / resolved if resolved else None,
        }
    baseline = by_condition["off"]
    treatment = by_condition["full"]
    delta = treatment["total_tokens"] - baseline["total_tokens"]
    return {
        "by_condition": by_condition,
        "paired_total_token_delta": delta,
        "paired_total_token_reduction_pct": (
            -100.0 * delta / baseline["total_tokens"] if baseline["total_tokens"] else None
        ),
        "quality_gate_equal_resolved": treatment["resolved"] == baseline["resolved"],
        "all_calls_reconciled": all(row["usage_reconciled"] for row in calls),
        "unique_provider_response_ids": len({row["provider_response_id"] for row in calls}) == len(calls),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--tasks", type=int, default=6)
    parser.add_argument("--fact-position", choices=("folded", "tail"), default="tail")
    parser.add_argument("--max-output-tokens", type=int, default=32)
    parser.add_argument("--budget-limit-usd", type=float, required=True)
    parser.add_argument("--marginal-input-usd-per-million", type=float, default=0.0)
    parser.add_argument("--marginal-output-usd-per-million", type=float, default=0.0)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.tasks < 1 or args.max_output_tokens < 1:
        raise SystemExit("tasks and max-output-tokens must be positive")
    repo = Path(__file__).resolve().parents[2]
    dirty = subprocess.check_output(
        ["git", "status", "--porcelain", "--untracked-files=all"], cwd=repo, text=True,
    )
    if dirty:
        raise SystemExit("source worktree is dirty; commit the runner before recording lineage")
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
    tasks = build_tasks(args.tasks, args.fact_position)
    model_catalog = http_json(args.base_url.rstrip("/") + "/v1/models")
    model_ids = [entry.get("id") for entry in model_catalog.get("data", [])]
    if args.model not in model_ids:
        raise SystemExit(f"model {args.model!r} absent from endpoint catalog")

    # Upper-bound the INPUT with the provider's own tokenizer using the original
    # prompt size observed during a one-call calibration is forbidden here: it
    # would itself spend. The pre-dispatch hard maximum therefore uses the
    # endpoint's declared 65,536-token context for every call.
    calls_planned = len(tasks) * 2
    context_cap = 65536
    expected_input = sum(len(json.dumps(t.messages)) // 4 for t in tasks) * 2
    expected_output = calls_planned * min(8, args.max_output_tokens)
    hard_input = calls_planned * context_cap
    hard_output = calls_planned * args.max_output_tokens
    expected_usd = (
        expected_input * args.marginal_input_usd_per_million
        + expected_output * args.marginal_output_usd_per_million
    ) / 1_000_000
    hard_usd = (
        hard_input * args.marginal_input_usd_per_million
        + hard_output * args.marginal_output_usd_per_million
    ) / 1_000_000
    budget = {
        "pricing_contract": "operator-owned local endpoint; marginal provider rate explicitly configured",
        "expected_input_tokens_estimate": expected_input,
        "expected_output_tokens_estimate": expected_output,
        "hard_input_token_cap": hard_input,
        "hard_output_token_cap": hard_output,
        "expected_spend_usd": expected_usd,
        "hard_maximum_spend_usd": hard_usd,
        "budget_limit_usd": args.budget_limit_usd,
    }
    print(json.dumps({"budget_preflight": budget}, indent=2), flush=True)
    if args.budget_limit_usd < hard_usd:
        raise SystemExit("budget limit is below hard maximum; no inference dispatched")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    preflight_path = args.output.with_suffix(".preflight.json")
    preflight_path.write_text(json.dumps({
        "schema_version": 1,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "commit": commit,
        "model": args.model,
        "fact_position": args.fact_position,
        "task_corpus_sha256": sha256_json([asdict(task) for task in tasks]),
        "calls_planned": calls_planned,
        "budget": budget,
        "dispatch_started": False,
    }, indent=2, sort_keys=True) + "\n")

    temporary = tempfile.TemporaryDirectory(prefix="aimee-roi-pilot-")
    probe_path = Path(temporary.name) / "aimee-economizer-probe"
    build_probe(repo, probe_path)
    probe = EconomizerProbe(probe_path)
    rng = random.Random(SEED)
    plan = [(task, condition) for task in tasks for condition in ("off", "full")]
    rng.shuffle(plan)
    all_answers = {task.expected for task in tasks}
    run_id = "roi-pilot-" + uuid.uuid4().hex[:16]
    rows: list[dict[str, Any]] = []
    started = time.monotonic()

    try:
        for ordinal, (task, condition) in enumerate(plan):
            messages = task.messages
            activation: dict[str, Any]
            if condition == "full":
                activation = probe.reduce(messages)
                if not activation.get("mutated") or activation.get("reason") != "reduced":
                    raise RuntimeError(f"economizer activation failed for {task.task_id}: {activation}")
                messages = activation.get("messages") or []
            else:
                activation = {
                    "mutated": False,
                    "reason": "off",
                    "input_sha256": sha256_json(messages),
                    "forwarded_sha256": sha256_json(messages),
                    "byte_identical": True,
                }
            call_id = f"{run_id}:{ordinal:03d}:{task.task_id}:{condition}"
            payload = {
                "model": args.model,
                "messages": [{"role": "system", "content": SYSTEM_PROMPT}, *messages],
                "temperature": 0,
                "max_tokens": args.max_output_tokens,
                "stream": False,
                "chat_template_kwargs": {"enable_thinking": False},
            }
            call_started = time.monotonic()
            response = http_json(args.base_url.rstrip("/") + "/v1/chat/completions", payload)
            elapsed = time.monotonic() - call_started
            usage = usage_buckets(response)
            choices = response.get("choices") or []
            content = ""
            if choices:
                content = str((choices[0].get("message") or {}).get("content") or "")
            provider_id = str(response.get("id") or "")
            if not provider_id:
                raise RuntimeError("provider response has no id; lineage cannot reconcile")
            row = {
                "run_id": run_id,
                "call_id": call_id,
                "task_id": task.task_id,
                "condition": condition,
                "provider": "llama.cpp-local",
                "model": args.model,
                "provider_response_id": provider_id,
                "usage": usage,
                "usage_reconciled": True,
                "marginal_provider_cost_usd": (
                    usage["input_tokens"] * args.marginal_input_usd_per_million
                    + usage["output_tokens"] * args.marginal_output_usd_per_million
                ) / 1_000_000,
                "wall_seconds": elapsed,
                "expected": task.expected,
                "answer": content,
                "resolved": exact_grade(content, task.expected, all_answers),
                "economizer": activation,
                "request_messages_sha256": sha256_json(payload["messages"]),
                "raw_provider_usage": response.get("usage"),
            }
            rows.append(row)
            print(json.dumps({
                "completed": ordinal + 1, "planned": len(plan),
                "task_id": task.task_id, "condition": condition,
                "input_tokens": usage["input_tokens"], "resolved": row["resolved"],
            }), flush=True)
    finally:
        probe.close()
        temporary.cleanup()

    artifact = {
        "schema_version": 1,
        "claim_status": "pilot_only",
        "execution_path": "production Go economizer handler in-process; direct local Qwen provider",
        "run_id": run_id,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "commit": commit,
        "seed": SEED,
        "model": args.model,
        "fact_position": args.fact_position,
        "chat_template_kwargs": {"enable_thinking": False},
        "model_catalog": model_catalog,
        "task_corpus_sha256": sha256_json([asdict(task) for task in tasks]),
        "budget": budget,
        "calls": rows,
        "summary": summarize(rows),
        "wall_seconds": time.monotonic() - started,
    }
    args.output.write_text(json.dumps(artifact, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"artifact": str(args.output), "summary": artifact["summary"]}, indent=2))


if __name__ == "__main__":
    main()
