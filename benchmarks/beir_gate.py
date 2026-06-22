#!/usr/bin/env python3
"""beir_gate.py — standard BEIR retrieval eval for an embedding endpoint.

The LoCoMo screen (embedder_gate.py) ranks short conversational-turn retrieval,
which under-discriminates embedder quality (a 137M model edged a SOTA 8B). This
runs a *standard* BEIR dataset (corpus + queries + qrels) and reports nDCG@10 /
Recall@10 — the metric embedder leaderboards use, where model quality separates.

Any OpenAI-compatible /v1/embeddings endpoint works (llama.cpp+Vulkan included).

BEIR layout (the HF `BeIR/<name>` "corpus"/"queries" configs + qrels TSV):
  corpus.jsonl : {"_id","title","text"}
  queries.jsonl: {"_id","text"}
  qrels.tsv    : query-id <tab> corpus-id <tab> score   (header row skipped)

Usage:
  python3 beir_gate.py --corpus corpus.jsonl --queries queries.jsonl --qrels qrels.tsv \
      --endpoint http://localhost:8899/v1/embeddings --name nomic --output out.json \
      [--query-prefix STR | --query-prefix-file F] [--doc-prefix STR] [--max-docs N]
"""
from __future__ import annotations

import argparse
import json
import math
import time
import urllib.request
from pathlib import Path

try:
    import numpy as np
except ImportError:
    np = None


def _post(endpoint, chunk):
    body = json.dumps({"input": chunk}).encode()
    req = urllib.request.Request(endpoint, data=body, headers={"Content-Type": "application/json"})
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=600) as r:
                data = json.loads(r.read())["data"]
            return [row["embedding"] for row in sorted(data, key=lambda d: d["index"])]
        except Exception:
            if attempt == 3:
                raise
            time.sleep(2 * (attempt + 1))


_skipped = 0


def embed(endpoint, texts, prefix="", batch=128, normalize=True, dim=None, suffix=""):
    """Robust: on a batch error (e.g. one doc too long for the ubatch, or a
    transient drop) fall back to one-at-a-time; a single text that still errors is
    skipped with a zero vector so a few pathological docs cannot kill a long run."""
    global _skipped
    out, d = [], dim
    for i in range(0, len(texts), batch):
        chunk = [prefix + t + suffix for t in texts[i : i + batch]]
        try:
            vecs = _post(endpoint, chunk)
        except Exception:
            vecs = []
            for t in chunk:
                try:
                    vecs.append(_post(endpoint, [t])[0])
                except Exception:
                    _skipped += 1
                    vecs.append(None)
        for v in vecs:
            if v is None:
                out.append([0.0] * (d or 1))
                continue
            d = d or len(v)
            if normalize:
                n = math.sqrt(sum(x * x for x in v)) or 1.0
                v = [x / n for x in v]
            out.append(v)
    return out


def dcg(rels):
    return sum((2 ** r - 1) / math.log2(i + 2) for i, r in enumerate(rels))


def ndcg_at_k(ranked_ids, gold, k=10):
    rels = [gold.get(cid, 0) for cid in ranked_ids[:k]]
    ideal = sorted(gold.values(), reverse=True)[:k]
    idcg = dcg(ideal)
    return (dcg(rels) / idcg) if idcg > 0 else 0.0


