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
import hmac
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.request

EMBED_URL = os.environ.get("AIMEE_LLM_EMBED_URL", "http://127.0.0.1:8081").rstrip("/")
RERANK_URL = os.environ.get("AIMEE_LLM_RERANK_URL", "http://127.0.0.1:8082").rstrip("/")
RERANK_HEAD_DIR = os.environ.get("AIMEE_LLM_RERANK_HEAD", "")
EMBED_MODEL = os.environ.get("AIMEE_LLM_EMBED_MODEL", "Qwen3-Embedding")
PORT = int(os.environ.get("AIMEE_LLM_PORT", "8080"))
BATCH_CAP = int(os.environ.get("AIMEE_LLM_BATCH_CAP", "512"))
RERANK_SEP = "</s>"

# CI/dev STUB mode: serve deterministic embed/rerank/synth with NO upstream
# llama-servers. The supervisor skips launching the per-role servers and the
# image can be built without baking the multi-GB GGUFs (Dockerfile arg), so e2e
# exercises the real kb -> gateway contract (and the /embed,/rerank,/v1/chat
# shapes) cheaply. Vectors are deterministic per-input so retrieval is stable.
STUB = os.environ.get("AIMEE_LLM_STUB", "") not in ("", "0", "false")
STUB_DIM = int(os.environ.get("AIMEE_LLM_STUB_DIM", os.environ.get("EMBEDDER_STUB_DIM", "1024")))


def _stub_vec(text):
    """Deterministic, L2-normalised STUB_DIM vector seeded from the input text."""
    import hashlib
    seed = int.from_bytes(hashlib.sha256((text or "").encode("utf-8")).digest()[:8], "big") or 1
    x = seed
    vals = []
    for _ in range(STUB_DIM):
        x = (x * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)
        vals.append((x >> 11) / float(1 << 53) * 2.0 - 1.0)
    norm = sum(v * v for v in vals) ** 0.5 or 1.0
    return [v / norm for v in vals]
# Synth role: AIMEE_LLM_SYNTH_URL is the upstream OpenAI-compatible base; empty = no
# synth configured (the role reports `gated`/unconfigured). Mode is informational here
# (local = a baked llama-server; forward/external = an off-box upstream) — both just
# proxy /v1/chat/completions to AIMEE_LLM_SYNTH_URL, which is why one code path serves
# all three. Streaming is disabled in the first release (typed 400).
SYNTH_URL = os.environ.get("AIMEE_LLM_SYNTH_URL", "").rstrip("/")
SYNTH_MODE = os.environ.get("AIMEE_LLM_SYNTH_MODE", "local")

# ---- §1a security: the gateway is privileged (it holds upstream creds + routes) ----
# Bearer auth (constant-time). mTLS is the documented production posture; bearer-with-the
# controls below is the in-container default. Empty token = auth disabled (dev only),
# which the bind guard then refuses to expose on 0.0.0.0.
AUTH_TOKEN = os.environ.get("AIMEE_LLM_AUTH_TOKEN", "")
# The container binds 0.0.0.0 (reachable by kb/server on the deployment network, as the
# embedder does today). With no auth the deployment network is the boundary; STRICT_BIND
# turns the §1a "no unauthenticated wildcard" into a hard startup refuse for operators who
# want it enforced rather than warned.
BIND = os.environ.get("AIMEE_LLM_BIND", "0.0.0.0")
STRICT_BIND = os.environ.get("AIMEE_LLM_STRICT_BIND", "") not in ("", "0", "false")
# Scope is DERIVED from the authenticated identity, never trusted from the wire. With a
# single bearer the gateway runs the operator/curator scope; per-user scope arrives with
# mTLS cert CN / per-user tokens. A caller-supplied X-Aimee-Scope is always rejected.
SCOPE_DEFAULT = os.environ.get("AIMEE_LLM_SCOPE", "curator")


