#!/usr/bin/env python3
"""embedder-server.py: persistent embedding service.

A long-lived HTTP service that loads the sentence-transformers model ONCE and
serves embeddings over HTTP, so the aimee-kb container can embed without paying
a multi-second model reload on every call. The thin embed-remote.py client in the
kb image talks to this service; this service holds the model.

Default model: perplexity-ai/pplx-embed-v1-4b (2560-dim, Qwen3-based,
retrieval-optimized, prefix-free); the lighter pplx-embed-v1-0.6b (1024-dim) is
the alternate tier. The reported dimension is whatever the loaded model produces
— it MUST match config embedding_dim and the schema vector(N) columns, or vector
inserts fail.

Also serves a cross-encoder reranker (lazy-loaded) for the aimee memory rerank
stage, reusing the resident torch stack instead of a second container.

Endpoints:
  POST /embed   body = raw UTF-8 text                 -> JSON float array (model dim)
  POST /rerank  body = JSON [[query,candidate],...]    -> JSON float score array
  GET  /health                                         -> {"status":"ok","model":...,"dim":N}

Config (env):
  EMBEDDER_PORT     listen port (default 8080)
  EMBEDDER_MODEL    sentence-transformers model id (default pplx-embed-v1-4b)
  RERANKER_MODEL    cross-encoder model id (default ettin-reranker-1b-v1)
  EMBEDDER_THREADS  torch intra-op threads (default min(8, ncpu))
  EMBEDDER_QUANTIZE fp32 (default) | int8 (torch dynamic; ~3.3x faster, drifts —
                    pair with the 4b deep tier; see below)

Dependencies: pip install "sentence-transformers>=3.3" "transformers>=5.2" einops
"""

import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODEL_NAME = os.environ.get("EMBEDDER_MODEL", "perplexity-ai/pplx-embed-v1-4b")
# Cross-encoder reranker, served from the same process (it reuses the resident
# torch/sentence-transformers stack). Lazy-loaded on first /rerank so the embedder
# starts and stays healthy even if reranking is never used.
RERANKER_MODEL = os.environ.get("RERANKER_MODEL", "cross-encoder/ettin-reranker-1b-v1")
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

# CI/test stub: serve deterministic fixed-dimension vectors without downloading
# or loading any model. e2e-docker uses this so CI exercises the real
# kb -> embedder -> pgvector wiring (at the deployment's real dim, e.g. 2560 or
# 1024 — never the retired 384) without a multi-GB cold model fetch. Off in prod.
EMBEDDER_STUB = os.environ.get("EMBEDDER_STUB", "").strip() not in ("", "0", "false", "no")
STUB_DIM = int(os.environ.get("EMBEDDER_STUB_DIM", os.environ.get("AIMEE_EMBEDDING_DIM", "2560")) or "2560")

_model = None
_dim = 0
_reranker = None
# First-boot load/fetch state. The model is no longer baked into the image (see
# Dockerfile.embedder) — it is fetched once into the HF_HOME volume on first
# start, which takes minutes for a multi-GB model. Load runs in a background
# thread (main() serves immediately) and /health reports the state. /health is
# 200 ONLY when "ok", so a compose `depends_on: condition: service_healthy` gate
# means the model is actually ready (the KB never starts against an unloaded
# model); a long Dockerfile HEALTHCHECK start-period covers the cold-fetch window
# so the "loading" 503s don't trip the container.
_load_status = "loading"  # loading | ok | error
_load_error = ""


def load_model():
    global _model, _dim
    if _model is not None:
        return _model
    from sentence_transformers import SentenceTransformer  # raises on missing dep
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


def _stub_embed(text):
    """Deterministic unit-norm pseudo-embedding of fixed dimension STUB_DIM, with
    no model. Same text -> same vector (so recall finds it); different text ->
    different vector. CI-only; not a real semantic embedding."""
    import hashlib
    import math

    out = []
    counter = 0
    while len(out) < STUB_DIM:
        h = hashlib.sha256(f"{text}\x00{counter}".encode("utf-8")).digest()
        for i in range(0, len(h), 2):
            if len(out) >= STUB_DIM:
                break
            out.append((int.from_bytes(h[i : i + 2], "big") / 65535.0) - 0.5)
        counter += 1
    norm = math.sqrt(sum(x * x for x in out)) or 1.0
    return [x / norm for x in out]


