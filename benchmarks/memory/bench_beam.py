#!/usr/bin/env python3
"""BEAM episodic agent-memory benchmark — conversational-memory track (grader=llm)."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


def _load_cases(path: str, max_cases: int) -> list[dict[str, Any]]:
    """Load BEAM JSONL cases into memory.

    Each case may have either:
      - "conversation": list of {speaker, text} turns   (flat list)
      - "sessions":     list of turn-lists              (LOCOMO-style)

    Questions follow the BEAM schema:
      - "question", "gold_answer", [question_id], [category]

    All conversation shapes are normalized to a flat list of turns,
    each with keys "speaker" and "text".
    """
    # BEAM ships as JSONL (one case per line). Tolerate a single JSON array too.
    text = Path(path).read_text()
    samples: list[dict[str, Any]] = []
    stripped = text.lstrip()
    if stripped.startswith("["):
        samples = json.loads(text)
    else:
        for line in text.splitlines():
            line = line.strip()
            if line:
                samples.append(json.loads(line))

    cases = []
    for idx, sample in enumerate(samples):
        if max_cases and idx >= max_cases:
            break

        # Normalize conversation / sessions to a flat list of turns
        raw_conversation = sample.get("conversation")
        raw_sessions = sample.get("sessions")
        turns: list[dict[str, Any]] = []

        if isinstance(raw_conversation, list):
            # Already a flat list of {speaker, text} turns
            for turn in raw_conversation:
                if isinstance(turn, dict):
                    turns.append(
                        {
                            "speaker": str(turn.get("speaker", "speaker")),
                            "text": str(turn.get("text", "")).strip(),
                        }
                    )
        elif isinstance(raw_sessions, list):
            # LOCOMO-style sessions — each entry is a list of turns
            for session in raw_sessions:
                if isinstance(session, list):
                    for turn in session:
                        if isinstance(turn, dict):
                            turns.append(
                                {
                                    "speaker": str(turn.get("speaker", "speaker")),
                                    "text": str(turn.get("text", "")).strip(),
                                }
                            )

        # Normalize questions
        raw_questions = sample.get("questions", [])
        questions: list[dict[str, Any]] = []
        for q_idx, row in enumerate(raw_questions):
            if not isinstance(row, dict):
                continue
            q = str(row.get("question", "")).strip()
            if not q:
                continue
            questions.append(
                {
                    "question_id": str(row.get("question_id", f"beam-{idx + 1}-q{q_idx + 1}")),
                    "question": q,
                    "gold_answer": str(row.get("gold_answer", "")),
                    "category": str(row.get("category", "unknown")),
                }
            )

        # Drop a case only when it carried no question entries at all. A case
        # whose questions were all filtered out (e.g. empty text) is retained
        # with an empty list so its conversation still counts as ingested.
        if raw_questions:
            cases.append(
                {
                    "conversation_id": str(sample.get("id") or sample.get("conversation_id", f"beam-{idx + 1}")),
                    "turns": turns,
                    "questions": questions,
                }
            )
    return cases


# ---------------------------------------------------------------------------
# Helpers mirrored from locomo so the benchmark is self-contained
# ---------------------------------------------------------------------------

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


def _store_turns(harness: AimeeHarness, home: Path, conversation_id: str, turns: list[dict[str, Any]]) -> None:
    """Ingest a flat list of turns into memory using harness.store_memory."""
    for turn_idx, turn in enumerate(turns):
        text = turn.get("text", "").strip()
        if not text:
            continue
        harness.store_memory(
            home,
            key=f"{conversation_id}-t{turn_idx}",
            content=f"{turn.get('speaker', 'speaker')}: {text}",
            session=str(conversation_id),
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

    # Guard: require a real dataset to avoid meaningless runs
    if not Path(args.dataset).exists():
        raise SystemExit(f"ERROR: dataset not found: {args.dataset}")

    harness = AimeeHarness()
    results: list[dict[str, Any]] = []

    samples_run = 0
    for sample in _load_cases(args.dataset, args.max_samples):
        samples_run += 1
        tmp, home = harness.prepare_home()
        try:
            _store_turns(harness, home, sample["conversation_id"], sample["turns"])

            for row in sample["questions"]:
                facts, retrieval_latency_s = harness.search_facts(
                    home, normalize_question(row["question"]), args.top_k
                )
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
        "dataset": "beam",
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
    print_summary("beam", "llm", summary, "category")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
