#!/usr/bin/env python3
"""aimee-llm gateway — the retrieval surface of the unified llama.cpp container.

Preserves the embedder-server contract (so embed-remote.py / rerank-remote.py / the kb
config / AIMEE_EMBEDDER_URL are untouched):

  POST /embed        body = raw UTF-8 text                 -> JSON float array (dim)
  POST /embed_batch  body = JSON [text, ...]               -> JSON [[float,...], ...]
  POST /rerank       body = JSON [[query, candidate], ...] -> JSON [float, ...]

/embed and /embed_batch take an optional `?input_type=query|document` selecting which of
the embedder's asymmetric prefixes to apply (see EMBED_PREFIXES). It rides in the query
string because neither body has an envelope to extend — /embed's is raw text and
/embed_batch's is a bare JSON array — so old clients keep working unchanged. Omitted
means `document`, since every ingest path is a document and only queries need to opt in.
  POST /auth/verify  authenticated service-identity check (no model work)
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
  AIMEE_LLM_EMBED_MODEL   embedder model id (for /health, the (model_id,dim) drift guard,
                          and the registry lookup — an id absent from the registry is
                          refused rather than served without prefixes)
  AIMEE_LLM_EMBEDDERS_FILE  embedder registry path (default scripts/embedders.json)
  AIMEE_LLM_PORT          listen port (default 8080)
  AIMEE_LLM_BATCH_CAP     /embed_batch max vectors per call (default 512 -> 413 on excess)

The router/modes (local/forward/external) and synth (/v1/chat/completions) land in a
follow-up; this module is the retrieval gateway + the validated rerank-head path.
"""
import hmac
import json
import os
import shlex
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

EMBED_URL = os.environ.get("AIMEE_LLM_EMBED_URL", "http://127.0.0.1:8081").rstrip("/")
RERANK_URL = os.environ.get("AIMEE_LLM_RERANK_URL", "http://127.0.0.1:8082").rstrip("/")
RERANK_HEAD_DIR = os.environ.get("AIMEE_LLM_RERANK_HEAD", "")
EMBED_MODEL = os.environ.get("AIMEE_LLM_EMBED_MODEL", "nomic-embed-text-v2-moe")
PORT = int(os.environ.get("AIMEE_LLM_PORT", "8080"))
BATCH_CAP = int(os.environ.get("AIMEE_LLM_BATCH_CAP", "512"))
RERANK_SEP = "</s>"

# ---- the embedder registry ----
# Everything that varies per embedder — coordinates, pooling, width, context, and the
# query/document prefixes — lives in ONE data file, scripts/embedders.json, keyed by
# model identity. The supervisor reads the same file for provisioning and serving flags,
# so switching embedders is one entry rather than edits scattered across a shell script,
# this module and a C header. See that file's _comment for why.
#
# Keying on identity is what stops a swap from silently inheriting the previous model's
# settings — the failure AIMEE_LLM_EMBED_POOLING already demonstrated by defaulting to
# `last` (right for Qwen3, silently wrong for nomic: well-formed vectors, no error,
# collapsed recall).
EMBEDDERS_FILE = os.environ.get("AIMEE_LLM_EMBEDDERS_FILE") or os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "embedders.json"
)
INPUT_TYPES = ("query", "document")
# Fields every entry must declare. Absent means the operator has not thought about it,
# which for pooling or prefixes is indistinguishable at runtime from thinking wrongly.
EMBEDDER_REQUIRED_FIELDS = ("pooling", "dim", "context", "prefixes")
# Additionally required to SERVE a model — the supervisor has to fetch a specific,
# verifiable file. An entry may carry these blank (a measured model nobody deploys);
# --embedder-descriptor refuses such an entry rather than let the fetch fail obscurely.
EMBEDDER_PROVISION_FIELDS = ("repo", "file", "revision", "sha256")


class EmbedderRegistryError(RuntimeError):
    """The registry is missing or malformed — an operator error, surfaced at startup."""