def _background_load():
    """Fetch (cold volume) + load the models off the request path. The embedder
    is the critical path: once it loads, /health flips to "ok" and /embed serves.
    The reranker is best-effort — a failure degrades reranking, not embedding."""
    global _load_status, _load_error, _dim
    if EMBEDDER_STUB:
        _dim = STUB_DIM
        _load_status = "ok"
        sys.stderr.write(f"embedder-server: STUB mode, dim={STUB_DIM} (no model loaded)\n")
        sys.stderr.flush()
        return
    try:
        load_model()
        _load_status = "ok"
    except Exception as exc:  # noqa: BLE001
        _load_error = str(exc)
        _load_status = "error"
        sys.stderr.write(f"embedder-server: model load failed: {exc}\n")
        sys.stderr.flush()
        return
    try:
        load_reranker()
    except Exception as exc:  # noqa: BLE001
        sys.stderr.write(f"embedder-server: reranker load failed (rerank degraded): {exc}\n")
    sys.stderr.flush()


def embed(text: str):
    if EMBEDDER_STUB:
        return _stub_embed(text)
    vec = _model.encode(text, normalize_embeddings=True)
    return vec.tolist()


def embed_batch(texts):
    """Embed a list of texts in one batched model.encode() call — one HTTP
    round-trip and one batched inference instead of N separate /embed calls.
    Returns a list of float vectors aligned 1:1 with `texts`."""
    if EMBEDDER_STUB:
        return [_stub_embed(t) for t in texts]
    if not texts:
        return []
    vecs = _model.encode(list(texts), normalize_embeddings=True)
    return [v.tolist() for v in vecs]


def rerank(pairs):
    """pairs: [[query, candidate], ...] -> [score, ...] (one cross-encoder score
    per pair, higher = more relevant). Matches the aimee memory_rerank_command
    contract (memory_core_search.inc: JSON pairs in, JSON float array out)."""
    if not pairs:
        return []
    if EMBEDDER_STUB:
        # Deterministic stub score from the embedding cosine — keeps CI's rerank
        # path exercised without a model.
        import math

        out = []
        for q, c in pairs:
            qv, cv = _stub_embed(str(q)), _stub_embed(str(c))
            out.append(float(sum(a * b for a, b in zip(qv, cv))))
        return out
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
            # 200 ONLY when ready (so compose service_healthy == model loaded);
            # "loading" and "error" are 503. dim is the model's true dimension
            # once loaded (0 while loading) — the KB reads it to size the schema,
            # so it is never a placeholder.
            payload = {
                "status": _load_status,
                "model": MODEL_NAME,
                "dim": _dim,
                "quantize": EMBEDDER_QUANTIZE,
            }
            if _load_status == "error":
                payload["error"] = _load_error
            self._send(200 if _load_status == "ok" else 503, payload)
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        path = self.path.rstrip("/")
        if path not in ("/embed", "/embed_batch", "/rerank"):
            self._send(404, {"error": "not found"})
            return
        # Refuse work until the model is loaded — never serve against a not-yet-
        # loaded model (no 0-vector fallback). The KB treats 503 as "warming up".
        if _load_status != "ok":
            self._send(503, {"error": "embedder warming up", "status": _load_status})
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
        if path == "/embed_batch":
            try:
                texts = json.loads(raw.decode("utf-8", errors="replace") or "[]")
                if not isinstance(texts, list):
                    self._send(400, {"error": "embed_batch expects a JSON array of strings"})
                    return
                self._send(200, embed_batch(texts))
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
    # Serve immediately; load the model in the background. On a cold volume the
    # first-boot fetch of a multi-GB model takes minutes, and blocking here would
    # trip the healthcheck before the server is even up. /health reports "loading"
    # (503) until ready; /embed + /rerank return 503 meanwhile.
    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    threading.Thread(target=_background_load, daemon=True).start()
    sys.stderr.write(f"embedder-server: serving on :{PORT}; loading {MODEL_NAME} in background\n")
    sys.stderr.flush()
    server.serve_forever()


if __name__ == "__main__":
    main()