class GatewayError(Exception):
    """Carries an HTTP status + a typed error body."""

    def __init__(self, status, code, message):
        super().__init__(message)
        self.status = status
        self.body = {"error": {"code": code, "message": message}}


# ---- §1a controls (pure; unit-tested without a model/network) ----

def validate_bind(bind, auth_token, strict=False):
    """The gateway is privileged. A wildcard bind (0.0.0.0 / ::) with NO auth exposes an
    unauthenticated surface — fine when the deployment network is the boundary (today's
    posture), but flagged. Returns "ok" / "warn"; raises RuntimeError under `strict` so an
    operator can enforce §1a's "no unauthenticated wildcard" as a hard startup failure."""
    wildcard = bind in ("0.0.0.0", "::", "", "0")  # "0" is also INADDR_ANY on Linux
    if wildcard and not auth_token:
        if strict:
            raise RuntimeError(
                f"STRICT_BIND: refusing wildcard '{bind}' with no AIMEE_LLM_AUTH_TOKEN — set a "
                "token (or mTLS); this is the §1a bind-address guard")
        return "warn"
    return "ok"


def check_auth(auth_header):
    """Constant-time bearer check. No token configured -> open (dev). Configured ->
    require `Authorization: Bearer <token>`; mismatch/absent -> 401. Never logs the token."""
    if not AUTH_TOKEN:
        return
    presented = ""
    if auth_header and auth_header[:7].lower() == "bearer ":  # scheme is case-insensitive
        presented = auth_header[7:].strip()
    if not hmac.compare_digest(presented, AUTH_TOKEN):
        raise GatewayError(401, "unauthorized", "missing or invalid bearer token")


def derive_scope(get_header):
    """Scope is DERIVED from the authenticated identity, never trusted from the wire. A
    caller-supplied X-Aimee-Scope is rejected (anti-spoof). `get_header(name)` returns the
    request header or None."""
    if get_header("X-Aimee-Scope"):
        raise GatewayError(403, "scope_spoof", "X-Aimee-Scope is set by the gateway, not the caller")
    return SCOPE_DEFAULT


_AUDIT_DENY = ("authorization", "cookie", "token", "secret", "key", "body", "prompt",
               "content", "message", "input", "password")


def audit(event, scope, outcome, **meta):
    """Metadata-only audit line to the named sink (stderr, 'AIMEE-AUDIT' prefix). NEVER
    content or credentials — only scope/outcome/byte-counts/request-id. A sanitizer drops
    any meta key that looks credential/content-bearing so the guarantee holds even if a
    future caller passes the wrong field. The kb/operator log pipeline ships these;
    encryption + retention are the sink's concern."""
    rec = {"audit": event, "scope": scope, "outcome": outcome}
    for k, v in meta.items():
        if any(bad in k.lower() for bad in _AUDIT_DENY):
            continue  # never audit a credential/content-bearing field
        rec[k] = v
    sys.stderr.write("AIMEE-AUDIT " + json.dumps(rec, sort_keys=True) + "\n")
    sys.stderr.flush()


class CircuitBreaker:
    """Per-upstream breaker for forward/external (§4): `threshold` consecutive errors OR
    >50% error rate over `window`s -> open; after `recovery`s a half-open probe; one
    success closes it. `allow()` is checked before a call; record the outcome after.
    Clock injected for testing."""

    def __init__(self, threshold=5, window=30.0, recovery=60.0, clock=time.monotonic):
        self.threshold = threshold
        self.window = window
        self.recovery = recovery
        self.clock = clock
        self.state = "closed"
        self._consec = 0
        self._events = []  # (t, ok) within the window
        self._opened_at = 0.0
        self._lock = threading.Lock()  # the gateway is multi-threaded

    def _prune(self, now):
        self._events = [(t, ok) for (t, ok) in self._events if now - t <= self.window]

    def allow(self):
        with self._lock:
            now = self.clock()
            if self.state == "open":
                if now - self._opened_at >= self.recovery:
                    self.state = "half-open"
                    return True  # single probe
                return False
            return True

    def record(self, ok):
        with self._lock:
            now = self.clock()
            self._events.append((now, ok))
            self._prune(now)
            if self.state == "half-open":
                self.state = "closed" if ok else "open"
                self._opened_at = now
                if ok:
                    self._consec = 0
                return
            if ok:
                self._consec = 0
                return
            self._consec += 1
            n = len(self._events)
            err_rate = sum(1 for _, o in self._events if not o) / n if n else 0
            if self._consec >= self.threshold or (n >= 4 and err_rate > 0.5):
                self.state = "open"
                self._opened_at = now


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
    if STUB:
        return _stub_vec(text)
    return _embeddings(EMBED_URL, text)


