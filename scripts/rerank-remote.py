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

Config (env):
  AIMEE_EMBEDDER_URL  base URL of embedder-server (default http://embedder:8080)
  AIMEE_RERANK_TIMEOUT  request timeout seconds (default 30)

Usage:
  memory_rerank_command: "python3 /opt/aimee/scripts/rerank-remote.py"
"""

import os
import sys
import urllib.error
import urllib.request

ENDPOINT = os.environ.get("AIMEE_EMBEDDER_URL", "http://embedder:8080").rstrip("/")
TIMEOUT = int(os.environ.get("AIMEE_RERANK_TIMEOUT", "30"))


def main() -> None:
    payload = sys.stdin.read()
    if not payload.strip():
        sys.stderr.write("rerank-remote: empty input\n")
        sys.exit(1)

    req = urllib.request.Request(
        f"{ENDPOINT}/rerank",
        data=payload.encode("utf-8"),
        headers={"content-type": "application/json"},
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
