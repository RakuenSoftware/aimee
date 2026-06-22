#!/usr/bin/env python3
"""embedder_gate.py — isolated embedder retrieval-quality probe on LoCoMo.

A precursor to the full ship-floor gate (unified-llm-container §"acceptance
gates"): it measures *embedder* retrieval quality in isolation (no reranker, no
fusion, no LLM) so a candidate embedder can be screened before the heavier
aimee-pipeline sweep. Any OpenAI-compatible `/v1/embeddings` endpoint works, so a
llama.cpp+Vulkan server (e.g. on the 7900XTX) is a drop-in candidate.

Task (mirrors benchmarks/locomo/bench_aimee_direct.py semantics): for each
conversation the corpus is its turns (`dia_id` -> "speaker: text"); each `qa`
question retrieves over those turns by cosine; a turn is relevant iff its
`dia_id` is in the question's `evidence`. We report Recall@5, Recall@10 and MRR
averaged over all questions — the same metrics the gate uses.

Usage:
  python3 embedder_gate.py --dataset locomo10.json --endpoint http://localhost:8899/v1/embeddings \
      --name qwen3-0.6b --output results_qwen3.json [--max-conversations N] [--query-prefix STR]
"""
from __future__ import annotations

import argparse
import json
import math
import time
import urllib.request
from pathlib import Path


def load_locomo(path: str, max_conversations: int = 0):
    """Yield (conversation_id, turns, questions). turns: list[(dia_id, text)];
    questions: list[(question, set(evidence_dia_ids))]. Replicated from
    benchmarks/locomo/common/dataset.py so the probe is self-contained."""
    root = json.loads(Path(path).read_text())
    out = []
    for i, sample in enumerate(root):
        if max_conversations and i >= max_conversations:
            break
        conv = sample.get("conversation", {})
        turns = []
        for key, val in conv.items():
            if not key.startswith("session_") or key.endswith("date_time") or not isinstance(val, list):
                continue
            for t in val:
                did = t.get("dia_id")
                txt = t.get("text", "")
                if did and txt:
                    turns.append((did, f"{t.get('speaker', '')}: {txt}".strip()))
        questions = []
        for item in sample.get("qa", []):
            q = str(item.get("question", "")).strip()
            ev = {str(e) for e in item.get("evidence", []) if isinstance(e, str)}
            if q and ev:  # adversarial/unanswerable rows carry no evidence -> skip
                questions.append((q, ev))
        if turns and questions:
            out.append((str(sample.get("id") or f"conv-{i+1}"), turns, questions))
    return out


def embed(endpoint: str, texts: list[str], batch: int = 256, prefix: str = "",
          normalize: bool = True) -> list[list[float]]:
    """Embed via an OpenAI-compatible /v1/embeddings endpoint. By default vectors
    are L2-normalized so the dot product in evaluate() is exactly cosine
    similarity (normalize-then-dot == cosine); --no-normalize disables it for
    endpoints that already return unit vectors or where raw dot is wanted."""
    vecs: list[list[float]] = []
    for i in range(0, len(texts), batch):
        chunk = [prefix + t for t in texts[i : i + batch]]
        body = json.dumps({"input": chunk}).encode()
        req = urllib.request.Request(
            endpoint, data=body, headers={"Content-Type": "application/json"}
        )
        for attempt in range(4):
            try:
                with urllib.request.urlopen(req, timeout=600) as r:
                    data = json.loads(r.read())["data"]
                break
            except Exception:  # noqa: BLE001 — transient server/load errors, retried
                if attempt == 3:
                    raise
                time.sleep(2 * (attempt + 1))
        for row in sorted(data, key=lambda d: d["index"]):
            v = row["embedding"]
            if normalize:
                n = math.sqrt(sum(x * x for x in v)) or 1.0
                v = [x / n for x in v]
            vecs.append(v)
    return vecs


try:
    import numpy as _np
