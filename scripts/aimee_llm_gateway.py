#!/usr/bin/env python3
"""aimee-llm gateway — the retrieval surface of the unified llama.cpp container.

Preserves the embedder-server contract (so embed-remote.py / rerank-remote.py / the kb
config / AIMEE_EMBEDDER_URL are untouched):

  POST /embed        body = raw UTF-8 text                 -> JSON float array (dim)
  POST /embed_batch  body = JSON [text, ...]               -> JSON [[float,...], ...]
  POST /rerank       body = JSON [[query, candidate], ...] -> JSON [float, ...]
  GET  /health       -> {"status": ok|loading|down, "model":..., "dim":N}

…but the backend is **llama.cpp**, not torch/sentence-transformers:
  - /embed[_batch] proxy to the embedder llama-server `/v1/embeddings`.
  - /rerank embeds `query</s>candidate` on the ETTIN ENCODER llama-server `/v1/embeddings`
    (CLS pooling) and applies the EttinRerankHead (the ST Dense head, numpy) — see
    aimee_llm_rerank_head.py and benchmarks/results/unified-llm/P2-serving-validation.md.

Config (env):
  AIMEE_LLM_EMBED_URL     embedder llama-server base (default http://127.0.0.1:8081)
  AIMEE_LLM_RERANK_URL    ettin-encoder llama-server base (default http://127.0.0.1:8082)
  AIMEE_LLM_RERANK_HEAD   dir with 2_Dense/3_LayerNorm/4_Dense safetensors (the head)
  AIMEE_LLM_EMBED_MODEL   embedder model id (for /health + the (model_id,dim) drift guard)
  AIMEE_LLM_PORT          listen port (default 8080)
  AIMEE_LLM_BATCH_CAP     /embed_batch max vectors per call (default 512 -> 413 on excess)

The router/modes (local/forward/external) and synth (/v1/chat/completions) land in a
follow-up; this module is the retrieval gateway + the validated rerank-head path.
"""
import json
import os
import urllib.request

EMBED_URL = os.environ.get("AIMEE_LLM_EMBED_URL", "http://127.0.0.1:8081").rstrip("/")
RERANK_URL = os.environ.get("AIMEE_LLM_RERANK_URL", "http://127.0.0.1:8082").rstrip("/")
RERANK_HEAD_DIR = os.environ.get("AIMEE_LLM_RERANK_HEAD", "")
EMBED_MODEL = os.environ.get("AIMEE_LLM_EMBED_MODEL", "Qwen3-Embedding")
PORT = int(os.environ.get("AIMEE_LLM_PORT", "8080"))
BATCH_CAP = int(os.environ.get("AIMEE_LLM_BATCH_CAP", "512"))
RERANK_SEP = "</s>"


class GatewayError(Exception):
    """Carries an HTTP status + a typed error body."""

    def __init__(self, status, code, message):
        super().__init__(message)
        self.status = status
        self.body = {"error": {"code": code, "message": message}}