def do_embed_batch(texts):
    validate_batch(texts)
    if STUB:
        return [_stub_vec(t) for t in texts]
    return _embeddings(EMBED_URL, list(texts)) if texts else []


def do_rerank(obj):
    """[[query, candidate], ...] -> [score, ...] (aligned to input order). Embeds each
    `query</s>candidate` on the ettin encoder and applies the Dense head."""
    pairs = parse_rerank_pairs(obj)
    if not pairs:
        return []
    if STUB:
        # Deterministic per-pair score (stable ordering) without the encoder/head.
        return [sum(_stub_vec(f"{q}{RERANK_SEP}{c}")[:8]) for q, c in pairs]
    head = _rerank_head()
    texts = [f"{q}{RERANK_SEP}{c}" for q, c in pairs]
    vecs = _embeddings(RERANK_URL, texts)
    scores = head.score(vecs)  # numpy array aligned to input order
    return [float(s) for s in (scores if hasattr(scores, "__len__") else [scores])]


_synth_breaker = CircuitBreaker()


def do_synth(body):
    """Proxy a /v1/chat/completions request to the configured synth upstream
    (AIMEE_LLM_SYNTH_URL — a local baked llama-server, or a forwarded/external base).
    Streaming is disabled in the first release: `stream:true` -> a typed 400 the client
    can branch on. A circuit breaker (§4) opens on repeated upstream errors -> typed 503."""
    if not isinstance(body, dict):
        raise GatewayError(400, "bad_request", "chat/completions expects a JSON object")
    if body.get("stream"):
        raise GatewayError(
            400, "streaming_unsupported",
            "streaming is disabled in this release; set stream=false")
    if STUB:
        return {
            "id": "stub", "object": "chat.completion", "model": "aimee-llm-stub",
            "choices": [{"index": 0, "finish_reason": "stop",
                         "message": {"role": "assistant", "content": "stub synthesis response"}}],
            "usage": {"prompt_tokens": 0, "completion_tokens": 2, "total_tokens": 2},
        }
    if not SYNTH_URL:
        raise GatewayError(503, "synth_unconfigured", "AIMEE_LLM_SYNTH_URL is not set")
    if not _synth_breaker.allow():
        raise GatewayError(503, "provider_unavailable", "synth upstream circuit is open")
    try:
        out = _http_post_json(f"{SYNTH_URL}/v1/chat/completions", body, timeout=300)
    except Exception:
        _synth_breaker.record(False)
        raise
    _synth_breaker.record(True)
    return out


def role_state(base, configured=True):
    """Four-state readiness for one role (unified-llm §4): `gated` (role not configured
    / upstream absent), `ready` (child serving), `loading` (child up but model warming,
    503), `down` (configured but unreachable)."""
    if not configured or not base:
        return "gated"
    try:
        with urllib.request.urlopen(f"{base}/health", timeout=3) as r:
            return "ready" if r.status == 200 else "loading"
    except urllib.error.HTTPError as e:  # 503 while a model loads
        return "loading" if e.code == 503 else "down"
    except Exception:  # noqa: BLE001
        return "down"


