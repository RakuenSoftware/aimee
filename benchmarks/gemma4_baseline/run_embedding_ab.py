#!/usr/bin/env python3
"""Run the frozen 10k paired retrieval suite against one embedding endpoint.

The model is scored at its native output width. If that width exceeds aimee's
4,000-dimension ceiling, an explicitly labelled, untrained prefix-4k diagnostic
is also reported; that diagnostic is not evidence of Matryoshka support.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

import numpy as np


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    with path.open("r", encoding="utf-8") as handle:
        return [json.loads(line) for line in handle if line.strip()]


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    return float(np.percentile(np.asarray(values), p * 100))


def request_embeddings(endpoint: str, model: str, texts: list[str], timeout: int, gateway_batch: bool) -> tuple[np.ndarray, dict[str, Any]]:
    request_body: Any = texts if gateway_batch else {"model": model, "input": texts, "encoding_format": "float"}
    body = json.dumps(request_body).encode()
    request = urllib.request.Request(
        endpoint.rstrip("/") + ("/embed_batch" if gateway_batch else "/v1/embeddings"),
        data=body,
        headers={"Content-Type": "application/json"},
    )
    last_error: Exception | None = None
    for attempt in range(3):
        started = time.perf_counter()
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                payload = json.load(response)
            latency = time.perf_counter() - started
            if gateway_batch:
                vectors = np.asarray(payload, dtype=np.float32)
                usage: dict[str, Any] = {}
            else:
                ordered = sorted(payload["data"], key=lambda item: int(item["index"]))
                vectors = np.asarray([item["embedding"] for item in ordered], dtype=np.float32)
                usage = payload.get("usage", {})
            if vectors.shape[0] != len(texts) or vectors.ndim != 2:
                raise RuntimeError(f"unexpected embedding response shape {vectors.shape}")
            if not np.isfinite(vectors).all():
                raise RuntimeError("embedding response contains a non-finite value")
            return vectors, {"latency_s": latency, "usage": usage, "attempts": attempt + 1}
        except (OSError, KeyError, ValueError, urllib.error.URLError) as exc:
            last_error = exc
            if attempt < 2:
                time.sleep(2**attempt)
    assert last_error is not None
    raise last_error


def normalize(vectors: np.ndarray) -> np.ndarray:
    norms = np.linalg.norm(vectors, axis=1, keepdims=True)
    if np.any(norms == 0):
        raise RuntimeError("embedding endpoint returned a zero vector")
    return vectors / norms


def embed_document_cache(
    endpoint: str,
    model: str,
    ids: list[str],
    corpus: dict[str, str],
    output_dir: Path,
    label: str,
    batch_size: int,
    timeout: int,
    gateway_batch: bool,
) -> tuple[np.ndarray, list[dict[str, Any]]]:
    matrix_path = output_dir / f"doc_vectors_{label}.npy"
    ids_path = output_dir / f"doc_ids_{label}.json"
    if matrix_path.exists() and ids_path.exists():
        cached_ids = json.loads(ids_path.read_text(encoding="utf-8"))
        if cached_ids != ids:
            raise RuntimeError("document embedding cache IDs do not match the frozen suite")
        return np.load(matrix_path), []
    batches: list[np.ndarray] = []
    telemetry: list[dict[str, Any]] = []
    for offset in range(0, len(ids), batch_size):
        batch_ids = ids[offset : offset + batch_size]
        vectors, timing = request_embeddings(endpoint, model, [corpus[doc_id] for doc_id in batch_ids], timeout, gateway_batch)
        batches.append(vectors)
        telemetry.append(timing)
        if len(telemetry) % 100 == 0:
            print(f"{label}: embedded {min(offset + batch_size, len(ids))}/{len(ids)} documents", flush=True)
    matrix = np.concatenate(batches, axis=0)
    np.save(matrix_path, matrix)
    ids_path.write_text(json.dumps(ids) + "\n", encoding="utf-8")
    return matrix, telemetry


def score_rank(ranked_ids: list[str], positive_ids: set[str]) -> dict[str, float | int]:
    rank = next((index for index, doc_id in enumerate(ranked_ids, 1) if doc_id in positive_ids), len(ranked_ids) + 1)
    return {
        "positive_rank": rank,
        "recall_at_1": float(rank <= 1),
        "recall_at_5": float(rank <= 5),
        "recall_at_10": float(rank <= 10),
        "mrr_at_10": 1.0 / rank if rank <= 10 else 0.0,
        "ndcg_at_10": 1.0 / math.log2(rank + 1) if rank <= 10 else 0.0,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--bundle", type=Path, default=Path("benchmarks/fixtures/gemma4-unified/ab-v1"))
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--batch-size", type=int, default=16)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--max-cases", type=int, default=0)
    parser.add_argument("--gateway-batch", action="store_true", help="Use aimee's /embed_batch adapter instead of OpenAI /v1/embeddings")
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    corpus = {row["doc_id"]: row["content"] for row in load_jsonl(args.bundle / "corpus.jsonl")}
    embedding_cases = {row["case_id"]: row for row in load_jsonl(args.bundle / "embedding.jsonl")}
    rerank_cases = load_jsonl(args.bundle / "reranking.jsonl")
    if args.max_cases:
        rerank_cases = rerank_cases[: args.max_cases]
    candidate_ids = sorted({doc_id for case in rerank_cases for doc_id in case["candidate_doc_ids"]})
    missing = set(candidate_ids) - set(corpus)
    if missing:
        raise RuntimeError(f"{len(missing)} candidate documents are absent from corpus")
    document_vectors, doc_telemetry = embed_document_cache(
        args.endpoint, args.model, candidate_ids, corpus, args.output_dir, args.label, args.batch_size, args.timeout, args.gateway_batch
    )
    native_width = int(document_vectors.shape[1])
    widths = [native_width]
    if native_width > 4000:
        widths.append(4000)
    width_labels = {native_width: "native", 4000: "untrained_prefix_4000"}
    doc_index = {doc_id: index for index, doc_id in enumerate(candidate_ids)}
    normalized_docs = {width: normalize(document_vectors[:, :width]) for width in widths}

    raw_path = args.output_dir / f"raw_embedding_{args.label}.jsonl"
    done = {row["case_id"]: row for row in load_jsonl(raw_path)} if raw_path.exists() else {}
    query_telemetry: list[dict[str, Any]] = []
    pending = [case for case in rerank_cases if case["case_id"] not in done]
    with raw_path.open("a", encoding="utf-8", newline="\n") as handle:
        for offset in range(0, len(pending), args.batch_size):
            batch = pending[offset : offset + args.batch_size]
            vectors, timing = request_embeddings(
                args.endpoint, args.model, [embedding_cases[case["case_id"]]["query"] for case in batch], args.timeout, args.gateway_batch
            )
            query_telemetry.append(timing)
            for case, query_vector in zip(batch, vectors, strict=True):
                positive_ids = set(embedding_cases[case["case_id"]]["positive_doc_ids"])
                row: dict[str, Any] = {"case_id": case["case_id"], "metrics": {}}
                for width in widths:
                    query = normalize(query_vector[None, :width])[0]
                    ranked = sorted(
                        case["candidate_doc_ids"],
                        key=lambda doc_id: (-float(normalized_docs[width][doc_index[doc_id]] @ query), doc_id),
                    )
                    row["metrics"][width_labels[width]] = score_rank(ranked, positive_ids)
                done[case["case_id"]] = row
                handle.write(json.dumps(row, sort_keys=True) + "\n")
            handle.flush()
            if (offset // args.batch_size + 1) % 50 == 0:
                print(f"{args.label}: scored {len(done)}/{len(rerank_cases)} queries", flush=True)

    rows = [done[case["case_id"]] for case in rerank_cases]
    summary: dict[str, Any] = {
        "label": args.label,
        "model": args.model,
        "suite_manifest_sha256": hashlib.sha256((args.bundle / "manifest.json").read_bytes()).hexdigest(),
        "cases": len(rows),
        "candidate_documents": len(candidate_ids),
        "native_dimensions": native_width,
        "dimensions": {},
        "telemetry": {
            "document_batches": len(doc_telemetry),
            "query_batches": len(query_telemetry),
            "request_latency_s": {
                "p50": percentile([x["latency_s"] for x in doc_telemetry + query_telemetry], 0.50),
                "p95": percentile([x["latency_s"] for x in doc_telemetry + query_telemetry], 0.95),
            },
            "vectors_per_second": (
                (len(candidate_ids) + len(rows)) / sum(x["latency_s"] for x in doc_telemetry + query_telemetry)
                if doc_telemetry or query_telemetry else 0.0
            ),
        },
    }
    for width in widths:
        name = width_labels[width]
        metrics = [row["metrics"][name] for row in rows]
        summary["dimensions"][name] = {
            "dimensions": width,
            "projection": "none" if width == native_width else "raw_prefix_then_l2_normalize_not_trained_or_claimed_mrl",
            **{key: float(np.mean([row[key] for row in metrics])) for key in ("recall_at_1", "recall_at_5", "recall_at_10", "mrr_at_10", "ndcg_at_10")},
        }
    (args.output_dir / f"summary_embedding_{args.label}.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