def _http_post_json(url, payload, timeout=120):
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers={"content-type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _embeddings(base_url, inputs):
    """Call a llama-server `/v1/embeddings`; return a list of float vectors aligned to
    `inputs` (a str or list[str])."""
    one = isinstance(inputs, str)
    body = _http_post_json(f"{base_url}/v1/embeddings", {"input": inputs, "model": "e"})
    vecs = [row["embedding"] for row in body["data"]]
    return vecs[0] if one else vecs


# ---- pure request logic (stdlib-only; unit-tested without a model) ----

def validate_batch(texts, cap=BATCH_CAP):
    """Validate an /embed_batch payload. Raises GatewayError(400/413). Returns the list."""
    if not isinstance(texts, list):
        raise GatewayError(400, "bad_request", "embed_batch expects a JSON array of strings")
    if len(texts) > cap:
        raise GatewayError(
            413, "batch_too_large", f"{len(texts)} inputs exceeds the per-call cap of {cap}"
        )
    return texts


def parse_rerank_pairs(obj):
    """Validate a /rerank payload ([[query, candidate], ...]). Raises GatewayError(400)."""
    if not isinstance(obj, list):
        raise GatewayError(400, "bad_request", "rerank expects a JSON array of [query, candidate]")
    pairs = []
    for p in obj:
        if not (isinstance(p, (list, tuple)) and len(p) == 2):
            raise GatewayError(400, "bad_request", "each rerank item must be [query, candidate]")
        pairs.append((str(p[0]), str(p[1])))
    return pairs


def health_state(child_oks):
    """Aggregate per-child readiness into the gateway status. `child_oks` maps role ->
    bool|None (None = not configured). ready iff every configured child is up."""
    configured = {k: v for k, v in child_oks.items() if v is not None}
    if not configured:
        return "loading"
    return "ok" if all(configured.values()) else "down"


# ---- handlers (call llama.cpp; the rerank head is the validated P2 path) ----

_head = None


def _rerank_head():
    global _head
    if _head is None:
        if not RERANK_HEAD_DIR:
            raise GatewayError(503, "rerank_unconfigured", "AIMEE_LLM_RERANK_HEAD is not set")
        from aimee_llm_rerank_head import EttinRerankHead

        _head = EttinRerankHead.from_dir(RERANK_HEAD_DIR)
    return _head


def do_embed(text):
    return _embeddings(EMBED_URL, text)


def do_embed_batch(texts):
    validate_batch(texts)
    return _embeddings(EMBED_URL, list(texts)) if texts else []


def do_rerank(obj):
    """[[query, candidate], ...] -> [score, ...] (aligned to input order). Embeds each
    `query</s>candidate` on the ettin encoder and applies the Dense head."""
    pairs = parse_rerank_pairs(obj)
    if not pairs:
        return []
    head = _rerank_head()
    texts = [f"{q}{RERANK_SEP}{c}" for q, c in pairs]
    vecs = _embeddings(RERANK_URL, texts)
    scores = head.score(vecs)  # numpy array aligned to input order
    return [float(s) for s in (scores if hasattr(scores, "__len__") else [scores])]


def child_health():
    """Probe the configured llama-server children. Returns role -> bool|None."""
    out = {"embed": None, "rerank": None}
    for role, base in (("embed", EMBED_URL), ("rerank", RERANK_URL if RERANK_HEAD_DIR else None)):
        if not base:
            continue
        try:
            with urllib.request.urlopen(f"{base}/health", timeout=3) as r:
                out[role] = r.status == 200
        except Exception:  # noqa: BLE001
            out[role] = False
    return out


def build_server():
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):  # quiet
            pass

        def _send(self, code, payload):
            body = json.dumps(payload).encode("utf-8")
            self.send_response(code)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path.rstrip("/") == "/health":
                st = health_state(child_health())
                self._send(200 if st == "ok" else 503, {"status": st, "model": EMBED_MODEL})
            else:
                self._send(404, {"error": "not found"})

        def do_POST(self):
            path = self.path.rstrip("/")
            n = int(self.headers.get("content-length", "0") or "0")
            raw = self.rfile.read(n) if n else b""
            try:
                if path == "/embed":
                    text = raw.decode("utf-8", errors="replace")
                    if not text.strip():
                        raise GatewayError(400, "bad_request", "empty input")
                    self._send(200, do_embed(text))
                elif path == "/embed_batch":
                    self._send(200, do_embed_batch(json.loads(raw or b"[]")))
                elif path == "/rerank":
                    self._send(200, do_rerank(json.loads(raw or b"[]")))
                else:
                    self._send(404, {"error": "not found"})
            except GatewayError as ge:
                self._send(ge.status, ge.body)
            except Exception as exc:  # noqa: BLE001
                self._send(500, {"error": {"code": "internal", "message": str(exc)}})

    return ThreadingHTTPServer(("0.0.0.0", PORT), Handler)


def main():  # pragma: no cover
    import sys

    srv = build_server()
    sys.stderr.write(f"aimee-llm-gateway: :{PORT} embed={EMBED_URL} rerank={RERANK_URL}\n")
    sys.stderr.flush()
    srv.serve_forever()


if __name__ == "__main__":  # pragma: no cover
    main()
