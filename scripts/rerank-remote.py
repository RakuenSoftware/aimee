#!/usr/bin/env python3
"""rerank-remote.py: thin memory_rerank_command client for the embedder service.

aimee's cross-encoder rerank stage (memory_core_search.inc:
memory_cross_encoder_scores) shells out to memory_rerank_command with a JSON
array of [query, candidate] pairs on stdin and expects a JSON float array of
scores on stdout (higher = more relevant). On any non-zero exit / malformed
output the C caller silently falls back to hybrid ordering, so this never has to
be present — but when it is, it gives a second-pass cross-encoder rerank of the
top-K candidates.

This client holds no model: it POSTs the pairs to the persistent embedder-server
(/rerank), which serves the cross-encoder from its resident torch stack. Stdlib
only — no torch in the kb image.

Contract:
  stdin:  JSON [[query, cand0], [query, cand1], ...]
  stdout: JSON [score0, score1, ...]   (same length/order)
  exit 0 on success; non-zero on error (C caller logs + falls back).

Config (env), in precedence order:
  AIMEE_RERANKER_URL  base URL of the reranker service (pins rerank independently
                      of the embedder)
  AIMEE_EMBEDDER_URL  base URL of the embedder service (rerank shares it by default)
  AIMEE_LLM_URL       base URL of the unified aimee-llm container (one knob for
                      embed + rerank + synth)
  AIMEE_LLM_AUTH_TOKEN bearer service identity for authenticated gateways
  AIMEE_LLM_AUTH_REQUIRED=1 refuse requests when that identity is missing
  (unset)             no reranker configured; reported immediately. Pin the
                      legacy compose service explicitly if wanted.
  AIMEE_RERANK_TIMEOUT  request timeout seconds (default 30)

Usage:
  memory_rerank_command: "python3 /opt/aimee/scripts/rerank-remote.py"
"""

import os
import sys
import urllib.error
import urllib.request

# No implicit legacy fallback -- see the note in embed-remote.py. Defaulting to
# the old combined-compose http://embedder:8080 made an unconfigured kb dial a
# host that cannot resolve on a split deploy or a bare `docker run`. Pin the
# legacy service explicitly if you want it.
ENDPOINT = (
    os.environ.get("AIMEE_RERANKER_URL")
    or os.environ.get("AIMEE_EMBEDDER_URL")
    or os.environ.get("AIMEE_LLM_URL")
    or ""
).rstrip("/")
TIMEOUT = int(os.environ.get("AIMEE_RERANK_TIMEOUT", "30"))
AUTH_TOKEN = os.environ.get("AIMEE_LLM_AUTH_TOKEN", "")
AUTH_REQUIRED = os.environ.get("AIMEE_LLM_AUTH_REQUIRED", "") == "1"


def main() -> None:
    if AUTH_REQUIRED and not AUTH_TOKEN:
        sys.stderr.write(
            "rerank-remote: AIMEE_LLM_AUTH_REQUIRED=1 but AIMEE_LLM_AUTH_TOKEN is empty\n"
        )
        sys.exit(1)
    if not ENDPOINT:
        sys.stderr.write(
            "rerank-remote: no reranker configured "
            "(set AIMEE_LLM_URL, or AIMEE_RERANKER_URL to pin one)\n"
        )
        sys.exit(1)
    payload = sys.stdin.read()
    if not payload.strip():
        sys.stderr.write("rerank-remote: empty input\n")
        sys.exit(1)

    headers = {"content-type": "application/json"}
    if AUTH_TOKEN:
        headers["authorization"] = f"Bearer {AUTH_TOKEN}"
    req = urllib.request.Request(
        f"{ENDPOINT}/rerank",
        data=payload.encode("utf-8"),
        headers=headers,
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
            body = resp.read().decode("utf-8")
    except urllib.error.URLError as exc:
        sys.stderr.write(f"rerank-remote: request to {ENDPOINT} failed: {exc}\n")
        sys.exit(1)

    # Pass the embedder service's JSON score array straight through.
    sys.stdout.write(body)


if __name__ == "__main__":
    main()
