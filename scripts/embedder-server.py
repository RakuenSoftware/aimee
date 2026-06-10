#!/usr/bin/env python3
"""embedder-server.py: persistent embedding service.

A long-lived HTTP service that loads the sentence-transformers model ONCE and
serves embeddings over HTTP, so the aimee-kb container can embed without paying
a multi-second model reload on every call. The thin embed-remote.py client in the
kb image talks to this service; this service holds the model.

Default model: perplexity-ai/pplx-embed-v1-0.6b (1024-dim, Qwen3-based,
retrieval-optimized, prefix-free). The reported dimension is whatever the loaded
model produces — it MUST match config embedding_dim and the schema vector(N)
columns, or vector inserts fail.

Also serves a cross-encoder reranker (lazy-loaded) for the aimee memory rerank
stage, reusing the resident torch stack instead of a second container.

Endpoints:
  POST /embed   body = raw UTF-8 text                 -> JSON float array (model dim)
  POST /rerank  body = JSON [[query,candidate],...]    -> JSON float score array
  GET  /health                                         -> {"status":"ok","model":...,"dim":N}

Config (env):
  EMBEDDER_PORT     listen port (default 8080)
  EMBEDDER_MODEL    sentence-transformers model id (default pplx-embed-v1-0.6b)
  RERANKER_MODEL    cross-encoder model id (default ettin-reranker-400m-v1)
  EMBEDDER_THREADS  torch intra-op threads (default min(8, ncpu))
  EMBEDDER_QUANTIZE fp32 (default) | int8 (torch dynamic; ~3.3x faster, drifts —
                    pair with the 4b deep tier; see below)

Dependencies: pip install "sentence-transformers>=3.3" "transformers>=5.2" einops
"""

import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODEL_NAME = os.environ.get("EMBEDDER_MODEL", "perplexity-ai/pplx-embed-v1-0.6b")
# Cross-encoder reranker, served from the same process (it reuses the resident
# torch/sentence-transformers stack). Lazy-loaded on first /rerank so the embedder
# starts and stays healthy even if reranking is never used.
RERANKER_MODEL = os.environ.get("RERANKER_MODEL", "cross-encoder/ettin-reranker-400m-v1")
PORT = int(os.environ.get("EMBEDDER_PORT", "8080"))
# CPU serving tuning. A single short embed does not scale past ~8 intra-op
# threads — on a 32-core host pplx-embed-0.6b is 269ms at 32 threads but 189ms at
# 8 (per-call thread overhead dominates the tiny workload). Cap to a sane default;
# an explicit EMBEDDER_THREADS wins. Set OMP before torch is imported.
EMBEDDER_THREADS = int(os.environ.get("EMBEDDER_THREADS", "0")) or min(8, os.cpu_count() or 8)
os.environ.setdefault("OMP_NUM_THREADS", str(EMBEDDER_THREADS))
# Optional int8 dynamic quantization. Pure torch (no optimum/onnx), so it composes
# with the transformers>=5.2 the Ettin reranker needs — unlike the q4 ONNX path.
# EMBEDDER_QUANTIZE=int8 runs ~3.3x faster on CPU (58ms vs 190ms/embed @ 8 threads)
# but the embedding drifts (~0.90 cosine vs fp32): dynamic quant is per-tensor and
# uncalibrated. INTENDED PAIRING: serve int8 only when the 4b deep tier is enabled
# (it re-embeds for quality, backstopping the drift); serve fp32 when the 0.6b is
# the sole tier. The deep-tier deployment sets this to int8; the default is fp32.
EMBEDDER_QUANTIZE = os.environ.get("EMBEDDER_QUANTIZE", "fp32").strip().lower()

_model = None
_dim = 0
_reranker = None


def load_model():
    global _model, _dim
    if _model is not None:
        return _model
    try:
        from sentence_transformers import SentenceTransformer
    except ImportError:
        sys.stderr.write(
            "embedder-server: sentence-transformers not installed"
            ' (pip install "sentence-transformers>=3.3" einops)\n'
        )
        sys.exit(1)
    import torch

    torch.set_num_threads(EMBEDDER_THREADS)
    # trust_remote_code: the Qwen3-based embedders (pplx-embed, gte-Qwen2) ship
    # custom modelling code on the Hub.
    _model = SentenceTransformer(MODEL_NAME, trust_remote_code=True)
    _dim = _model.get_sentence_embedding_dimension() or 0  # read before quantizing
    if EMBEDDER_QUANTIZE == "int8":
        import torch.ao.quantization as ao_q

        _model = ao_q.quantize_dynamic(_model, {torch.nn.Linear}, dtype=torch.qint8)
    sys.stderr.write(
        f"embedder-server: loaded {MODEL_NAME} dim={_dim} threads={EMBEDDER_THREADS}"
        f" quant={EMBEDDER_QUANTIZE}\n"
    )
    return _model


def load_reranker():
    global _reranker
    if _reranker is not None:
        return _reranker
    from sentence_transformers import CrossEncoder

    _reranker = CrossEncoder(RERANKER_MODEL, trust_remote_code=True)
    return _reranker


def embed(text: str):
    vec = load_model().encode(text, normalize_embeddings=True)
    return vec.tolist()


def rerank(pairs):
    """pairs: [[query, candidate], ...] -> [score, ...] (one cross-encoder score
    per pair, higher = more relevant). Matches the aimee memory_rerank_command
    contract (memory_core_search.inc: JSON pairs in, JSON float array out)."""
    if not pairs:
        return []
    scores = load_reranker().predict([[str(q), str(c)] for q, c in pairs])
    return [float(s) for s in scores]


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):  # quiet access log
        pass

    def do_GET(self):
        if self.path.rstrip("/") == "/health":
            self._send(
                200,
                {"status": "ok", "model": MODEL_NAME, "dim": _dim, "quantize": EMBEDDER_QUANTIZE},
            )
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        path = self.path.rstrip("/")
        if path not in ("/embed", "/rerank"):
            self._send(404, {"error": "not found"})
            return
        length = int(self.headers.get("content-length", "0") or "0")
        raw = self.rfile.read(length) if length else b""
        if path == "/rerank":
            try:
                pairs = json.loads(raw.decode("utf-8", errors="replace") or "[]")
                self._send(200, rerank(pairs))
            except Exception as exc:  # noqa: BLE001
                self._send(500, {"error": str(exc)})
            return
        text = raw.decode("utf-8", errors="replace")
        if not text.strip():
            self._send(400, {"error": "empty input"})
            return
        try:
            self._send(200, embed(text))
        except Exception as exc:  # noqa: BLE001
            self._send(500, {"error": str(exc)})


def main():
    # Fail fast if the model can't load, so the container healthcheck flips
    # rather than silently serving 500s.
    load_model()
    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    sys.stderr.write(f"embedder-server: {MODEL_NAME} ready on :{PORT}\n")
    sys.stderr.flush()
    server.serve_forever()


if __name__ == "__main__":
    main()
