#!/usr/bin/env python3
"""MSC (Multi-Session Chat) conversational-memory benchmark runner.

Grader: LLM (majority vote).  Dataset: JSONL with per-case conversations
and question lists.

Usage:
  python3 benchmarks/memory/bench_msc.py \\
    --dataset data/msc/test.jsonl --output benchmarks/results/msc.json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from benchmarks.common.harness import AimeeHarness, git_commit, normalize_question
from benchmarks.common.llm_eval import (
    ANSWER_SYSTEM,
    build_answer_prompt,
    judge_majority,
    llm_cost_breakdown,
)
from benchmarks.common.result_schema import make_coverage
from benchmarks.common.runner import build_summary, print_summary, write_result_file


def _normalize_turn(turn: dict) -> dict:
    """Coerce a raw turn into {speaker, text} with sane defaults."""
    speaker = turn.get("speaker", turn.get("role", "speaker"))
    text = turn.get("text", turn.get("content", ""))
    return {"speaker": str(speaker), "text": str(text)}


def _normalize_turns(sample: dict) -> list[dict]:
    """Flatten conversation/sessions into a flat list of {speaker, text} turns."""
    turns: list[dict] = []

    # Explicit flat conversation list
    if "conversation" in sample and isinstance(sample["conversation"], list):
        for turn in sample["conversation"]:
            if isinstance(turn, dict):
                turns.append(_normalize_turn(turn))
        return turns

    # Sessions: list of session-objects, each with a turns list
    for session in sample.get("sessions", []):
        for turn in session.get("turns", []):
            if isinstance(turn, dict):
                turns.append(_normalize_turn(turn))

    return turns


def _load_cases(path: str, max_cases: int = 0) -> list[dict]:
    """Load JSONL cases from *path*, applying *max_cases* truncation."""
    cases = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            cases.append(json.loads(line))
    if max_cases > 0:
        cases = cases[:max_cases]
    return cases


def _store_conversation(harness: AimeeHarness, home: Path, sample: dict) -> None:
    """Ingest all turns from *sample* into memory."""
    for idx, turn in enumerate(_normalize_turns(sample)):
        speaker = str(turn.get("speaker", "speaker"))
        text = str(turn.get("text", "")).strip()
        if not text:
            continue
        key = str(turn.get("dia_id") or f"msc-turn-{idx + 1}")
        harness.store_memory(
            home,
            key=key,
            content=f"{speaker}: {text}",
            session=str(turn.get("session", "")),
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--top-k", type=int, default=100)
    parser.add_argument("--max-samples", type=int, default=0)
    parser.add_argument("--output", required=True)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    harness = AimeeHarness()
    results = []

    samples_run = 0
    for case in _load_cases(args.dataset, args.max_samples):
        samples_run += 1
        tmp, home = harness.prepare_home()
        try:
            _store_conversation(harness, home, case)
            for row in case["questions"]:
                question_text = normalize_question(row["question"])
                facts, retrieval_latency_s = harness.search_facts(home, question_text, args.top_k)
                retrieved_ids = [int(fact["id"]) for fact in facts]
                context = "\n".join(f"[{idx + 1}] {fact['content']}" for idx, fact in enumerate(facts))
                prompt = build_answer_prompt(row["question"], context)
                answer_exec = harness.agent_run(home, prompt=prompt, system=ANSWER_SYSTEM, max_tokens=256)
                votes, judge_latency_s, judge_in, judge_out, verdict = judge_majority(
                    harness,
                    home,
                    question=row["question"],
                    gold_answer=row["gold_answer"],
                    candidate=answer_exec.response,
                )
                costs = llm_cost_breakdown(harness, answer_exec, judge_in, judge_out)
                results.append(
                    {
                        "system": "aimee",
                        "track": "llm",
                        "git_commit": git_commit(),
                        "question_id": row.get("question_id", ""),
                        "category": row.get("category", ""),
                        "question": row["question"],
                        "gold_answer": row["gold_answer"],
                        "generated_answer": answer_exec.response,
                        "judge_votes": votes,
                        "verdict": verdict,
                        "retrieval_latency_s": round(retrieval_latency_s, 6),
                        "answer_latency_s": round(answer_exec.latency_s, 6),
                        "judge_latency_s": round(judge_latency_s, 6),
                        "wall_clock_s": round(retrieval_latency_s + answer_exec.latency_s + judge_latency_s, 6),
                        "retrieved_ids": retrieved_ids,
                        "citations": [{"node_id": rid, "relation": "memory"} for rid in retrieved_ids[:5]],
                        "tokens": {
                            "answer_in": answer_exec.prompt_tokens,
                            "answer_out": answer_exec.completion_tokens,
                            "judge_in": judge_in,
                            "judge_out": judge_out,
                        },
                        "cost": costs,
                    }
                )
        finally:
            tmp.cleanup()

    summary = build_summary(results, label_field="category", include_llm=True)
    payload = {
        "dataset": "msc",
        "system": "aimee",
        "track": "llm",
        "git_commit": git_commit(),
        "result_count": len(results),
        "agent_model": harness.current_model,
        "judge_runs": 3,
        # Records whether this run was capped, so a subsample cannot later be
        # compared against a full-run baseline without the difference showing.
        # See require_complete_run in benchmarks/common/result_schema.py.
        "coverage": make_coverage(
            max_samples=args.max_samples,
            samples_run=samples_run,
            questions_run=len(results),
        ),
        "results": results,
        "summary": summary,
    }
    write_result_file(Path(args.output), payload)
    print_summary("msc", "llm", summary, "category")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())