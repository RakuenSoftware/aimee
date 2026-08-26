#!/usr/bin/env python3
"""embed-remote.py: thin embedding_command client for the embedder service.

Same contract as embed-minilm.py (stdin text -> stdout JSON float array), but
instead of loading the model in-process it POSTs to the persistent
embedder-server.py service, so embedding stays fast inside the aimee-kb
container. Stdlib only — no torch in the kb image.

Contract (platform_exec_pipe in src/memory_core_scope_embed.inc):
  stdin:  raw UTF-8 text
  stdout: JSON float array  [0.123, -0.456, ...]  (L2-normalised). The dimension
          is whatever the pinned embedder emits — for example 384 for the shipped
          Bekko tier or 768 for Nomic — NOT a fixed size;
          probe it with `--dim`. `--serving-id` prints the endpoint's vector-space
          identity (empty when it reports none).
  exit 0 on success; non-zero on error (C caller logs a warning and skips)

Config (env), in precedence order:
  EMBEDDER_URL  base URL of the embedder service (pins the embedder)
  SYNTHESIS_ENDPOINT       DEPRECATED for embedding: synthesis-only since the aimee-llm
                      container was retired. Still read as a last resort so an older
                      deployment keeps working, but EMBEDDER_URL is the knob (
                      embed + synth); used when EMBEDDER_URL is unset
  SYNTHESIS_API_KEY bearer service identity for authenticated gateways
  SYNTHESIS_AUTH_REQUIRED=1 refuse requests when that identity is missing
  (unset)             no embedder configured; reported immediately so the caller
                      can use its builtin path. Pin the legacy compose service
                      with EMBEDDER_URL=http://embedder:8080 if wanted.

Usage:
  embedding_command: "python3 /opt/aimee/scripts/embed-remote.py"
"""

import json
import os
import sys
import urllib.error
import urllib.request

# No implicit legacy fallback. This used to default to http://embedder:8080, the
# old combined-compose service name, which resolves nowhere on a split deploy or a
# bare `docker run` -- so an unconfigured kb retried a host that cannot exist and
# never reported healthy. The Dockerfile removed the matching EMBEDDER_URL /
# SYNTHESIS_ENDPOINT env defaults for exactly that reason; this fallback survived it.
# Unset now means "no embedder configured", reported immediately so the caller can
# take its builtin path. The legacy service is still reachable by setting
# EMBEDDER_URL=http://embedder:8080 explicitly, as compose.yaml documents.
ENDPOINT = (
    os.environ.get("EMBEDDER_URL") or os.environ.get("SYNTHESIS_ENDPOINT") or ""
).rstrip("/")

NO_ENDPOINT_MESSAGE = (
    "embed-remote: no embedder configured "
    "(set SYNTHESIS_ENDPOINT, or EMBEDDER_URL to pin one)\n"
)
TIMEOUT = int(os.environ.get("AIMEE_EMBEDDER_TIMEOUT", "30"))
AUTH_TOKEN = os.environ.get("SYNTHESIS_API_KEY", "")
AUTH_REQUIRED = os.environ.get("SYNTHESIS_AUTH_REQUIRED", "") == "1"


def _auth_ready() -> bool:
    if AUTH_REQUIRED and not AUTH_TOKEN:
        sys.stderr.write(
            "embed-remote: SYNTHESIS_AUTH_REQUIRED=1 but SYNTHESIS_API_KEY is empty\n"
        )
        return False
    return True


def _headers(content_type: str) -> dict[str, str]:
    headers = {"content-type": content_type}
    if AUTH_TOKEN:
        headers["authorization"] = f"Bearer {AUTH_TOKEN}"
    return headers


def _post(path: str, data: bytes, content_type: str) -> str:
    req = urllib.request.Request(
        f"{ENDPOINT}{path}", data=data, headers=_headers(content_type), method="POST"
    )
    with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
        return resp.read().decode("utf-8")


