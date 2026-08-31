#!/usr/bin/env python3

from __future__ import annotations

import argparse
import time
from pathlib import Path

from benchmarks.common.bm25 import BM25Index
from benchmarks.common.harness import AimeeHarness, git_commit
from benchmarks.common.llm_eval import ANSWER_SYSTEM, build_answer_prompt, judge_majority, llm_cost_breakdown
from benchmarks.common.result_schema import make_coverage
from benchmarks.common.runner import build_summary, print_summary, write_result_file
from benchmarks.locomo.common.dataset import load_cases


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--top-k", type=int, default=100)
    parser.add_argument("--max-samples", type=int, default=0)
    parser.add_argument("--output", required=True)
    return parser


def build_documents(sample: dict) -> list[dict[str, str]]:
    docs = []
    for session in sample["sessions"]:
        date_prefix = f"[{session['date_time']}] " if session.get("date_time") else ""
        for turn in session["turns"]:
            speaker = str(turn.get("speaker", "speaker"))
            text = str(turn.get("text", "")).strip()
            if not text:
                continue
            docs.append(
                {
                    "id": str(turn.get("dia_id") or f"{session['name']}-{len(docs) + 1}"),
                    "content": f"{date_prefix}{speaker}: {text}",
                }
            )
    return docs


def main() -> int:
    args = build_parser().parse_args()
    harness = AimeeHarness()
    results = []
    tmp, home = harness.prepare_home()
    try:
        samples_run = 0
        for sample in load_cases(args.dataset, args.max_samples):
            samples_run += 1
            index = BM25Index(build_documents(sample))
            for row in sample["questions"]:
                started = time.perf_counter()
                facts = index.search(row["question"], args.top_k)
                retrieval_latency_s = time.perf_counter() - started
                retrieved_ids = [fact["id"] for fact in facts]
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
                        "system": "bm25",
                        "track": "llm",
                        "git_commit": git_commit(),
                        "question_id": row["question_id"],
                        "category": row["category"],
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
                        "citations": [{"node_id": rid, "relation": "bm25"} for rid in retrieved_ids[:5]],
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
        "dataset": "locomo",
        "system": "bm25",
        "system_version": "stdlib-bm25",
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
    print_summary("locomo", "llm", summary, "category")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
