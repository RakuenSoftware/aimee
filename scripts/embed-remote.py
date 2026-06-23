#!/usr/bin/env python3
"""embed-remote.py: thin embedding_command client for the embedder service.

Same contract as embed-minilm.py (stdin text -> stdout JSON float array), but
instead of loading the model in-process it POSTs to the persistent
embedder-server.py service, so embedding stays fast inside the aimee-kb
container. Stdlib only — no torch in the kb image.

Contract (platform_exec_pipe in src/memory_core_scope_embed.inc):
  stdin:  raw UTF-8 text
  stdout: JSON float array  [0.123, -0.456, ...]  (384-dim, L2-normalised)
  exit 0 on success; non-zero on error (C caller logs a warning and skips)

Config (env):
  AIMEE_EMBEDDER_URL  base URL of embedder-server (default http://embedder:8080)

Usage:
  embedding_command: "python3 /opt/aimee/scripts/embed-remote.py"
"""

import json
import os
import sys
import urllib.error
import urllib.request

ENDPOINT = os.environ.get("AIMEE_EMBEDDER_URL", "http://embedder:8080").rstrip("/")
TIMEOUT = int(os.environ.get("AIMEE_EMBEDDER_TIMEOUT", "30"))


def _post(path: str, data: bytes, content_type: str) -> str:
    req = urllib.request.Request(
        f"{ENDPOINT}{path}", data=data, headers={"content-type": content_type}, method="POST"
    )
    with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
        return resp.read().decode("utf-8")


def probe_dim() -> int:
    """embedder-autodim §2b: GET /health and return the loaded model's output dim.
    Exit 0 + print the integer dim ONLY when the embedder reports status=ok with a
    positive integer dim; exit non-zero (caller treats as 'not ready') while the
    model is still loading, on any HTTP/parse error, or on a missing/non-positive
    dim. Single GET, no embedding — cheap enough to poll."""
    try:
        with urllib.request.urlopen(f"{ENDPOINT}/health", timeout=TIMEOUT) as resp:
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


def main() -> None:
    if "--dim" in sys.argv[1:]:
        sys.exit(probe_dim())
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