def role_states():
    """Per-role state for /health/{embed,rerank,synth}."""
    if STUB:
        return {"embed": "ready", "rerank": "ready", "synth": "ready"}
    return {
        "embed": role_state(EMBED_URL),
        "rerank": role_state(RERANK_URL, configured=bool(RERANK_HEAD_DIR)),
        "synth": role_state(SYNTH_URL, configured=bool(SYNTH_URL)),
    }


def child_health():
    """Probe the configured llama-server children for the AGGREGATE /health. Returns
    role -> bool|None (None = not configured, excluded from the aggregate)."""
    states = role_states()
    out = {}
    for role, st in states.items():
        out[role] = None if st == "gated" else (st == "ready")
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
            path = self.path.rstrip("/")
            if path == "/health":
                if STUB:
                    # Report the dim so the kb's embedder-autodim sizes the schema
                    # (the embed-remote.py --dim probe reads payload["dim"]).
                    self._send(200, {"status": "ok", "model": "aimee-llm-stub", "dim": STUB_DIM})
                    return
                st = health_state(child_health())
                self._send(200 if st == "ok" else 503, {"status": st, "model": EMBED_MODEL})
            elif path in ("/health/embed", "/health/rerank", "/health/synth"):
                role = path.rsplit("/", 1)[1]
                st = role_states()[role]
                # ready=200 so a probe can gate on a single role (the kb hits
                # /health/embed so it indexes as soon as embed is ready).
                self._send(200 if st == "ready" else 503, {"status": st, "role": role})
            else:
                self._send(404, {"error": "not found"})

        def do_POST(self):
            path = self.path.rstrip("/")
            n = int(self.headers.get("content-length", "0") or "0")
            raw = self.rfile.read(n) if n else b""
            try:
                # §1a: every work request is authenticated; the scope is DERIVED from the
                # identity (a caller-supplied X-Aimee-Scope is rejected).
                check_auth(self.headers.get("Authorization"))
                scope = derive_scope(self.headers.get)
                if path == "/embed":
                    text = raw.decode("utf-8", errors="replace")
                    if not text.strip():
                        raise GatewayError(400, "bad_request", "empty input")
                    self._send(200, do_embed(text))
                elif path == "/embed_batch":
                    self._send(200, do_embed_batch(json.loads(raw or b"[]")))
                elif path == "/rerank":
                    self._send(200, do_rerank(json.loads(raw or b"[]")))
                elif path == "/v1/chat/completions":
                    out = do_synth(json.loads(raw or b"{}"))
                    # §4 audit: metadata-only (scope/outcome/bytes), never content/creds.
                    audit("chat", scope, "ok", bytes_in=len(raw),
                          bytes_out=len(json.dumps(out)))
                    self._send(200, out)
                else:
                    self._send(404, {"error": "not found"})
            except GatewayError as ge:
                if path == "/v1/chat/completions":
                    audit("chat", SCOPE_DEFAULT, "error", code=ge.body["error"]["code"])
                self._send(ge.status, ge.body)
            except Exception as exc:  # noqa: BLE001
                self._send(500, {"error": {"code": "internal", "message": str(exc)}})

    return ThreadingHTTPServer((BIND, PORT), Handler)


def main():  # pragma: no cover
    # §1a bind guard: refuse (strict) or warn on an unauthenticated wildcard bind.
    if validate_bind(BIND, AUTH_TOKEN, STRICT_BIND) == "warn":
        audit("startup", SCOPE_DEFAULT, "warn", bind=BIND, auth="off",
              note="unauthenticated wildcard bind; deployment network is the boundary")
    srv = build_server()
    sys.stderr.write(f"aimee-llm-gateway: {BIND}:{PORT} embed={EMBED_URL} rerank={RERANK_URL} "
                     f"auth={'on' if AUTH_TOKEN else 'off'}\n")
    sys.stderr.flush()
    srv.serve_forever()


if __name__ == "__main__":  # pragma: no cover
    main()