def recall_at_k(ranked_ids, gold, k=10):
    rel = {cid for cid, s in gold.items() if s > 0}
    if not rel:
        return 0.0
    return len(rel & set(ranked_ids[:k])) / len(rel)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--queries", required=True)
    ap.add_argument("--qrels", required=True)
    ap.add_argument("--endpoint", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--query-prefix", default="")
    ap.add_argument("--query-prefix-file")
    ap.add_argument("--doc-prefix", default="")
    ap.add_argument("--max-docs", type=int, default=0, help="cap corpus (0=all)")
    ap.add_argument("--no-normalize", action="store_true")
    ap.add_argument("--batch", type=int, default=128)
    ap.add_argument("--suffix", default="", help="appended to every input text (e.g. <|endoftext|> for Qwen3 last-token pooling)")
    ap.add_argument("--max-doc-chars", type=int, default=0, help="truncate each doc to N chars (fair cross-model when models have different max context)")
    args = ap.parse_args()

    qprefix = Path(args.query_prefix_file).read_text() if args.query_prefix_file else args.query_prefix

    # qrels: query_id -> {corpus_id: score}; only judged queries are scored.
    qrels = {}
    for ln, line in enumerate(Path(args.qrels).read_text().splitlines()):
        if ln == 0 and not line[:1].isalnum():
            continue
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        qid, cid, score = parts[0], parts[1], parts[2]
        if ln == 0 and score.lower() in ("score",):
            continue
        try:
            s = int(float(score))
        except ValueError:
            continue
        qrels.setdefault(qid, {})[cid] = s

    corpus = {}
    for line in Path(args.corpus).read_text().splitlines():
        d = json.loads(line)
        txt = ((d.get("title", "") + " ") + d.get("text", "")).strip()
        corpus[str(d["_id"])] = (txt[:args.max_doc_chars] if args.max_doc_chars else txt)
        if args.max_docs and len(corpus) >= args.max_docs:
            break
    # ensure all judged-relevant docs are in the (possibly capped) corpus
    if args.max_docs:
        need = {cid for g in qrels.values() for cid, s in g.items() if s > 0}
        miss = need - set(corpus)
        if miss:
            for line in Path(args.corpus).read_text().splitlines():
                d = json.loads(line)
                if str(d["_id"]) in miss:
                    corpus[str(d["_id"])] = (((d.get("title", "") + " ") + d.get("text", "")).strip())[:args.max_doc_chars] if args.max_doc_chars else ((d.get("title", "") + " ") + d.get("text", "")).strip()

    queries = {}
    for line in Path(args.queries).read_text().splitlines():
        d = json.loads(line)
        qid = str(d["_id"])
        if qid in qrels:
            queries[qid] = d.get("text", "")

    cids = list(corpus)
    t0 = time.time()
    doc_vecs = embed(args.endpoint, [corpus[c] for c in cids], args.doc_prefix, batch=args.batch, normalize=not args.no_normalize, suffix=args.suffix)
    q_ids = list(queries)
    q_vecs = embed(args.endpoint, [queries[q] for q in q_ids], qprefix, batch=args.batch, normalize=not args.no_normalize, suffix=args.suffix)

    if np is not None:
        D = np.asarray(doc_vecs, dtype="float32")
    ndcgs, recalls = [], []
    for qid, qv in zip(q_ids, q_vecs):
        if np is not None:
            order = D.dot(np.asarray(qv, dtype="float32")).argsort()[::-1][:50]
            ranked = [cids[i] for i in order]
        else:
            scored = sorted(((sum(a * b for a, b in zip(qv, dv)), c) for dv, c in zip(doc_vecs, cids)), reverse=True)
            ranked = [c for _, c in scored[:50]]
        gold = qrels[qid]
        ndcgs.append(ndcg_at_k(ranked, gold, 10))
        recalls.append(recall_at_k(ranked, gold, 10))

    n = len(ndcgs) or 1
    res = {
        "name": args.name,
        "endpoint": args.endpoint,
        "queries": len(ndcgs),
        "corpus": len(cids),
        "docs_skipped": _skipped,  # docs the server could not embed (e.g. > model context)
        "ndcg_10": sum(ndcgs) / n,
        "recall_10": sum(recalls) / n,
        "query_prefix": qprefix,
        "doc_prefix": args.doc_prefix,
        "max_doc_chars": args.max_doc_chars,
        "wall_seconds": round(time.time() - t0, 1),
    }
    Path(args.output).write_text(json.dumps(res, indent=2))
    print(f"{args.name}: nDCG@10={res['ndcg_10']:.4f} Recall@10={res['recall_10']:.4f} "
          f"({res['queries']} q / {res['corpus']} docs, skipped={_skipped}, {res['wall_seconds']}s)")


if __name__ == "__main__":
    main()