def probe_dim() -> int:
    """embedder-autodim §2b: GET /health and return the loaded model's output dim.
    Exit 0 + print the integer dim ONLY when the embedder reports status=ok with a
    positive integer dim; exit non-zero (caller treats as 'not ready') while the
    model is still loading, on any HTTP/parse error, or on a missing/non-positive
    dim. Single GET, no embedding — cheap enough to poll."""
    if not _auth_ready():
        return 1
    if not ENDPOINT:
        sys.stderr.write(NO_ENDPOINT_MESSAGE)
        return 1
    try:
        req = urllib.request.Request(f"{ENDPOINT}/health", headers=_headers("application/json"))
        with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
    except (urllib.error.URLError, OSError) as exc:
        sys.stderr.write(f"embed-remote --dim: /health at {ENDPOINT} unreachable: {exc}\n")
        return 1
    except (json.JSONDecodeError, ValueError) as exc:
        sys.stderr.write(f"embed-remote --dim: bad /health payload: {exc}\n")
        return 1
    if not isinstance(payload, dict) or payload.get("status") != "ok":
        sys.stderr.write(f"embed-remote --dim: embedder not ready (status={payload.get('status')!r})\n")
        return 1
    dim = payload.get("dim")
    if not isinstance(dim, int) or dim <= 0:
        sys.stderr.write(f"embed-remote --dim: no positive integer dim (got {dim!r})\n")
        return 1
    print(dim)
    return 0


def probe_serving_id() -> int:
    """Print the endpoint's `serving_id` from /health — the identity of the vector space
    it serves (model + pooling + prefixes), which the kb records against its corpus.

    Exists because the shipped container reaches the gateway THROUGH this script, not
    over an in-process http:// transport, so the kb cannot GET /health itself. Mirrors
    --dim: exit non-zero when the endpoint is unreachable (the caller retries), exit 0
    with EMPTY output when it is reachable but reports no identity — an endpoint that
    predates the field, which must leave the guard inactive rather than refuse.

    Unlike --dim this does NOT require status=ok: the identity is registry data, not a
    measurement, so it is answerable while the model is still loading."""
    if not _auth_ready():
        return 1
    if not ENDPOINT:
        sys.stderr.write(NO_ENDPOINT_MESSAGE)
        return 1
    try:
        req = urllib.request.Request(f"{ENDPOINT}/health", headers=_headers("application/json"))
        with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        # 503 while warming up still carries the payload.
        try:
            payload = json.loads(exc.read().decode("utf-8"))
        except Exception:  # noqa: BLE001
            sys.stderr.write(f"embed-remote --serving-id: /health at {ENDPOINT}: {exc}\n")
            return 1
    except (urllib.error.URLError, OSError) as exc:
        sys.stderr.write(f"embed-remote --serving-id: /health at {ENDPOINT} unreachable: {exc}\n")
        return 1
    except (json.JSONDecodeError, ValueError) as exc:
        sys.stderr.write(f"embed-remote --serving-id: bad /health payload: {exc}\n")
        return 1
    if not isinstance(payload, dict):
        sys.stderr.write("embed-remote --serving-id: /health payload is not an object\n")
        return 1
    serving = payload.get("serving_id")
    if isinstance(serving, str) and serving:
        print(serving)
    return 0


def main() -> None:
    if "--dim" in sys.argv[1:]:
        sys.exit(probe_dim())
    if "--serving-id" in sys.argv[1:]:
        sys.exit(probe_serving_id())
    if not _auth_ready():
        sys.exit(1)
    if not ENDPOINT:
        sys.stderr.write(NO_ENDPOINT_MESSAGE)
        sys.exit(1)
    raw = sys.stdin.read()
    if not raw.strip():
        sys.stderr.write("embed-remote: empty input\n")
        sys.exit(1)

    # Batch mode: a JSON array of strings on stdin -> /embed_batch -> JSON array
    # of vectors (one per input). Anything else is one raw text -> /embed -> one
    # vector. (The C single-embed path always sends raw text, so it is unaffected.)
    batch_texts = None
    if raw.lstrip()[:1] == "[":
        try:
            parsed = json.loads(raw)
            if isinstance(parsed, list) and all(isinstance(t, str) for t in parsed):
                batch_texts = parsed
        except json.JSONDecodeError:
            batch_texts = None

    try:
        if batch_texts is not None:
            body = _post(
                "/embed_batch", json.dumps(batch_texts).encode("utf-8"), "application/json"
            )
        else:
            body = _post("/embed", raw.encode("utf-8"), "text/plain; charset=utf-8")
    except urllib.error.URLError as exc:
        sys.stderr.write(f"embed-remote: request to {ENDPOINT} failed: {exc}\n")
        sys.exit(1)

    try:
        out = json.loads(body)
    except json.JSONDecodeError as exc:
        sys.stderr.write(f"embed-remote: bad response: {exc}: {body[:200]}\n")
        sys.exit(1)

    if not isinstance(out, list):
        sys.stderr.write(f"embed-remote: expected a JSON array, got {body[:200]}\n")
        sys.exit(1)

    json.dump(out, sys.stdout)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
