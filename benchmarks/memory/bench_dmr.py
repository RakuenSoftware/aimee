#!/usr/bin/env python3
"""DMR (Deep Memory Retrieval) benchmark — LLM-track grader."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from benchmarks.common.harness import AimeeHarness, collect_vector_runtime_metadata, git_commit, normalize_question
from benchmarks.common.llm_eval import (
    ANSWER_SYSTEM,
    build_answer_prompt,
    judge_majority,
    llm_cost_breakdown,
)
from benchmarks.common.result_schema import make_coverage
from benchmarks.common.runner import build_summary, write_result_file
from benchmarks.memory.dataset import _load_cases, _normalize_turns


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--top-k", type=int, default=100)
    parser.add_argument("--max-samples", type=int, default=0)
    parser.add_argument("--output", required=True)
    return parser


def store_turns(harness: AimeeHarness, home: Path, turns: list[dict[str, str]]) -> None:
    """Ingest a flat list of conversation turns into the memory system.

    Mirrors the pattern used by ``benchmarks.locomo.common`` for session replay.
    Each turn carries ``speaker`` and ``text``.
    """
    session = "dmr-0"
    for idx, turn in enumerate(turns):
        speaker = turn.get("speaker", "user")
        text = turn.get("text", "")
        if not text:
            continue
        key = f"dmr-turn-{idx}"
        content = f"[{speaker}] {text}"
        harness.store_memory(home, key=key, content=content, session=session, tier="L2", kind="fact")


def main() -> int:
    args = build_parser().parse_args()

    harness = AimeeHarness()
    results: list[dict] = []

    samples_run = 0
    for case in _load_cases(args.dataset, args.max_samples):
        samples_run += 1
        # Normalise both 'conversation' and 'sessions' shapes to a flat turn list.
        turns = _normalize_turns(case)
        tmp, home = harness.prepare_home()
        try:
            store_turns(harness, home, turns)
            for qrow in case["questions"]:
                question = normalize_question(qrow["question"])
                facts, retrieval_latency_s = harness.search_facts(home, question, args.top_k)
                retrieved_ids = [int(fact["id"]) for fact in facts]
                context = "\n".join(f"[{i + 1}] {f['content']}" for i, f in enumerate(facts))
                prompt = build_answer_prompt(qrow["question"], context)
                answer_exec = harness.agent_run(home, prompt=prompt, system=ANSWER_SYSTEM, max_tokens=256)
                votes, judge_latency_s, judge_in, judge_out, verdict = judge_majority(
                    harness,
                    home,
                    question=qrow["question"],
                    gold_answer=qrow["gold_answer"],
                    candidate=answer_exec.response,
                )
                costs = llm_cost_breakdown(harness, answer_exec, judge_in, judge_out)
                results.append(
                    {
                        "system": "aimee",
                        "track": "llm",
                        "git_commit": git_commit(),
                        "question_id": qrow.get("question_id", ""),
                        "category": qrow.get("category", ""),
                        "question": qrow["question"],
                        "gold_answer": qrow["gold_answer"],
                        "generated_answer": answer_exec.response,
                        "judge_votes": votes,
                        "verdict": verdict,
                        "retrieval_latency_s": round(retrieval_latency_s, 6),
                        "answer_latency_s": round(answer_exec.latency_s, 6),
                        "judge_latency_s": round(judge_latency_s, 6),
                        "wall_clock_s": round(
                            retrieval_latency_s + answer_exec.latency_s + judge_latency_s, 6
                        ),
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
        "dataset": "dmr",
        "system": "aimee",
        "track": "llm",
        "git_commit": git_commit(),
        "result_count": len(results),
        "agent_model": harness.current_model,
        "judge_runs": 3,
        "vector_runtime": collect_vector_runtime_metadata(),
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
    return 0


if __name__ == "__main__":
    sys.exit(main())