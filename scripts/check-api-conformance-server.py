#!/usr/bin/env python3
"""API conformance gate for the aimee-server /v1 surface: every path documented
in api/openapi-server-v1.yaml must be served by the server HTTP router
(src/server/server_http.c).

This is the server-side twin of scripts/check-api-conformance.py (which guards
the aimee-kb surface). It fails CI if the public OpenAPI contract drifts from
the implementation (a documented endpoint with no handler). It is spec → code
only: the spec is the public contract, so everything in it must be routed.

A spec path is considered implemented when:
  - (literal) its exact "/v1<path>" string appears as a route literal, or
  - (prefix-routed) a "/v1/<prefix>/" literal covers the part before the first
    {template} segment (e.g. "/v1/personas/" serves /v1/personas/{name} and
    "/v1/sessions/" serves /v1/sessions/{id}/persona).

Run via `make server-api-conformance-check` (wired into `lint`).
"""

from __future__ import annotations

import glob
import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
SPEC = ROOT / "api" / "openapi-server-v1.yaml"
# The router (server_http.c) plus the declarative route registry it includes
# (server_http_routes.c). Route path literals live in the registry now; the
# router still holds the streaming-route literals handled before the table.
SRC_GLOBS = [
    str(ROOT / "src" / "server" / "server_http.c"),
    str(ROOT / "src" / "server" / "server_http_routes.c"),
]


def load_code():
    code = ""
    for pat in SRC_GLOBS:
        for f in glob.glob(pat):
            code += Path(f).read_text(encoding="utf-8")
    literals = set(re.findall(r'"(/v1/[^"]*)"', code))
    seg_literals = set(re.findall(r'strcmp\(seg\d+,\s*"([^"]+)"\)', code))
    return literals, seg_literals


def implemented(path: str, literals: set, seg_literals: set) -> bool:
    full = "/v1" + path
    if "{" not in path:
        return full in literals or (full + "/") in literals

    concrete = [s for s in path.strip("/").split("/") if not s.startswith("{")]
    if concrete and all(s in seg_literals for s in concrete):
        return True

    prefix_parts = []
    for s in path.strip("/").split("/"):
        if s.startswith("{"):
            break
        prefix_parts.append(s)
    prefix = "/v1/" + "/".join(prefix_parts)
    return prefix in literals or (prefix + "/") in literals


def documented(literal: str, spec_paths: list) -> bool:
    """code -> spec: a route literal found in the server is covered by the spec
    when it equals a documented path, or is the static prefix of a templated
    documented path (e.g. "/v1/personas/" -> /personas/{name}, "/v1/sessions/"
    -> /sessions/{id}/persona)."""
    p = literal[len("/v1"):]
    if p in spec_paths or p.rstrip("/") in spec_paths:
        return True
    base = literal.rstrip("/")
    return any("{" in sp and ("/v1" + sp).startswith(base + "/") for sp in spec_paths)


def main() -> int:
    spec = yaml.safe_load(SPEC.read_text(encoding="utf-8"))
    paths = sorted((spec.get("paths") or {}).keys())
    literals, seg_literals = load_code()

    # An empty spec makes every documented path routed by vacuity, and no route
    # literals makes the comparison about neither side. Either way the check
    # exits zero having verified nothing, and a gate that stopped reading its
    # input is indistinguishable from one that is satisfied.
    if not paths:
        print(f"server-api-conformance-check: {SPEC} declares no paths; this check "
              f"would pass having verified nothing")
        return 2
    if not literals and not seg_literals:
        print("server-api-conformance-check: found no route literals in the "
              "sources; the comparison would be about neither side")
        return 2

    # spec -> code: every documented path must have a route handler.
    missing = [p for p in paths if not implemented(p, literals, seg_literals)]
    if missing:
        print("server-api-conformance-check: FAIL — documented paths with no route handler:")
        for p in missing:
            print(f"  /v1{p}")
        print("Add a handler in src/server/server_http.c, or remove the path from "
              "api/openapi-server-v1.yaml.")
        return 1

    # code -> spec: every routed /v1 endpoint must appear in the spec.
    undocumented = sorted(lit for lit in literals if not documented(lit, paths))
    if undocumented:
        print("server-api-conformance-check: FAIL — routed paths missing from the spec:")
        for lit in undocumented:
            print(f"  {lit}")
        print("Document it in api/openapi-server-v1.yaml, or remove the route from "
              "src/server/server_http.c.")
        return 1

    print(f"server-api-conformance-check: ok ({len(paths)} documented endpoints all routed, "
          f"no undocumented routes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
