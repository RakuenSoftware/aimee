#!/usr/bin/env python3
"""LLM-judge track driver for stdio adapter targets (model_only, small_agent, …).

run-llm.sh originally wired only the `aimee` target, which needs the memory
store/search path (a running server + vector DB). This driver runs the LLM-judge
track for the adapter targets instead: it drives the target's stdio adapter to
produce an answer, then grades that answer with the configured judge via
`judge_majority` (3 votes). The judge — and, for model_only, the answer itself —
runs through `aimee delegate execute`, so no GPU or pinned judge is required:
the configured execute-role delegate is the judge.

It emits the same LLM result schema the aimee bench scripts produce, so
verify_scores.py / compare_targets.py consume it unchanged.

Usage:
    python3 benchmarks/common/adapter_llm.py \
        --target model_only --bench longmemeval_s \
        --dataset data/longmemeval/longmemeval_s_cleaned.json \
        --output benchmarks/results/longmemeval_s_model_only_llm_vSHA.json \
        --max-samples 2
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterator

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from benchmarks.common.harness import AimeeHarness, git_commit
from benchmarks.common.llm_eval import judge_majority, llm_cost_breakdown
from benchmarks.common.result_schema import make_coverage
from benchmarks.common.runner import build_summary, print_summary, write_result_file

_BENCH_META = {
    "locomo": {"loader": "benchmarks.locomo.common.dataset", "label": "category", "dataset_name": "locomo"},
    "longmemeval_s": {"loader": "benchmarks.longmemeval.common.dataset", "label": "subset", "dataset_name": "longmemeval"},
    "longmemeval_m": {"loader": "benchmarks.longmemeval.common.dataset", "label": "subset", "dataset_name": "longmemeval"},
    "longmemeval_l": {"loader": "benchmarks.longmemeval.common.dataset", "label": "subset", "dataset_name": "longmemeval"},
}


def _load_cases(bench: str, dataset: str, limit: int) -> list[dict[str, Any]]:
    import importlib

    mod = importlib.import_module(_BENCH_META[bench]["loader"])
    return mod.load_cases(dataset, limit)


def _flatten_events(sample: dict[str, Any], bench: str) -> list[dict[str, str]]:
    """Flatten a sample's sessions into adapter ingest events."""
    events: list[dict[str, str]] = []
    for session in sample.get("sessions", []):
        prefix = f"[{session['date_time']}] " if session.get("date_time") else ""
        for i, turn in enumerate(session.get("turns", []), start=1):
            if bench == "locomo":
                role, text = str(turn.get("speaker", "")), str(turn.get("text", "")).strip()
                key = str(turn.get("dia_id") or f"{session.get('name', 's')}#{i}")
            else:
                role, text = str(turn.get("role", "")), str(turn.get("content", "")).strip()
                key = f"{session.get('session_id', 's')}#{i}"
            if text:
                events.append({"key": key, "content": f"{prefix}{role}: {text}"})
    return events


def _questions(sample: dict[str, Any], bench: str, max_questions: int = 0) -> Iterator[dict[str, Any]]:
    if bench == "locomo":
        # A single LoCoMo conversation carries ~100+ questions; without a cap a
        # one-sample run would issue 100s of (slow) delegate calls. max_questions
        # bounds the questions answered per sample (0 = all).
        rows = sample.get("questions", [])
        if max_questions > 0:
            rows = rows[:max_questions]
        for row in rows:
            yield {"question_id": row["question_id"], "question": row["question"],
                   "gold_answer": row["gold_answer"], "label": row["category"]}
    else:
        yield {"question_id": sample["question_id"], "question": sample["question"],
               "gold_answer": sample["gold_answer"], "label": sample["subset"]}


