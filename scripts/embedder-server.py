#!/usr/bin/env python3
"""embedder-server.py: the aimee-kb container's in-container embedder.

Runs INSIDE the kb container on loopback, holding the model resident so the kb
embeds without a per-call reload and without leaving the container. There is no
separate embedder or aimee-llm service on this path any more: the weights are baked
into the kb image and this process serves them.

WHY IT OWNS THE PREFIXES. The kb declares only POLARITY — query or document — and
the prefix belonging to each model is deployment data, not caller data. Whatever sits
closest to the model has to apply it, because a prefix-dependent embedder served bare
is a silent quality regression: nomic measures 0.5823 NDCG@10 without its card
prefixes against 0.6075 with them, no error either way. That used to be the aimee-llm
gateway's job; with the embedder in-container it is this file's, reading the same
scripts/embedders.json everything else reads.

Endpoints:
  POST /embed        raw UTF-8 text          -> JSON float array (model dim)
  POST /embed_batch  JSON [text, ...]        -> JSON [[float, ...], ...]
  GET  /health       -> {"status":..., "model":..., "dim":N, "serving_id":...}

Both embed endpoints take `?input_type=query|document`; omitted means `document`,
since every ingest path is a document and only queries need to opt in. It rides in the
query string because neither body has an envelope to extend.

`serving_id` is the identity of the VECTOR SPACE being served — the model plus a
digest over its pooling and prefix pair. The kb records it against its corpus and
refuses to start when it changes, because a pooling or prefix change rewrites every
vector while leaving both the width and the model name alone.

Config (env):
  EMBEDDER_PORT     listen port (default 8080)
  EMBEDDER_MODEL    registry id of the embedder to serve (see EMBEDDERS_FILE)
  EMBEDDERS_FILE    registry path (default /opt/aimee/embedders.json)
  EMBEDDERS_EXTRA   operator overlay merged over it
  EMBEDDER_THREADS  torch intra-op threads (default min(8, ncpu))
  EMBEDDER_QUANTIZE fp32 (default) | int8 (torch dynamic; ~3.3x faster, drifts)

Dependencies: torch, "sentence-transformers>=3.3", "transformers>=5.2", einops
"""

import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ---- the embedder registry ----
# The same file the kb and the setup wizard read. Every per-model fact that changes the
# vectors lives there — pooling, width, context, prefixes — keyed by model identity, so a
# swap cannot silently inherit the previous model's settings. That failure has happened
# twice here: pooling defaulted to `last` (right for Qwen3, wrong for nomic) and prefixes
# were absent entirely.
EMBEDDERS_FILE = os.environ.get("EMBEDDERS_FILE", "/opt/aimee/embedders.json")
EMBEDDERS_EXTRA = os.environ.get("EMBEDDERS_EXTRA", "")
INPUT_TYPES = ("query", "document")
# Ingest is the overwhelming majority of embed traffic and every ingest path is a
# document, so an omitted input_type means `document`. It must NOT mean "no prefix":
# defaulting to bare text is exactly the regression the registry exists to prevent.
INPUT_TYPE_DEFAULT = "document"
EMBEDDER_REQUIRED_FIELDS = ("repo", "revision", "pooling", "dim", "context", "prefixes")


class RegistryError(RuntimeError):
    """The registry is missing or malformed — an operator error, surfaced at startup."""


def _load_registry_file(path):
    with open(path, "r", encoding="utf-8") as handle:
        document = json.load(handle)
    if not isinstance(document, dict):
        raise RegistryError(f"{path}: top level must be an object")
    table = document.get("embedders")
    if not isinstance(table, dict) or not table:
        raise RegistryError(f"{path} declares no embedders")
    out = {}
    for model_id, spec in table.items():
        if not isinstance(spec, dict):
            raise RegistryError(f"{path}: entry {model_id!r} is not an object")
        missing = [f for f in EMBEDDER_REQUIRED_FIELDS if f not in spec]
        if missing:
            raise RegistryError(f"{path}: entry {model_id!r} is missing {', '.join(missing)}")
        prefixes = spec["prefixes"]
        if not isinstance(prefixes, dict) or set(prefixes) != set(INPUT_TYPES):
            raise RegistryError(
                f"{path}: entry {model_id!r} must declare prefixes for exactly "
                f"{', '.join(INPUT_TYPES)} (empty strings if its card defines none)"
            )
        if not all(isinstance(prefixes[side], str) for side in INPUT_TYPES):
            raise RegistryError(f"{path}: entry {model_id!r} has a non-string prefix")
        out[model_key(model_id)] = spec
    return out


