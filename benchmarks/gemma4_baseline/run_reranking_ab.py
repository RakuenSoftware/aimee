#!/usr/bin/env python3
"""Run the frozen 10k fixed-candidate reranking suite against one provider."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import statistics
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    with path.open("r", encoding="utf-8") as handle:
        return [json.loads(line) for line in handle if line.strip()]


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    point = (len(ordered) - 1) * p
    low, high = math.floor(point), math.ceil(point)
    return ordered[low] if low == high else ordered[low] * (high - point) + ordered[high] * (point - low)


def ranking_metrics(ranked_ids: list[str], relevance: dict[str, int]) -> dict[str, float | int]:
    relevant = {doc_id for doc_id, value in relevance.items() if value > 0}
    rank = next((index for index, doc_id in enumerate(ranked_ids, 1) if doc_id in relevant), len(ranked_ids) + 1)
    return {
        "positive_rank": rank,
        "recall_at_1": float(rank <= 1),
        "recall_at_5": float(rank <= 5),
        "recall_at_10": float(rank <= 10),
        "mrr_at_10": 1.0 / rank if rank <= 10 else 0.0,
        "ndcg_at_10": 1.0 / math.log2(rank + 1) if rank <= 10 else 0.0,
    }


def bounded_text(value: str, limit: int) -> str:
    """Model-agnostic deterministic bound for common 512-token reranker contexts."""
    if len(value) <= limit:
        return value
    marker = "\n[...truncated...]\n"
    available = limit - len(marker)
    head = available * 2 // 3
    return value[:head] + marker + value[-(available - head) :]


def call(
    endpoint: str,
    case: dict[str, Any],
    corpus: dict[str, str],
    timeout: int,
    pair_batch_size: int,
    query_char_cap: int,
    candidate_char_cap: int,
) -> dict[str, Any]:
    query = bounded_text(case["query"], query_char_cap)
    pairs = [[query, bounded_text(corpus[doc_id], candidate_char_cap)] for doc_id in case["candidate_doc_ids"]]
    last_error = ""
    scores: list[float] = []
    latency = 0.0
    attempts_total = 0
    try:
        for offset in range(0, len(pairs), pair_batch_size):
            chunk = pairs[offset : offset + pair_batch_size]
            encoded = json.dumps(chunk).encode()
            chunk_scores: list[float] | None = None
            for attempt in range(3):
                attempts_total += 1
                started = time.perf_counter()
                try:
                    request = urllib.request.Request(
                        endpoint.rstrip("/") + "/rerank", data=encoded, headers={"Content-Type": "application/json"}
                    )
                    with urllib.request.urlopen(request, timeout=timeout) as response:
                        value = json.load(response)
                    latency += time.perf_counter() - started
                    if not isinstance(value, list) or len(value) != len(chunk):
                        raise RuntimeError(f"expected {len(chunk)} aligned scores")
                    if not all(isinstance(item, (int, float)) and math.isfinite(item) for item in value):
                        raise RuntimeError("provider returned a non-finite reranking score")
                    chunk_scores = [float(item) for item in value]
                    break
                except (OSError, ValueError, RuntimeError, urllib.error.URLError) as exc:
                    last_error = f"{type(exc).__name__}: {exc}"
                    if attempt < 2:
                        time.sleep(2**attempt)
            if chunk_scores is None:
                raise RuntimeError(last_error)
            scores.extend(chunk_scores)
        if len(scores) != len(pairs):
            raise RuntimeError(f"expected {len(pairs)} aligned scores, received {len(scores)}")
        ranked_ids = sorted(
            case["candidate_doc_ids"],
            key=lambda doc_id: (-float(scores[case["candidate_doc_ids"].index(doc_id)]), doc_id),
        )
        return {
            "case_id": case["case_id"], "ok": True, "attempts": attempts_total, "latency_s": latency,
            "request_chunks": math.ceil(len(pairs) / pair_batch_size),
            "scores": scores, "ranked_doc_ids": ranked_ids,
            "metrics": ranking_metrics(ranked_ids, case["relevance"]),
        }
    except (OSError, ValueError, RuntimeError, urllib.error.URLError) as exc:
        last_error = f"{type(exc).__name__}: {exc}"
        return {
            "case_id": case["case_id"], "ok": False, "attempts": attempts_total, "latency_s": latency, "error": last_error,
            "request_chunks": math.ceil(len(pairs) / pair_batch_size), "scores": [], "ranked_doc_ids": [],
            "metrics": {key: 0.0 for key in ("recall_at_1", "recall_at_5", "recall_at_10", "mrr_at_10", "ndcg_at_10")},
        }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--endpoint", required=True, help="aimee-compatible base URL exposing POST /rerank")
    parser.add_argument("--label", required=True)
    parser.add_argument("--bundle", type=Path, default=Path("benchmarks/fixtures/gemma4-unified/ab-v1"))
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--pair-batch-size", type=int, default=4)
    parser.add_argument("--query-char-cap", type=int, default=512)
    parser.add_argument("--candidate-char-cap", type=int, default=1024)
    parser.add_argument("--max-cases", type=int, default=0)
    parser.add_argument("--environment-note", default="")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    corpus = {row["doc_id"]: row["content"] for row in load_jsonl(args.bundle / "corpus.jsonl")}
    cases = load_jsonl(args.bundle / "reranking.jsonl")
    if args.max_cases:
        cases = cases[: args.max_cases]
    raw_path = args.output_dir / f"raw_reranking_{args.label}.jsonl"
    done = {row["case_id"]: row for row in load_jsonl(raw_path)} if raw_path.exists() else {}
    with raw_path.open("a", encoding="utf-8", newline="\n") as handle:
        for index, case in enumerate((case for case in cases if case["case_id"] not in done), 1):
            row = call(
                args.endpoint, case, corpus, args.timeout, args.pair_batch_size,
                args.query_char_cap, args.candidate_char_cap,
            )
            done[case["case_id"]] = row
            handle.write(json.dumps(row, sort_keys=True) + "\n")
            handle.flush()
            if index % 100 == 0:
                print(f"{args.label}: {len(done)}/{len(cases)}", flush=True)
    rows = [done[case["case_id"]] for case in cases]
    latencies = [float(row["latency_s"]) for row in rows if row["ok"]]
    names = ("recall_at_1", "recall_at_5", "recall_at_10", "mrr_at_10", "ndcg_at_10")
    summary = {
        "label": args.label,
        "suite_manifest_sha256": hashlib.sha256((args.bundle / "manifest.json").read_bytes()).hexdigest(),
        "cases": len(rows),
        "candidates_per_case": 20,
        "input_bounds": {
            "query_chars": args.query_char_cap,
            "candidate_chars": args.candidate_char_cap,
            "method": "utf8_head_2_over_3_tail_1_over_3_with_marker",
        },
        "success_rate": sum(bool(row["ok"]) for row in rows) / max(1, len(rows)),
        **{name: statistics.fmean(float(row["metrics"][name]) for row in rows) for name in names},
        "latency_s": {"p50": percentile(latencies, 0.50), "p95": percentile(latencies, 0.95), "p99": percentile(latencies, 0.99)},
        "requests_retried": sum(max(0, int(row["attempts"]) - int(row.get("request_chunks", 1))) for row in rows),
        "environment_note": args.environment_note,
    }
    (args.output_dir / f"summary_reranking_{args.label}.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary["success_rate"] == 1.0 else 1


if __name__ == "__main__":
    sys.exit(main())