class _Adapter:
    """Thin stdio JSON-lines client for a target adapter subprocess."""

    def __init__(self, script: Path) -> None:
        self._proc = subprocess.Popen(
            ["python3", str(script)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
        )

    def call(self, req: dict[str, Any]) -> dict[str, Any]:
        assert self._proc.stdin and self._proc.stdout
        self._proc.stdin.write(json.dumps(req) + "\n")
        self._proc.stdin.flush()
        return json.loads(self._proc.stdout.readline())

    def close(self) -> None:
        try:
            self.call({"op": "shutdown"})
        except Exception:
            pass
        # Close stdin so the adapter's `for line in sys.stdin` loop ends, then
        # reap it; kill if it does not exit promptly.
        try:
            if self._proc.stdin:
                self._proc.stdin.close()
            self._proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self._proc.kill()
            self._proc.wait(timeout=5)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--target", required=True)
    p.add_argument("--bench", required=True, choices=sorted(_BENCH_META))
    p.add_argument("--dataset", required=True)
    p.add_argument("--output", required=True)
    p.add_argument("--max-samples", type=int, default=0)
    p.add_argument("--max-questions", type=int, default=0,
                   help="cap questions answered per sample (0 = all; matters for LoCoMo)")
    return p


def main() -> int:
    args = build_parser().parse_args()
    meta = _BENCH_META[args.bench]
    root = Path(__file__).resolve().parents[2]
    adapter_script = root / "benchmarks" / "targets" / args.target / "adapter.py"
    if not adapter_script.exists():
        print(f"adapter not found: {adapter_script}", file=sys.stderr)
        return 1

    harness = AimeeHarness(root=root)
    adapter = _Adapter(adapter_script)
    results: list[dict[str, Any]] = []
    try:
        adapter.call({"op": "describe"})
        samples_run = 0
        for sample in _load_cases(args.bench, args.dataset, args.max_samples):
            samples_run += 1
            events = _flatten_events(sample, args.bench)
            ingest = adapter.call({"op": "ingest", "session_id": sample.get("id", "bench"), "events": events})
            state_ref = ingest.get("state_ref", "")
            for q in _questions(sample, args.bench, args.max_questions):
                ans = adapter.call({"op": "answer", "state_ref": state_ref,
                                    "question": q["question"], "budget": {"max_tokens": 512}})
                candidate = str(ans.get("answer", ""))
                answer_latency_s = float(ans.get("latency_ms", 0)) / 1000.0
                votes, judge_latency_s, judge_in, judge_out, verdict = judge_majority(
                    harness, harness.source_home,
                    question=q["question"], gold_answer=q["gold_answer"], candidate=candidate,
                )
                answer_in = len(q["question"]) // 4
                answer_out = max(1, len(candidate) // 4)

                class _Ex:  # minimal AgentExecution stand-in for cost estimate
                    prompt_tokens, completion_tokens = answer_in, answer_out
                costs = llm_cost_breakdown(harness, _Ex(), judge_in, judge_out)

                row: dict[str, Any] = {
                    "system": args.target,
                    "track": "llm",
                    "git_commit": git_commit(root),
                    "question_id": q["question_id"],
                    meta["label"]: q["label"],
                    "question": q["question"],
                    "gold_answer": q["gold_answer"],
                    "generated_answer": candidate,
                    "judge_votes": votes,
                    "verdict": verdict,
                    "retrieval_latency_s": 0.0,
                    "answer_latency_s": round(answer_latency_s, 6),
                    "judge_latency_s": round(judge_latency_s, 6),
                    "wall_clock_s": round(answer_latency_s + judge_latency_s, 6),
                    "retrieved_ids": [],
                    "citations": [],
                    "tokens": {"answer_in": answer_in, "answer_out": answer_out,
                               "judge_in": judge_in, "judge_out": judge_out},
                    "cost": costs,
                }
                results.append(row)
    finally:
        adapter.close()

    summary = build_summary(results, label_field=meta["label"], include_llm=True, dataset=meta["dataset_name"])
    payload = {
        "dataset": meta["dataset_name"],
        "system": args.target,
        "track": "llm",
        "git_commit": git_commit(root),
        "result_count": len(results),
        # Records whether this run was capped, so a subsample cannot later be
        # compared against a full-run baseline without the difference showing.
        # See require_complete_run in benchmarks/common/result_schema.py.
        "coverage": make_coverage(
            max_samples=args.max_samples,
            max_questions=args.max_questions,
            samples_run=samples_run,
            questions_run=len(results),
        ),
        "agent_model": harness.current_model,
        "judge_runs": 3,
        "judge_profile": "frontier",
        "results": results,
        "summary": summary,
    }
    write_result_file(Path(args.output), payload)
    print_summary(meta["dataset_name"], "llm", summary, meta["label"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