def model_key(model_id):
    """Normalise an id to its registry key. Ids carry deployment decoration — an @rev
    suffix (as the db2 model records use) or case differences — none of which change
    which prefixes the model was trained with."""
    return (model_id or "").split("@", 1)[0].strip().lower()


def load_registry():
    """Parse the registry, applying the operator overlay over it. Raises rather than
    degrading: every degraded mode here is silent downstream."""
    try:
        registry = _load_registry_file(EMBEDDERS_FILE)
    except (OSError, ValueError) as exc:
        raise RegistryError(f"cannot read embedder registry {EMBEDDERS_FILE}: {exc}") from exc
    if EMBEDDERS_EXTRA:
        try:
            registry.update(_load_registry_file(EMBEDDERS_EXTRA))
        except (OSError, ValueError) as exc:
            raise RegistryError(f"cannot read embedder overlay {EMBEDDERS_EXTRA}: {exc}") from exc
    return registry


# `or` rather than a get() default: an EMPTY EMBEDDER_MODEL is set-but-blank, which a
# default argument would not catch — and a caller that exports "" means "I did not
# choose", not "serve the model called empty string".
try:
    REGISTRY = load_registry()
    REGISTRY_ERROR = None
except RegistryError as exc:
    REGISTRY = {}
    REGISTRY_ERROR = str(exc)

# Unset falls back to the SOLE registered model rather than a hardcoded name: with one
# bundled embedder "unset" has exactly one sensible answer, and naming it here would be a
# second place to update. Ambiguous (more than one registered, none chosen) is left empty
# so the refusal names what is available instead of picking for the operator.
_DEFAULT_ID = next(iter(REGISTRY)) if len(REGISTRY) == 1 else ""
EMBEDDER_ID = model_key(os.environ.get("EMBEDDER_MODEL") or _DEFAULT_ID)

SPEC = REGISTRY.get(EMBEDDER_ID)
# The repo id sentence-transformers loads. Refusing an unregistered model is the point:
# serving it would mean guessing its pooling and prefixes.
MODEL_NAME = str(SPEC["repo"]) if SPEC else ""
MODEL_REVISION = str(SPEC.get("revision") or "main") if SPEC else "main"


def prefix_for(input_type):
    """The prefix for `input_type` under the configured embedder."""
    return SPEC["prefixes"][input_type] if SPEC else ""


def serving_id():
    """Identity of the vector space served: model + digest over pooling and prefixes.

    A dim is not enough and a model name is not enough — pooling and prefixes change
    every vector while leaving both alone. Empty when the model is unregistered, so the
    kb's guard stays a no-op rather than refusing on a value that was never meaningful.
    """
    if not SPEC:
        return ""
    import hashlib

    material = "\x00".join((
        EMBEDDER_ID,
        str(SPEC["pooling"]),
        SPEC["prefixes"]["query"],
        SPEC["prefixes"]["document"],
    ))
    return f"{EMBEDDER_ID}/{hashlib.sha256(material.encode('utf-8')).hexdigest()[:16]}"


def parse_input_type(value):
    """Validate a caller-supplied input_type, or None when it is not one of ours."""
    if value is None or value == "":
        return INPUT_TYPE_DEFAULT
    return value if value in INPUT_TYPES else None
