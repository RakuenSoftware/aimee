#!/usr/bin/env python3
"""Optional Mem0 LoCoMo baseline.

Requires MEM0_API_KEY to be set in the environment and:
    pip install mem0ai

Skips cleanly with a clear message when either is absent.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

from benchmarks.common.harness import AimeeHarness, git_commit
from benchmarks.common.llm_eval import ANSWER_SYSTEM, build_answer_prompt, judge_majority, llm_cost_breakdown
from benchmarks.common.runner import build_summary, print_summary, write_result_file
from benchmarks.locomo.common.dataset import load_cases

SYSTEM_NAME = "mem0"


def _check_prerequisites():
    """Return the mem0 MemoryClient, or exit with a clear message."""
    api_key = os.environ.get("MEM0_API_KEY", "")
    if not api_key:
        print(
            f"[{SYSTEM_NAME}] skipping — MEM0_API_KEY is not set.\n"
            "  Set the environment variable to enable this baseline.",
            file=sys.stderr,
        )
        sys.exit(0)

    try:
        from mem0 import MemoryClient  # noqa: PLC0415
        return MemoryClient(api_key=api_key)
    except ImportError as exc:
        print(
            f"[{SYSTEM_NAME}] skipping — missing dependency: {exc}\n"
            "  Install with: pip install mem0ai",
            file=sys.stderr,
        )
        sys.exit(0)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--top-k", type=int, default=10)
    parser.add_argument("--max-samples", type=int, default=0)
    parser.add_argument("--output", required=True)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    cases = load_cases(args.dataset, args.max_samples)
    mem0 = _check_prerequisites()

    harness = AimeeHarness()
    results = []
    tmp, home = harness.prepare_home()
    try:
        for sample in cases:
            # Ingest conversation turns into Mem0
            user_id = f"bench_{sample.get('conv_id', 'x')}"
            messages = []
            for session in sample["sessions"]:
                date_prefix = f"[{session['date_time']}] " if session.get("date_time") else ""
                for turn in session["turns"]:
                    speaker = str(turn.get("speaker", "speaker"))
                    text = str(turn.get("text", "")).strip()
                    if text:
                        messages.append({"role": "user", "content": f"{date_prefix}{speaker}: {text}"})
            if messages:
                mem0.add(messages, user_id=user_id)

            for row in sample["questions"]:
                started = time.perf_counter()
                mem_results = mem0.search(row["question"], user_id=user_id, limit=args.top_k)
                retrieval_latency_s = time.perf_counter() - started

                retrieved_ids = [m.get("id", "") for m in mem_results]
                context = "\n".join(
                    f"[{idx + 1}] {m.get('memory', '')}"
                    for idx, m in enumerate(mem_results)
                )
                prompt = build_answer_prompt(row["question"], context)
                answer_exec = harness.agent_run(home, prompt=prompt, system=ANSWER_SYSTEM, max_tokens=256)
                votes, judge_latency_s, judge_in, judge_out, verdict = judge_majority(
                    harness,
                    home,
                    question=row["question"],
                    gold_answer=row["gold_answer"],
                    answerable=row["answerable"],
                    candidate=answer_exec.response,
                )
                costs = llm_cost_breakdown(harness, answer_exec, judge_in, judge_out)
                results.append(
                    {
                        "system": SYSTEM_NAME,
                        "track": "llm",
                        "git_commit": git_commit(),
                        "question_id": row["question_id"],
                        "category": row["category"],
                        "question": row["question"],
                        "gold_answer": row["gold_answer"],
                    "answerable": row["answerable"],
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
                        "citations": [
                            {"node_id": rid, "relation": "mem0"} for rid in retrieved_ids[:5]
                        ],
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
        "dataset_inventory": cases.inventory,
        "dataset": "locomo",
        "system": SYSTEM_NAME,
        "system_version": "mem0-api",
        "track": "llm",
        "git_commit": git_commit(),
        "result_count": len(results),
        "agent_model": harness.current_model,
        "judge_runs": 3,
        "results": results,
        "summary": summary,
    }
    write_result_file(Path(args.output), payload)
    print_summary("locomo", "llm", summary, "category")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
