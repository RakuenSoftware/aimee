#!/usr/bin/env python3
"""beir_cli.py — BEIR retrieval eval via the `llama-embedding` CLI (no HTTP server).

The llama-server HTTP path was flaky for bulk embedding (connection drops, slot/
batch scheduling, fallback storms). `llama-embedding` embeds a whole file in one
GPU-batched process — reliable. This wraps it: write the corpus/queries to files,
run the CLI, parse the JSON, compute nDCG@10 / Recall@10.

Usage (on the box with the model + llama-embedding):
  python3 beir_cli.py --bin /path/llama-embedding --model m.gguf \
      --corpus corpus.jsonl --queries queries.jsonl --qrels test.tsv \
      --name qwen3-0.6b --output out.json --pooling last \
      [--query-prefix STR | --query-prefix-file F] [--doc-prefix STR] [--ctx 4096]
"""
import argparse, json, math, subprocess, tempfile, time, os
from pathlib import Path

try:
    import numpy as np
except ImportError:
    np = None


def load_qrels(p):
    qrels = {}
    for ln, line in enumerate(Path(p).read_text().splitlines()):
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        qid, cid, score = parts[0], parts[1], parts[2]
        try:
            s = int(float(score))
        except ValueError:
            continue
        qrels.setdefault(qid, {})[cid] = s
    return qrels


def dcg(rels):
    return sum((2 ** r - 1) / math.log2(i + 2) for i, r in enumerate(rels))


def ndcg_at_k(ranked, gold, k=10):
    idcg = dcg(sorted(gold.values(), reverse=True)[:k])
    return (dcg([gold.get(c, 0) for c in ranked[:k]]) / idcg) if idcg > 0 else 0.0


def recall_at_k(ranked, gold, k=10):
    rel = {c for c, s in gold.items() if s > 0}
    return (len(rel & set(ranked[:k])) / len(rel)) if rel else 0.0


def embed_file(bin_path, model, texts, prefix, pooling, ctx, ngl, ub, b):
    """Write texts (newlines flattened) to a temp file, run llama-embedding, return
    the list of (already L2-normalized) embedding vectors."""
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        for t in texts:
            f.write((prefix + " ".join(t.split())) + "\n")  # one prompt per line
        fn = f.name
    out = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False).name
    env = {**os.environ, "LD_LIBRARY_PATH": str(Path(bin_path).parent)}
    cmd = [bin_path, "-m", model, "-f", fn, "--pooling", pooling,
           "--embd-output-format", "json", "--embd-normalize", "2",
           "-ngl", str(ngl), "-c", str(ctx), "-ub", str(ub), "-b", str(b), "--no-warmup"]
    with open(out, "w") as o:
        subprocess.run(cmd, stdout=o, stderr=subprocess.DEVNULL, env=env, check=True)
    data = json.loads(Path(out).read_text())
    rows = data["data"] if isinstance(data, dict) else data
    os.unlink(fn); os.unlink(out)
    return [(r["embedding"] if isinstance(r, dict) else r) for r in rows]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--queries", required=True)
    ap.add_argument("--qrels", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--pooling", default="last")
    ap.add_argument("--query-prefix", default="")
    ap.add_argument("--query-prefix-file")
    ap.add_argument("--doc-prefix", default="")
    ap.add_argument("--ctx", type=int, default=4096)
    ap.add_argument("--ngl", type=int, default=99)
    ap.add_argument("--ub", type=int, default=2048)
    ap.add_argument("--b", type=int, default=8192)
    ap.add_argument("--max-docs", type=int, default=0)
    args = ap.parse_args()

    qprefix = Path(args.query_prefix_file).read_text() if args.query_prefix_file else args.query_prefix
    qrels = load_qrels(args.qrels)

    corpus = {}
    for line in Path(args.corpus).read_text().splitlines():
        d = json.loads(line)
        corpus[str(d["_id"])] = ((d.get("title", "") + " ") + d.get("text", "")).strip()
        if args.max_docs and len(corpus) >= args.max_docs:
            break
    if args.max_docs:
        need = {c for g in qrels.values() for c, s in g.items() if s > 0}
        for line in Path(args.corpus).read_text().splitlines():
            d = json.loads(line)
            if str(d["_id"]) in (need - set(corpus)):
                corpus[str(d["_id"])] = ((d.get("title", "") + " ") + d.get("text", "")).strip()

    queries = {}
    for line in Path(args.queries).read_text().splitlines():
        d = json.loads(line)
        if str(d["_id"]) in qrels:
            queries[str(d["_id"])] = d.get("text", "")

    cids = list(corpus)
    t0 = time.time()
    dv = embed_file(args.bin, args.model, [corpus[c] for c in cids], args.doc_prefix,
                    args.pooling, args.ctx, args.ngl, args.ub, args.b)
    qids = list(queries)
    qv = embed_file(args.bin, args.model, [queries[q] for q in qids], qprefix,
                    args.pooling, args.ctx, args.ngl, args.ub, args.b)
    assert len(dv) == len(cids), f"doc embeddings {len(dv)} != {len(cids)} docs"
    assert len(qv) == len(qids), f"query embeddings {len(qv)} != {len(qids)} queries"

    D = np.asarray(dv, dtype="float32")
    ndcgs, recalls = [], []
    for qid, v in zip(qids, qv):
        order = D.dot(np.asarray(v, dtype="float32")).argsort()[::-1][:50]
        ranked = [cids[i] for i in order]
        ndcgs.append(ndcg_at_k(ranked, qrels[qid], 10))
        recalls.append(recall_at_k(ranked, qrels[qid], 10))
    n = len(ndcgs) or 1
    res = {"name": args.name, "model": args.model, "pooling": args.pooling,
           "queries": len(ndcgs), "corpus": len(cids),
           "ndcg_10": sum(ndcgs) / n, "recall_10": sum(recalls) / n,
           "query_prefix": qprefix, "doc_prefix": args.doc_prefix,
           "wall_seconds": round(time.time() - t0, 1)}
    Path(args.output).write_text(json.dumps(res, indent=2))
    print(f"{args.name}: nDCG@10={res['ndcg_10']:.4f} Recall@10={res['recall_10']:.4f} "
          f"({res['queries']} q / {res['corpus']} docs, {res['wall_seconds']}s)")


if __name__ == "__main__":
    main()