def _env_int(name, default, fallback=None):
    """Integer from the environment, treating EMPTY as unset.

    Compose passes an unset variable through as an EMPTY STRING --
    `EMBEDDER_THREADS: "${EMBEDDER_THREADS:-}"` yields "", not absence -- so
    os.environ.get(name, "0") returns "" and int("") raises. That killed the
    embedder at import on every deployment that did not explicitly set the
    variable: the KB never bound its port, its health check failed forever, and
    the wizard reported a KB that would not come up. The traceback named this
    line, but the value looked unset, which is what made it puzzling.

    A junk value is treated the same way. An embedder that refuses to start is
    worse than one that ignores EMBEDDER_THREADS=banana and logs the default.
    """
    raw = (os.environ.get(name) or "").strip()
    if raw:
        try:
            return int(raw)
        except ValueError:
            print("embedder-server: %s=%r is not an integer; using the default"
                  % (name, raw), file=sys.stderr, flush=True)
    return default() if callable(default) else default


PORT = _env_int("EMBEDDER_PORT", 8080)
# CPU serving tuning. A single short embed does not scale past ~8 intra-op
# threads — on a 32-core host pplx-embed-0.6b is 269ms at 32 threads but 189ms at
# 8 (per-call thread overhead dominates the tiny workload). Cap to a sane default;
# an explicit EMBEDDER_THREADS wins. Set OMP before torch is imported.
def _usable_cpus():
    """CPUs this process may actually run on.

    os.cpu_count() reports the HOST's CPUs and ignores the cgroup/affinity mask a
    container is confined to. On the bench container it answers 8 while the
    process is pinned to 4, so torch was told to run 8 intra-op threads on 4
    usable cores -- and with several ingest workers issuing concurrent batches
    that is dozens of threads contending for a handful of cores. Measured cost of
    that thrash on bekko-a25m (a 25M-parameter model): 1727 ms for a single
    ~512-token text, against tens of ms when the thread count matches reality.

    sched_getaffinity is what nproc uses and what the scheduler honours.
    """
    try:
        return len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        return os.cpu_count() or 1


EMBEDDER_THREADS = _env_int("EMBEDDER_THREADS", lambda: min(8, _usable_cpus()))
# onnx | torch | auto. "auto" prefers onnx and falls back; "onnx" refuses to serve
# on the slow path, which is what a benchmark host wants -- silently serving fp32
# torch is how a 50x regression went unnoticed.
EMBEDDER_BACKEND = os.environ.get("EMBEDDER_BACKEND", "auto").strip().lower()
_runtime = "unloaded"
os.environ.setdefault("OMP_NUM_THREADS", str(EMBEDDER_THREADS))
# Optional int8 dynamic quantization. Pure torch (no optimum/onnx) rather than the
# q4 ONNX path.
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
STUB_DIM = int(os.environ.get("EMBEDDER_STUB_DIM", os.environ.get("EMBEDDER_DIMS", "2560")) or "2560")

