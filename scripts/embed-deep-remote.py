#!/usr/bin/env python3
"""embed-deep-remote.py: deep-tier embedding client for the embedder service.

Same stdin-text -> stdout-JSON-float-array contract as embed-remote.py, but POSTs
to the embedder's /embed_deep route, which serves the larger DEEP_MODEL (default
pplx-embed-v1-4b, 2560-dim). Used by the background 4B "comprehensive" re-embed
pass (config memory_deep_embedding_command) — never on the interactive recall path
(a 4B embed is ~2.7s on CPU). Stdlib only — no torch in the kb image.

The deep model is lazy-loaded on the embedder's first /embed_deep call (a one-time
~150s load on CPU), so the timeout defaults high; tune AIMEE_DEEP_EMBED_TIMEOUT for
your hardware. On any error the C caller logs and skips the row (the backfill pass
retries it on a later cycle), so a transient embedder hiccup never wedges the pass.

Contract:
  stdin:  raw UTF-8 text
  stdout: JSON float array  [0.123, -0.456, ...]  (deep dim, L2-normalised)
  exit 0 on success; non-zero on error

Config (env):
  AIMEE_EMBEDDER_URL      base URL of embedder-server (default http://embedder:8080)
  AIMEE_DEEP_EMBED_TIMEOUT  request timeout seconds (default 300; covers the cold
                            deep-model load on the first call)

Usage:
  memory_deep_embedding_command: "python3 /opt/aimee/scripts/embed-deep-remote.py"
"""

import json
import os
import sys
import urllib.error
import urllib.request

ENDPOINT = os.environ.get("AIMEE_EMBEDDER_URL", "http://embedder:8080").rstrip("/")
TIMEOUT = int(os.environ.get("AIMEE_DEEP_EMBED_TIMEOUT", "300"))


def main() -> None:
    text = sys.stdin.read()
    if not text.strip():
        sys.stderr.write("embed-deep-remote: empty input\n")
        sys.exit(1)

    req = urllib.request.Request(
        f"{ENDPOINT}/embed_deep",
        data=text.encode("utf-8"),
        headers={"content-type": "text/plain; charset=utf-8"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
            body = resp.read().decode("utf-8")
    except urllib.error.URLError as exc:
        sys.stderr.write(f"embed-deep-remote: request to {ENDPOINT} failed: {exc}\n")
        sys.exit(1)

    try:
        vec = json.loads(body)
    except json.JSONDecodeError as exc:
        sys.stderr.write(f"embed-deep-remote: bad response: {exc}: {body[:200]}\n")
        sys.exit(1)

    if not isinstance(vec, list):
        sys.stderr.write(f"embed-deep-remote: expected a JSON array, got {body[:200]}\n")
        sys.exit(1)

    json.dump(vec, sys.stdout)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