def load_embedders(path=None):
    """Parse the registry into {normalised model id: spec}. Raises on anything malformed
    rather than degrading, since every degraded mode here is silent downstream."""
    path = path or EMBEDDERS_FILE
    try:
        with open(path, "r", encoding="utf-8") as handle:
            document = json.load(handle)
    except (OSError, ValueError) as exc:
        raise EmbedderRegistryError(f"cannot read embedder registry {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise EmbedderRegistryError(f"{path}: top level must be an object")
    table = document.get("embedders")
    if not isinstance(table, dict) or not table:
        raise EmbedderRegistryError(f"{path} declares no embedders")
    registry = {}
    for model_id, spec in table.items():
        if not isinstance(spec, dict):
            raise EmbedderRegistryError(f"{path}: entry {model_id!r} is not an object")
        missing = [field for field in EMBEDDER_REQUIRED_FIELDS if field not in spec]
        if missing:
            raise EmbedderRegistryError(
                f"{path}: entry {model_id!r} is missing {', '.join(missing)}"
            )
        prefixes = spec["prefixes"]
        if not isinstance(prefixes, dict) or set(prefixes) != set(INPUT_TYPES):
            raise EmbedderRegistryError(
                f"{path}: entry {model_id!r} must declare prefixes for exactly "
                f"{', '.join(INPUT_TYPES)} (empty strings if its card defines none)"
            )
        if not all(isinstance(prefixes[side], str) for side in INPUT_TYPES):
            raise EmbedderRegistryError(f"{path}: entry {model_id!r} has a non-string prefix")
        registry[embed_model_key(model_id)] = spec
    return registry
# Ingest is the overwhelming majority of embed traffic and every ingest path is a
# document, so an omitted input_type means `document` and only the query paths opt in.
# It must NOT mean "no prefix": defaulting to bare text is exactly the regression this
# table exists to prevent.
INPUT_TYPE_DEFAULT = "document"


def embed_model_key(model_id):
    """Normalise a model id to its prefix-table key.

    Model ids carry deployment decoration — a `@v1` revision suffix (as the db2 model
    records use) or case differences — none of which change which prefixes the model
    was trained with.
    """
    return (model_id or "").split("@", 1)[0].strip().lower()


# Read once at import — the registry is deployment data, not request data. A malformed or
# missing file is an operator error, so the failure is kept rather than swallowed and is
# reported twice over: main() refuses to start on it, and any embed request answers 503
# carrying the same message (which is what an already-running gateway would see if the
# file were edited badly under it).
try:
    EMBEDDERS = load_embedders()
    EMBEDDERS_ERROR = None
except EmbedderRegistryError as exc:  # pragma: no cover - exercised via load_embedders
    EMBEDDERS = {}
    EMBEDDERS_ERROR = str(exc)


def parse_input_type(value):
    """Validate a caller-supplied input_type. Raises GatewayError(400)."""
    if value is None or value == "":
        return INPUT_TYPE_DEFAULT
    if value not in INPUT_TYPES:
        raise GatewayError(
            400,
            "bad_request",
            f"input_type must be one of {', '.join(INPUT_TYPES)} (got {value!r})",
        )
    return value


def embedder_spec(model_id=None):
    """Return the registry entry for the configured embedder.

    Refuses (503) a model absent from the registry rather than serving it with empty or
    inherited settings — an unregistered model is an operator error we can detect, while
    quietly embedding with the wrong prefixes or pooling is undetectable downstream.
    """
    if EMBEDDERS_ERROR is not None:
        raise GatewayError(503, "embedder_registry_invalid", EMBEDDERS_ERROR)
    key = embed_model_key(EMBED_MODEL if model_id is None else model_id)
    spec = EMBEDDERS.get(key)
    if spec is None:
        raise GatewayError(
            503,
            "embedder_unregistered",
            f"embedder {key or '(unset)'} is not in {EMBEDDERS_FILE}; add an entry "
            "declaring its pooling, dim, context and prefixes (empty prefixes are a "
            "valid declaration for a model whose card defines none)",
        )
    return spec


def embed_prefix(input_type, model_id=None):
    """Return the prefix for `input_type` under the configured embedder."""
    return embedder_spec(model_id)["prefixes"][input_type]

# CI/dev STUB mode: serve deterministic embed/rerank/synth with NO upstream
# llama-servers. The supervisor skips launching the per-role servers and the
# image can be built without baking the multi-GB GGUFs (Dockerfile arg), so e2e
# exercises the real kb -> gateway contract (and the /embed,/rerank,/v1/chat
# shapes) cheaply. Vectors are deterministic per-input so retrieval is stable.
STUB = os.environ.get("AIMEE_LLM_STUB", "") not in ("", "0", "false")


def _optional_positive_env_int(name):
    """Return a positive configured integer, or None to derive at runtime."""
    try:
        value = int(os.environ.get(name, "") or "0")
    except ValueError:
        return None
    return value if value > 0 else None


EXPECTED_EMBED_DIM = _optional_positive_env_int("AIMEE_EMBEDDING_DIM")
STUB_DIM = (_optional_positive_env_int("AIMEE_LLM_STUB_DIM")
            or _optional_positive_env_int("EMBEDDER_STUB_DIM")
            or EXPECTED_EMBED_DIM
            or 1024)


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
# all three. Streaming is relayed verbatim for OpenAI-compatible clients.
SYNTH_URL = os.environ.get("AIMEE_LLM_SYNTH_URL", "").rstrip("/")
SYNTH_MODE = os.environ.get("AIMEE_LLM_SYNTH_MODE", "local")
SYNTH_MODEL = os.environ.get("AIMEE_LLM_SYNTH_MODEL", "aimee-synth")


def _positive_env_int(name, default):
    """Compose commonly materializes unset optional values as empty strings."""
    try:
        value = int(os.environ.get(name, "") or default)
    except ValueError:
        value = default
    return value if value > 0 else default


SYNTH_SLOTS = _positive_env_int("AIMEE_LLM_SYNTH_SLOTS", 1)
SYNTH_CONTEXT = _positive_env_int("AIMEE_LLM_SYNTH_CTX", 32768)

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


class ClientDisconnected(Exception):
    """The downstream HTTP client closed its response socket."""


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


# The kb's readiness probe (embed-remote.py --dim) reads payload["dim"] from
# /health and treats a missing/absent dim as "embedder not ready" forever — so
# the real (non-stub) path must report it too, not just the stub. The embedder
# child doesn't expose its dim as metadata, so learn it once from an actual
# embedding and cache it; until the child can embed, /health simply omits dim
# (status won't be "ok" yet either).
_embed_dim = None


def embed_dim():
    global _embed_dim
    if _embed_dim is None:
        try:
            _embed_dim = len(do_embed("dim probe"))
        except Exception:  # noqa: BLE001 - child not ready; retry on a later probe
            return None
    return _embed_dim


def _validate_embed_dim(vec):
    """Enforce an optional wizard/operator pin; otherwise preserve native dim."""
    if EXPECTED_EMBED_DIM is not None and len(vec) != EXPECTED_EMBED_DIM:
        raise GatewayError(
            503,
            "embedding_dim_mismatch",
            f"embedder returned {len(vec)} dimensions; expected {EXPECTED_EMBED_DIM}",
        )
    return vec


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


def do_embed(text, input_type=INPUT_TYPE_DEFAULT):
    # The prefix is resolved (and an unregistered embedder refused) in STUB mode too, so
    # e2e exercises the same seam the real path takes rather than a laxer one.
    prefixed = embed_prefix(input_type) + text
    if STUB:
        vec = _stub_vec(prefixed)
    else:
        vec = _embeddings(EMBED_URL, prefixed)
    return _validate_embed_dim(vec)


def do_embed_batch(texts, input_type=INPUT_TYPE_DEFAULT):
    validate_batch(texts)
    prefix = embed_prefix(input_type)
    prefixed = [prefix + t for t in texts]
    if STUB:
        vecs = [_stub_vec(t) for t in prefixed]
    else:
        vecs = _embeddings(EMBED_URL, prefixed) if prefixed else []
    for vec in vecs:
        _validate_embed_dim(vec)
    return vecs


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

# A lost Vulkan/GPU device is unrecoverable in-process for a llama-server: after an
# amdgpu ring-reset / ErrorDeviceLost the child's /health still returns 200 but EVERY
# decode fails (HTTP 500 carrying one of these markers) forever, so the synth breaker
# latches open and nothing self-heals (the supervisor only restarts on a child EXIT,
# and a wedged child stays up). We detect the signature and tear the gateway down so
# the supervisor restarts the container with a fresh GPU context + a reset breaker.
# Markers are Vulkan-specific signatures that only appear in a llama-server 5xx body —
# deliberately NOT loose phrases like "device lost" that could occur in echoed input.
_DEVICE_LOST_MARKERS = ("ErrorDeviceLost", "ERROR_DEVICE_LOST", "vk::Queue::submit")
# Rate-limit the auto-restart via a timestamp recorded in a state file so a
# deterministically-bad GPU (e.g. it re-wedges on the first decode after every restart)
# does not thrash in a reload loop — after the guard trips we leave it for an operator
# (loud log). The file persists across a `docker restart` (same container), so the
# window survives the restart it triggers.
_DEVICE_LOST_STATE = os.environ.get("AIMEE_LLM_DEVICE_LOST_STATE",
                                    "/tmp/aimee-llm-device-lost.ts")
_DEVICE_LOST_MIN_INTERVAL = float(
    os.environ.get("AIMEE_LLM_DEVICE_LOST_RESTART_MIN_INTERVAL", "180"))


def _error_text(exc):
    """Best-effort text of an upstream failure; an HTTPError carries the JSON body with
    the llama-server error message (where the device-lost signature appears)."""
    if isinstance(exc, urllib.error.HTTPError):
        try:
            return exc.read().decode("utf-8", "replace")
        except Exception:  # noqa: BLE001
            return str(exc)
    return str(exc)


def _is_device_lost(text):
    return any(m in text for m in _DEVICE_LOST_MARKERS)


def _is_synth_device_lost(exc):
    """A GPU device-lost surfaces ONLY as a 5xx from the synth llama-server. Gate on
    that so a 4xx body that echoes request content can't spoof a marker and force a
    spurious container restart."""
    if isinstance(exc, urllib.error.HTTPError) and exc.code >= 500:
        return _is_device_lost(_error_text(exc))
    return False


def _device_lost_restart_allowed(now):
    """True iff we have not auto-restarted within the rate-limit window. Records |now|
    as the last-restart time when it returns True (so a re-wedge inside the window is
    held off). Best-effort: a missing/unreadable/unwritable state file means 'allowed'."""
    try:
        with open(_DEVICE_LOST_STATE) as fh:
            last = float(fh.read().strip() or 0)
    except (OSError, ValueError):
        last = 0.0
    if now - last < _DEVICE_LOST_MIN_INTERVAL:
        return False
    try:
        with open(_DEVICE_LOST_STATE, "w") as fh:
            fh.write(str(now))
    except OSError:
        pass
    return True


def _handle_device_lost():
    """A synth decode failed with a GPU device-lost. Schedule a container restart
    (deferred ~1s so the in-flight request still returns its error first), unless we
    restarted too recently — then leave it for an operator with a loud log."""
    sys.stderr.write("aimee-llm-gateway: synth GPU device lost (Vulkan) — the child cannot "
                     "recover its context in-process\n")
    if not _device_lost_restart_allowed(time.time()):
        sys.stderr.write("aimee-llm-gateway: NOT auto-restarting — another device-lost within "
                         f"{_DEVICE_LOST_MIN_INTERVAL:.0f}s (suspected hard GPU fault / "
                         "contention; investigate the host)\n")
        sys.stderr.flush()
        return
    sys.stderr.write("aimee-llm-gateway: restarting the container to recover a fresh GPU "
                     "context\n")
    sys.stderr.flush()
    threading.Timer(1.0, lambda: os._exit(75)).start()  # supervisor reaps -> container restart


def do_synth(body):
    """Proxy a /v1/chat/completions request to the configured synth upstream
    (AIMEE_LLM_SYNTH_URL — a local baked llama-server, or a forwarded/external base).
    A circuit breaker (§4) opens on repeated upstream errors -> typed 503.

    `stream:true` is proxied verbatim: see do_synth_stream. It used to return a typed
    400 ("disabled in this release"), which made this gateway unusable as an
    OpenAI-chat backend for anything that streams."""
    if not isinstance(body, dict):
        raise GatewayError(400, "bad_request", "chat/completions expects a JSON object")
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
    except Exception as exc:
        _synth_breaker.record(False)
        if _is_synth_device_lost(exc):
            _handle_device_lost()
        raise
    _synth_breaker.record(True)
    return out



def do_synth_stream(body, write):
    """Proxy a streaming /v1/chat/completions to the synth upstream, relaying the SSE
    bytes VERBATIM via `write`.

    Verbatim matters: the caller is an OpenAI-chat client and the upstream already
    speaks OpenAI-chat SSE, so re-encoding here would only add a place for the two to
    disagree. The gateway's job is auth + breaker + audit, not translation.

    Returns the number of body bytes relayed (for the metadata-only audit). Raises
    GatewayError before any bytes are written so the caller can still send a typed
    error; once relaying starts the status line is already committed."""
    if not isinstance(body, dict):
        raise GatewayError(400, "bad_request", "chat/completions expects a JSON object")
    if STUB:
        # Deterministic two-chunk stream so CI can exercise the path with no upstream.
        chunks = [
            b'data: {"choices":[{"index":0,"delta":{"role":"assistant","content":"stub"}}]}\n\n',
            b'data: {"choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}\n\n',
            b'data: [DONE]\n\n',
        ]
        n = 0
        for c in chunks:
            write(c)
            n += len(c)
        return n
    if not SYNTH_URL:
        raise GatewayError(503, "synth_unconfigured", "AIMEE_LLM_SYNTH_URL is not set")
    if not _synth_breaker.allow():
        raise GatewayError(503, "provider_unavailable", "synth upstream circuit is open")

    req = urllib.request.Request(
        f"{SYNTH_URL}/v1/chat/completions",
        data=json.dumps(body).encode("utf-8"),
        headers={"content-type": "application/json"},
        method="POST",
    )
    total = 0
    try:
        with urllib.request.urlopen(req, timeout=300) as r:
            while True:
                chunk = r.read(4096)
                if not chunk:
                    break
                write(chunk)
                total += len(chunk)
    except ClientDisconnected:
        # A downstream disconnect says nothing about synth upstream health.
        raise
    except Exception as exc:
        _synth_breaker.record(False)
        if _is_synth_device_lost(exc):
            _handle_device_lost()
        raise
    _synth_breaker.record(True)
    return total


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


def model_catalog():
    """OpenAI-compatible discovery for `aimee agent local` onboarding."""
    available = STUB or bool(SYNTH_URL)
    data = []
    if available:
        data.append({"id": SYNTH_MODEL, "object": "model", "owned_by": "aimee"})
    return {"object": "list", "data": data}


def slot_catalog():
    """llama-server-compatible capacity discovery for the local-agent probe.

    llama.cpp divides --ctx-size across --parallel slots, so report the usable
    per-request window rather than the aggregate allocation.
    """
    per_slot_context = max(1, SYNTH_CONTEXT // SYNTH_SLOTS)
    return [{"id": slot, "n_ctx": per_slot_context} for slot in range(SYNTH_SLOTS)]


def build_server():
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):  # quiet
            pass

        def _send(self, code, payload):
            body = json.dumps(payload).encode("utf-8")
            try:
                self.send_response(code)
                self.send_header("content-type", "application/json")
                self.send_header("content-length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                # A caller can disappear while synth is still generating (for
                # example, when the KB is restarted).  There is no socket left on
                # which to report another error, so finish the handler quietly.
                return False
            return True

        def _send_sse_head(self):
            """SSE headers. Sent BEFORE the first relayed byte; after this the status is
            committed, so upstream failures can no longer become a typed JSON error."""
            self.send_response(200)
            self.send_header("content-type", "text/event-stream")
            self.send_header("cache-control", "no-cache")
            self.send_header("connection", "close")
            self.end_headers()

        def do_GET(self):
            path = self.path.rstrip("/")
            if path == "/v1/models":
                self._send(200, model_catalog())
            elif path == "/slots":
                self._send(200, slot_catalog())
            elif path == "/health":
                if STUB:
                    # Report the dim so the kb's embedder-autodim sizes the schema
                    # (the embed-remote.py --dim probe reads payload["dim"]).
                    self._send(200, {"status": "ok", "model": "aimee-llm-stub", "dim": STUB_DIM})
                    return
                st = health_state(child_health())
                payload = {"status": st, "model": EMBED_MODEL}
                if st == "ok":
                    dim = embed_dim()
                    if dim:
                        payload["dim"] = dim
                    else:
                        # embedder reachable but not serving embeddings yet: the kb
                        # gates on dim, so don't report ok without one.
                        st = "loading"
                        payload["status"] = st
                self._send(200 if st == "ok" else 503, payload)
            elif path in ("/health/embed", "/health/rerank", "/health/synth"):
                role = path.rsplit("/", 1)[1]
                st = role_states()[role]
                # ready=200 so a probe can gate on a single role (the kb hits
                # /health/embed so it indexes as soon as embed is ready).
                self._send(200 if st == "ready" else 503, {"status": st, "role": role})
            else:
                self._send(404, {"error": "not found"})

        def do_POST(self):
            # input_type rides in the query string: /embed's body is raw UTF-8 text with
            # no envelope to extend, and /embed_batch's is a bare JSON array, so neither
            # body can carry it without breaking the existing wire contract.
            split = urllib.parse.urlsplit(self.path)
            path = split.path.rstrip("/")
            query = urllib.parse.parse_qs(split.query)
            try:
                input_type = parse_input_type((query.get("input_type") or [None])[0])
                # §1a: every work request is authenticated; the scope is DERIVED from the
                # identity (a caller-supplied X-Aimee-Scope is rejected). Authenticate
                # before reading the body so an untrusted bridge peer cannot hold a
                # worker or force allocations with an unauthenticated Content-Length.
                check_auth(self.headers.get("Authorization"))
                scope = derive_scope(self.headers.get)
                n = int(self.headers.get("content-length", "0") or "0")
                raw = self.rfile.read(n) if n else b""
                if path == "/auth/verify":
                    # Cheap deploy-time proof that the request came from a caller
                    # holding the configured service identity. No model role has
                    # to be enabled, and no credential is reflected in the body.
                    self._send(200, {"status": "ok", "scope": scope})
                elif path == "/embed":
                    text = raw.decode("utf-8", errors="replace")
                    if not text.strip():
                        raise GatewayError(400, "bad_request", "empty input")
                    self._send(200, do_embed(text, input_type))
                elif path == "/embed_batch":
                    self._send(200, do_embed_batch(json.loads(raw or b"[]"), input_type))
                elif path == "/rerank":
                    self._send(200, do_rerank(json.loads(raw or b"[]")))
                elif path == "/v1/chat/completions":
                    req_body = json.loads(raw or b"{}")
                    if isinstance(req_body, dict) and req_body.get("stream"):
                        started = []

                        def _w(b):
                            try:
                                if not started:
                                    self._send_sse_head()
                                    started.append(True)
                                self.wfile.write(b)
                                self.wfile.flush()
                            except (BrokenPipeError, ConnectionResetError) as exc:
                                raise ClientDisconnected from exc

                        n_out = do_synth_stream(req_body, _w)
                        audit("chat", scope, "ok", bytes_in=len(raw), bytes_out=n_out)
                    else:
                        out = do_synth(req_body)
                        # §4 audit: metadata-only (scope/outcome/bytes), never content/completions
                        audit("chat", scope, "ok", bytes_in=len(raw),
                              bytes_out=len(json.dumps(out)))
                        self._send(200, out)
                else:
                    self._send(404, {"error": "not found"})
            except ClientDisconnected:
                # Streaming has already committed its status and the peer is gone.
                # In particular, do not fall through and attempt a second 500 write.
                return
            except GatewayError as ge:
                if path == "/v1/chat/completions":
                    audit("chat", SCOPE_DEFAULT, "error", code=ge.body["error"]["code"])
                self._send(ge.status, ge.body)
            except json.JSONDecodeError as exc:
                # A body this gateway cannot parse is the CALLER's error. It used to
                # fall through to the blanket handler below and come back as 500
                # "internal", which tells an operator their inference gateway broke
                # when in fact the request was malformed — and makes a client's retry
                # logic treat it as a transient server fault worth retrying, forever.
                if path == "/v1/chat/completions":
                    audit("chat", SCOPE_DEFAULT, "error", code="bad_request")
                self._send(400, {"error": {"code": "bad_request",
                                           "message": f"invalid JSON body: {exc}"}})
            except Exception as exc:  # noqa: BLE001
                self._send(500, {"error": {"code": "internal", "message": str(exc)}})

    return ThreadingHTTPServer((BIND, PORT), Handler)


def embedder_descriptor_shell(model_id):
    """Emit the configured embedder's registry entry as shell assignments.

    This is how the supervisor stops carrying its own copy of the coordinates: it evals
    this instead of a `case` statement, so the shell and the gateway cannot disagree
    about which model, pooling or width is being served. Raises EmbedderRegistryError on
    an unregistered model or one whose provisioning coordinates are blank — both are
    operator errors that must stop the boot, not be papered over with a default.
    """
    registry = load_embedders()
    key = embed_model_key(model_id)
    spec = registry.get(key)
    if spec is None:
        raise EmbedderRegistryError(
            f"embedder {key or '(unset)'} is not in {EMBEDDERS_FILE}; registered: "
            f"{', '.join(sorted(registry)) or '(none)'}"
        )
    blank = [f for f in EMBEDDER_PROVISION_FIELDS if not str(spec.get(f, "")).strip()]
    if blank:
        raise EmbedderRegistryError(
            f"embedder {key} cannot be served: {', '.join(blank)} "
            f"{'is' if len(blank) == 1 else 'are'} blank in {EMBEDDERS_FILE}"
        )
    fields = {
        "EMBED_MODEL_KEY": key,
        "EMBED_REPO": spec["repo"],
        "EMBED_FILE": spec["file"],
        "EMBED_REVISION": spec["revision"],
        "EMBED_SHA256": spec["sha256"],
        "EMBED_POOLING": spec["pooling"],
        "EMBED_DIM": spec["dim"],
        "EMBED_CONTEXT": spec["context"],
    }
    return "".join(f"{name}={shlex.quote(str(value))}\n" for name, value in fields.items())


def main():  # pragma: no cover
    # Refuse to start on a broken registry. Serving embeddings with unknown pooling or
    # prefixes is undetectable downstream, so a startup failure is the cheap outcome.
    if EMBEDDERS_ERROR is not None:
        sys.stderr.write(f"aimee-llm-gateway: {EMBEDDERS_ERROR}\n")
        sys.stderr.flush()
        return 2
    # §1a bind guard: refuse (strict) or warn on an unauthenticated wildcard bind.
    if validate_bind(BIND, AUTH_TOKEN, STRICT_BIND) == "warn":
        audit("startup", SCOPE_DEFAULT, "warn", bind=BIND, auth="off",
              note="unauthenticated wildcard bind; deployment network is the boundary")
    srv = build_server()
    sys.stderr.write(f"aimee-llm-gateway: {BIND}:{PORT} embed={EMBED_URL} rerank={RERANK_URL} "
                     f"auth={'on' if AUTH_TOKEN else 'off'}\n")
    sys.stderr.flush()
    srv.serve_forever()
    return 0


def cli(argv):  # pragma: no cover - thin argv dispatch, behaviour covered per-callee
    """`--embedder-descriptor [model]` prints shell assignments for the supervisor to
    eval; no arguments runs the gateway."""
    if argv[:1] == ["--embedder-descriptor"]:
        model = argv[1] if len(argv) > 1 else EMBED_MODEL
        try:
            sys.stdout.write(embedder_descriptor_shell(model))
        except EmbedderRegistryError as exc:
            sys.stderr.write(f"aimee-llm: {exc}\n")
            return 2
        return 0
    if argv:
        sys.stderr.write(f"aimee-llm: unknown arguments: {' '.join(argv)}\n")
        return 2
    return main()


if __name__ == "__main__":  # pragma: no cover
    sys.exit(cli(sys.argv[1:]))