_model = None
_dim = 0
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
    # revision pinned from the registry: the weights are baked at build time, and a
    # floating ref would let an image rebuild change the vector space silently.
    # ONNX FIRST. onnxruntime is the CPU path this embedder was characterised on;
    # fp32 torch is the fallback, not the intent. Measured on 4 cores with a
    # 25M-parameter model, torch costs ~2000 ms for a single ~512-token text,
    # which made one source file take 66 s to embed and corpus ingest run at
    # ~20 s/file. The base onnx graph is baked alongside the safetensors.
    #
    # Fall back rather than fail: a deployment whose image predates the baked
    # graph, or one built with a different embedder, must still serve. The path
    # actually taken is reported in /health as `runtime`, so "is this the fast
    # one?" is answerable without reading logs.
    global _runtime
    _model = None
    if EMBEDDER_BACKEND in ("onnx", "auto"):
        try:
            # Load from the LOCAL SNAPSHOT DIRECTORY, not the repo id.
            #
            # Given a repo id, sentence-transformers lists the repo tree over the
            # network to decide which onnx artefact to load -- before it honours
            # file_name -- so under HF_HUB_OFFLINE=1 it fails with "Cannot reach
            # https://huggingface.co/api/models/.../tree/main: offline mode is
            # enabled" and silently drops onto fp32 torch. local_files_only and
            # an explicit file_name do not prevent that listing.
            #
            # snapshot_download(local_files_only=True) resolves the already-baked
            # directory without touching the network, and a local path gives ST
            # nothing to enumerate. Loading through ST rather than driving
            # onnxruntime directly keeps its pooling and normalisation exactly as
            # the torch path had them, so the vector space does not move.
            from huggingface_hub import snapshot_download

            local_dir = snapshot_download(MODEL_NAME, revision=MODEL_REVISION,
                                          local_files_only=True)
            _model = SentenceTransformer(
                local_dir, trust_remote_code=True, backend="onnx",
                model_kwargs={"file_name": "onnx/model.onnx"})
            _runtime = "onnx"
        except Exception as exc:  # missing optimum/onnxruntime, or no baked graph
            if EMBEDDER_BACKEND == "onnx":
                raise
            sys.stderr.write(f"embedder-server: onnx unavailable ({exc}); using torch\n")
            _model = None
    if _model is None:
        _model = SentenceTransformer(MODEL_NAME, revision=MODEL_REVISION, trust_remote_code=True)
        _runtime = "torch"
    _dim = _model.get_sentence_embedding_dimension() or 0  # read before quantizing
    if EMBEDDER_QUANTIZE == "int8":
        import torch.ao.quantization as ao_q

        _model = ao_q.quantize_dynamic(_model, {torch.nn.Linear}, dtype=torch.qint8)
    sys.stderr.write(
        f"embedder-server: loaded {MODEL_NAME} dim={_dim} threads={EMBEDDER_THREADS}"
        f" quant={EMBEDDER_QUANTIZE} runtime={_runtime}\n"
    )
    return _model


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


def _refuse_reason():
    """Why this process cannot serve, or None. Refusing beats guessing: an unregistered
    model has no declared pooling or prefixes, and serving it bare is undetectable
    downstream."""
    if REGISTRY_ERROR:
        return REGISTRY_ERROR
    if not SPEC:
        return (f"embedder {EMBEDDER_ID or '(unset)'} is not in {EMBEDDERS_FILE}; registered: "
                f"{', '.join(sorted(REGISTRY)) or '(none)'}")
    return None


def _background_load():
    """Fetch (cold volume) + load the model off the request path: once it loads,
    /health flips to "ok" and /embed serves."""
    global _load_status, _load_error, _dim
    reason = _refuse_reason()
    if reason:
        _load_error = reason
        _load_status = "error"
        sys.stderr.write(f"embedder-server: {reason}\n")
        sys.stderr.flush()
        return
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
    sys.stderr.flush()


def embed(text: str, input_type=INPUT_TYPE_DEFAULT):
    # The prefix is applied here, closest to the model, and in STUB mode too — an e2e
    # that skipped it would exercise a laxer contract than production.
    prefixed = prefix_for(input_type) + text
    if EMBEDDER_STUB:
        return _stub_embed(prefixed)
    if EMBEDDER_BATCH_WINDOW_MS <= 0:
        vec = _model.encode(prefixed, normalize_embeddings=True)
        return vec.tolist()
    # n=1 is the case the coalescer exists for: a lone text costs 58.7 ms on its
    # own and 11.7 ms as part of a full batch. Joining whatever else is in flight
    # is worth a bounded wait of a few milliseconds.
    return _encode_coalesced([prefixed])[0]


# --- dynamic batching -------------------------------------------------------
#
# Callers do not control how much work arrives per request, and the kb's document
# ingest cannot: it embeds one file at a time, and a source file is usually 1-5
# chunks. Measured on this model, batch size dominates everything else --
#
#     n=1   58.7 ms/text        n=32   13.0 ms/text
#     n=8   15.3 ms/text        n=128  11.7 ms/text
#
# -- so a fleet of ingest workers each sending its own small batch pays a ~5x
# penalty on nearly every chunk while the server sits underused. Coalescing here
# fixes it for every caller at once, without each of them having to restructure
# to accumulate work: several concurrent small requests become one large
# model.encode(), which is what the GPU/CPU wants anyway.
#
# The window is short (default 15 ms) because it is pure added latency for a
# request that arrives when the queue is empty -- an interactive query embed must
# not wait meaningfully. Set EMBEDDER_BATCH_WINDOW_MS=0 to disable coalescing.
EMBEDDER_BATCH_WINDOW_MS = _env_int("EMBEDDER_BATCH_WINDOW_MS", 15)
EMBEDDER_BATCH_MAX = _env_int("EMBEDDER_BATCH_MAX", 128)