except ImportError:  # pure-python fallback (slow at high dim, but dependency-free)
    _np = None


def _rank_dia_ids(qv, doc_vecs, dia_ids):
    """Return doc dia_ids ranked by descending cosine to query vector qv."""
    if _np is not None:
        order = _np.asarray(doc_vecs).dot(_np.asarray(qv)).argsort()[::-1]
        return [dia_ids[i] for i in order]
    scored = sorted(
        ((sum(a * b for a, b in zip(qv, dv)), did) for dv, did in zip(doc_vecs, dia_ids)),
        key=lambda x: x[0],
        reverse=True,
    )
    return [did for _, did in scored]


def evaluate(cases, endpoint: str, query_prefix: str = "", doc_prefix: str = "", normalize: bool = True):
    rows = []
    for cid, turns, questions in cases:
        dia_ids = [d for d, _ in turns]
        doc_vecs = embed(endpoint, [t for _, t in turns], prefix=doc_prefix, normalize=normalize)
        q_vecs = embed(endpoint, [qt for qt, _ in questions], prefix=query_prefix, normalize=normalize)
        for (_q, evidence), qv in zip(questions, q_vecs):
            ranked = _rank_dia_ids(qv, doc_vecs, dia_ids)
            rank = next((i + 1 for i, did in enumerate(ranked) if did in evidence), 0)
            rows.append(
                {
                    "conversation_id": cid,
                    "recall_5": 1.0 if any(d in evidence for d in ranked[:5]) else 0.0,
                    "recall_10": 1.0 if any(d in evidence for d in ranked[:10]) else 0.0,
                    "mrr": (1.0 / rank) if rank else 0.0,
                }
            )
    n = len(rows) or 1
    return {
        "questions": len(rows),
        "conversations": len(cases),
        "recall_5": sum(r["recall_5"] for r in rows) / n,
        "recall_10": sum(r["recall_10"] for r in rows) / n,
        "mrr": sum(r["mrr"] for r in rows) / n,
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dataset", required=True)
    ap.add_argument("--endpoint", required=True, help="OpenAI-compatible /v1/embeddings URL")
    ap.add_argument("--name", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--max-conversations", type=int, default=0)
    ap.add_argument("--query-prefix", default="", help="instruction prefix for queries (some models)")
    ap.add_argument("--doc-prefix", default="", help="instruction prefix for documents (some models)")
    ap.add_argument("--query-prefix-file", help="read the query prefix from a file (preserves "
                    "embedded newlines, e.g. Qwen3's 'Instruct: ...\\nQuery: ')")
    ap.add_argument("--no-normalize", action="store_true",
                    help="do not L2-normalize (endpoint already returns unit vectors)")
    args = ap.parse_args()

    query_prefix = Path(args.query_prefix_file).read_text() if args.query_prefix_file else args.query_prefix

    cases = load_locomo(args.dataset, args.max_conversations)
    t0 = time.time()
    metrics = evaluate(cases, args.endpoint, query_prefix, args.doc_prefix, normalize=not args.no_normalize)
    metrics["name"] = args.name
    metrics["endpoint"] = args.endpoint
    # Persist the exact recipe so the artifact is a fair, reproducible comparison
    # (each model uses its own prefix; three recipes, one annotated artifact).
    metrics["query_prefix"] = query_prefix
    metrics["doc_prefix"] = args.doc_prefix
    metrics["normalized"] = not args.no_normalize
    metrics["wall_seconds"] = round(time.time() - t0, 1)
    Path(args.output).write_text(json.dumps(metrics, indent=2))
    print(
        f"{args.name}: Recall@5={metrics['recall_5']:.4f} Recall@10={metrics['recall_10']:.4f} "
        f"MRR={metrics['mrr']:.4f}  ({metrics['questions']} q / {metrics['conversations']} conv, "
        f"qprefix={query_prefix!r}, {metrics['wall_seconds']}s)"
    )


if __name__ == "__main__":
    main()
