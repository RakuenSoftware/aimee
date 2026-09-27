#!/usr/bin/env python3
"""Shared LLM-track helpers for benchmark wrappers."""

from __future__ import annotations

import os
import json
from pathlib import Path

from benchmarks.common.harness import AimeeHarness, AgentExecution, token_estimate_cost


ANSWER_SYSTEM = (
    "Answer benchmark questions using only the provided memory context. "
    "If the context is insufficient, answer Unknown. Respond with only the answer."
)
JUDGE_SYSTEM = (
    "You are grading benchmark answers. Compare the candidate answer to the gold answer for "
    'semantic equivalence. Ignore wording differences. Return JSON only: {"score":1} or {"score":0}.'
)


def build_answer_prompt(question: str, context: str) -> str:
    return (
        f"Question: {question}\n\nRetrieved memory context:\n"
        f"{context or '[no retrieved context]'}\nFinal answer:"
    )


def judge_vote(
    harness: AimeeHarness,
    home: Path,
    *,
    question: str,
    gold_answer: str,
    candidate: str,
    answerable: bool | None = None,
) -> tuple[str, float, int, int]:
    prompt = (
        f"Question: {question}\n"
        f"Gold answer: {gold_answer}\n"
        f"Candidate answer: {candidate}\n\nReturn JSON only."
    )
    if answerable is False:
        prompt = (f"Question: {question}\nThis question is unanswerable from the supplied history. "
                  f"Score 1 only if the candidate explicitly abstains without asserting an answer.\n"
                  f"Candidate answer: {candidate}\nReturn JSON only.")
    # Reasoning delegates (e.g. MiniMax) spend tokens on hidden reasoning before
    # emitting the JSON verdict; a too-small budget yields "no content". Allow a
    # larger judge budget via AIMEE_BENCH_JUDGE_MAX_TOKENS (default 64 preserves
    # prior behaviour for non-reasoning judges).
    judge_max_tokens = int(os.environ.get("AIMEE_BENCH_JUDGE_MAX_TOKENS", "64"))
    result = harness.agent_run(home, prompt=prompt, system=JUDGE_SYSTEM, max_tokens=judge_max_tokens)
    parsed = json.loads(result.response)
    if not isinstance(parsed, dict) or type(parsed.get("score")) is not int or parsed["score"] not in (0, 1):
        raise ValueError("invalid judge score")
    verdict = "CORRECT" if parsed["score"] else "WRONG"
    return verdict, result.latency_s, result.prompt_tokens, result.completion_tokens


def judge_majority(
    harness: AimeeHarness,
    home: Path,
    *,
    question: str,
    gold_answer: str,
    candidate: str,
    answerable: bool | None = None,
) -> tuple[list[str], float, int, int, str]:
    votes = []
    judge_latency_s = 0.0
    judge_in = 0
    judge_out = 0
    for _ in range(3):
        vote, latency_s, prompt_tokens, completion_tokens = judge_vote(
            harness,
            home,
            question=question,
            gold_answer=gold_answer,
            candidate=candidate,
            answerable=answerable,
        )
        votes.append(vote)
        judge_latency_s += latency_s
        judge_in += prompt_tokens
        judge_out += completion_tokens
    verdict = "CORRECT" if votes.count("CORRECT") >= 2 else "WRONG"
    return votes, judge_latency_s, judge_in, judge_out, verdict


def llm_cost_breakdown(harness: AimeeHarness, answer_exec: AgentExecution, judge_in: int, judge_out: int) -> dict[str, float]:
    answer_cost = token_estimate_cost(
        harness.current_model, answer_exec.prompt_tokens, answer_exec.completion_tokens
    )
    judge_cost = token_estimate_cost(harness.current_model, judge_in, judge_out)
    return {
        "answer_usd": answer_cost,
        "judge_usd": judge_cost,
        "total_usd": answer_cost + judge_cost,
    }