_batch_lock = threading.Lock()
_batch_cv = threading.Condition(_batch_lock)
_batch_pending = []          # [_BatchItem]
_batch_worker_started = False
_tokenizer = None


class _BatchItem:
    __slots__ = ("prefixed", "vec", "error", "done")

    def __init__(self, prefixed):
        self.prefixed = prefixed
        self.vec = None
        self.error = None
        self.done = threading.Event()


def _token_count(texts):
    """Real token count via the model's own tokenizer, cached after first use.

    Chunking targets ~512 APPROXIMATE tokens; the tokenizer is the authority on
    what the model actually processes, and the gap between the two is exactly
    what this is here to expose.
    """
    global _tokenizer
    if _tokenizer is None:
        tok = getattr(_model, "tokenizer", None)
        if tok is None:
            return -1
        _tokenizer = tok
    return sum(len(_tokenizer(t)["input_ids"]) for t in texts)


def _run_encode(items):
    """One model call for a group of items; distribute or fail them together."""
    _t0 = time.monotonic()
    _chars = sum(len(i.prefixed) for i in items)
    try:
        vecs = _model.encode([i.prefixed for i in items], normalize_embeddings=True)
        for item, vec in zip(items, vecs):
            item.vec = vec.tolist()
    except Exception as exc:  # noqa: BLE001 - surfaced per waiting request
        for item in items:
            item.error = exc
    finally:
        # INSTRUMENTATION: what is actually being handed to the model, and what it
        # costs. Rate in production sat ~13x below a standalone benchmark of this
        # same model, so the question is whether the inputs match what was
        # benchmarked (batch size and, more importantly, sequence length -- the
        # chunker targets "approximate tokens" and attention is quadratic).
        _el = time.monotonic() - _t0
        _n = len(items)
        try:
            _tok = _token_count([i.prefixed for i in items])
        except Exception:  # noqa: BLE001 - never fail an embed to log it
            _tok = -1
        sys.stderr.write(
            "embed-batch n=%d chars=%d tokens=%d %.3fs %.1f ms/text %.0f tok/s\n"
            % (_n, _chars, _tok, _el, _el * 1000.0 / max(_n, 1),
               (_tok / _el) if (_tok > 0 and _el > 0) else 0.0))
        sys.stderr.flush()
        for item in items:
            item.done.set()


def _batch_loop():
    """Collect whatever arrived within the window, then encode it as one batch."""
    while True:
        with _batch_cv:
            while not _batch_pending:
                _batch_cv.wait()
            # A short settle so sibling requests in flight join this batch. Bounded
            # by EMBEDDER_BATCH_MAX so a burst cannot grow one call without limit.
            deadline = time.monotonic() + (EMBEDDER_BATCH_WINDOW_MS / 1000.0)
            while len(_batch_pending) < EMBEDDER_BATCH_MAX:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                _batch_cv.wait(remaining)
            items = _batch_pending[:EMBEDDER_BATCH_MAX]
            del _batch_pending[:len(items)]
        _run_encode(items)


def _ensure_batch_worker():
    global _batch_worker_started
    with _batch_lock:
        if _batch_worker_started:
            return
        _batch_worker_started = True
        threading.Thread(target=_batch_loop, name="embed-batcher", daemon=True).start()


def _encode_coalesced(prefixed_list):
    """Encode via the shared batcher so concurrent callers share one model call."""
    _ensure_batch_worker()
    items = [_BatchItem(p) for p in prefixed_list]
    with _batch_cv:
        _batch_pending.extend(items)
        _batch_cv.notify()
    out = []
    for item in items:
        item.done.wait()
        if item.error is not None:
            raise item.error
        out.append(item.vec)
    return out


