#!/usr/bin/env python3

from __future__ import annotations

import argparse
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
from benchmarks.common.runner import build_summary, print_summary, write_result_file
from benchmarks.longmemeval.common.dataset import load_cases


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--top-k", type=int, default=100)
    parser.add_argument("--max-cases", type=int, default=0)
    parser.add_argument("--output", required=True)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    harness = AimeeHarness()
    results = []

    samples_run = 0
    for case in load_cases(args.dataset, args.max_cases):
        samples_run += 1
        tmp, home = harness.prepare_home()
        try:
            store_case(harness, home, case)
            facts, retrieval_latency_s = harness.search_facts(
                home, normalize_question(case["question"]), args.top_k
            )
            retrieved_ids = [int(fact["id"]) for fact in facts]
            context = "\n".join(f"[{idx + 1}] {fact['content']}" for idx, fact in enumerate(facts))
            prompt = build_answer_prompt(case["question"], context)
            answer_exec = harness.agent_run(home, prompt=prompt, system=ANSWER_SYSTEM, max_tokens=256)
            votes, judge_latency_s, judge_in, judge_out, verdict = judge_majority(
                harness,
                home,
                question=case["question"],
                gold_answer=case["gold_answer"],
                candidate=answer_exec.response,
            )
            costs = llm_cost_breakdown(harness, answer_exec, judge_in, judge_out)
            results.append(
                {
                    "system": "aimee",
                    "track": "llm",
                    "git_commit": git_commit(),
                    "question_id": case["question_id"],
                    "subset": case["subset"],
                    "question": case["question"],
                    "gold_answer": case["gold_answer"],
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

    summary = build_summary(results, label_field="subset", include_llm=True, dataset="longmemeval")
    payload = {
        "dataset": "longmemeval",
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
            max_samples=args.max_cases,
            samples_run=samples_run,
            questions_run=len(results),
        ),
        "results": results,
        "summary": summary,
    }
    write_result_file(Path(args.output), payload)
    print_summary("longmemeval", "llm", summary, "subset")
    return 0


def store_case(harness: AimeeHarness, home: Path, case: dict) -> None:
    for session in case["sessions"]:
        date_prefix = f"[{session['date_time']}] " if session.get("date_time") else ""
        for index, turn in enumerate(session["turns"], start=1):
            role = str(turn.get("role", "speaker"))
            text = str(turn.get("content", "")).strip()
            if not text:
                continue
            harness.store_memory(
                home,
                key=f"{session['session_id']}#{index}",
                content=f"{date_prefix}{role}: {text}",
                session=str(session["session_id"]),
            )


if __name__ == "__main__":
    raise SystemExit(main())