def embed_batch(texts, input_type=INPUT_TYPE_DEFAULT):
    """Embed a list of texts and return vectors aligned 1:1 with `texts`.

    The caller's list is a lower bound on batch size, not the batch: requests in
    flight from other callers are coalesced into the same model call. A caller
    that can batch well still should -- it saves HTTP round trips -- but one that
    cannot is no longer penalised for it."""
    prefix = prefix_for(input_type)
    prefixed = [prefix + t for t in texts]
    if EMBEDDER_STUB:
        return [_stub_embed(t) for t in prefixed]
    if not prefixed:
        return []
    if EMBEDDER_BATCH_WINDOW_MS <= 0:
        vecs = _model.encode(prefixed, normalize_embeddings=True)
        return [v.tolist() for v in vecs]
    return _encode_coalesced(prefixed)


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
                "model": EMBEDDER_ID or MODEL_NAME,
                "repo": MODEL_NAME,
                "dim": _dim,
                "quantize": EMBEDDER_QUANTIZE,
                # Which runtime is actually serving. Reported because the fast
                # path (onnx) and the ~50x slower fallback (fp32 torch) are
                # otherwise indistinguishable from outside, and a silent fallback
                # is exactly how corpus ingest ended up at ~20 s/file.
                "runtime": _runtime,
                "threads": EMBEDDER_THREADS,
            }
            # Registry data, not a measurement, so it is answerable while the model
            # loads — the kb reads it before it can embed anything.
            sid = serving_id()
            if sid:
                payload["serving_id"] = sid
            if REGISTRY_ERROR:
                payload["error"] = REGISTRY_ERROR
            elif _load_status == "error":
                payload["error"] = _load_error
            self._send(200 if _load_status == "ok" else 503, payload)
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        split = self.path.split("?", 1)
        path = split[0].rstrip("/")
        raw_type = ""
        if len(split) == 2:
            for part in split[1].split("&"):
                if part.startswith("input_type="):
                    raw_type = part[len("input_type="):]
        input_type = parse_input_type(raw_type)
        if input_type is None:
            self._send(400, {"error": f"input_type must be one of {', '.join(INPUT_TYPES)}"})
            return
        if path not in ("/embed", "/embed_batch"):
            self._send(404, {"error": "not found"})
            return
        # Refuse work until the model is loaded — never serve against a not-yet-
        # loaded model (no 0-vector fallback). The KB treats 503 as "warming up".
        if _load_status != "ok":
            self._send(503, {"error": "embedder warming up", "status": _load_status})
            return
        length = int(self.headers.get("content-length", "0") or "0")
        raw = self.rfile.read(length) if length else b""
        if path == "/embed_batch":
            try:
                texts = json.loads(raw.decode("utf-8", errors="replace") or "[]")
                if not isinstance(texts, list):
                    self._send(400, {"error": "embed_batch expects a JSON array of strings"})
                    return
                self._send(200, embed_batch(texts, input_type))
            except Exception as exc:  # noqa: BLE001
                self._send(500, {"error": str(exc)})
            return
        text = raw.decode("utf-8", errors="replace")
        if not text.strip():
            self._send(400, {"error": "empty input"})
            return
        try:
            self._send(200, embed(text, input_type))
        except Exception as exc:  # noqa: BLE001
            self._send(500, {"error": str(exc)})


def main():
    # Serve immediately; load the model in the background. On a cold volume the
    # first-boot fetch of a multi-GB model takes minutes, and blocking here would
    # trip the healthcheck before the server is even up. /health reports "loading"
    # (503) until ready; /embed returns 503 meanwhile.
    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    threading.Thread(target=_background_load, daemon=True).start()
    sys.stderr.write(f"embedder-server: serving on :{PORT}; loading {MODEL_NAME} in background\n")
    sys.stderr.flush()
    server.serve_forever()


if __name__ == "__main__":
    main()
