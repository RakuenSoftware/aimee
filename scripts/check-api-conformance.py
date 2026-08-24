#!/usr/bin/env python3
"""API conformance gate: every path documented in api/openapi-v1.yaml must be
served by the aimee-kb HTTP router (src/kb/http/*.c).

This is the static half of the proposal's "API conformance" AC — it fails CI if
the public OpenAPI contract drifts from the implementation (a documented
endpoint with no handler). It is spec → code only: the spec is the public
contract, so everything in it must be routed. Extra internal routes
(/v1/internal/*) and unversioned aliases in the code are intentionally allowed.

A spec path is considered implemented when:
  - (literal) its exact "/v1<path>" string appears as a route literal, or
  - (segment-routed) every concrete segment of a templated path appears in a
    `strcmp(segN, "<seg>")` routing comparison, or
  - (prefix-routed) a "/v1/<prefix>/" literal covers the part before the first
    {template} segment (e.g. "/v1/actions/" serves /v1/actions/{action}).

Run via `make api-conformance-check` (wired into `lint`).
"""

from __future__ import annotations

import glob
import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
SPEC = ROOT / "api" / "openapi-v1.yaml"
SRC_GLOB = str(ROOT / "src" / "kb" / "http" / "*.c")


def load_code():
    code = ""
    for f in glob.glob(SRC_GLOB):
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


def main() -> int:
    spec = yaml.safe_load(SPEC.read_text(encoding="utf-8"))
    paths = sorted((spec.get("paths") or {}).keys())
    literals, seg_literals = load_code()

    # A spec that parses to no paths makes every documented endpoint routed by
    # vacuity: `missing` is empty, the check prints "ok (0 documented endpoints
    # all routed)" and exits zero. That is not a passing gate, it is a gate that
    # stopped reading its input -- if the spec moves, is renamed, or changes the
    # shape this parse expects, the tree goes on looking verified.
    if not paths:
        print(f"api-conformance-check: {SPEC} declares no paths; this check would "
              f"pass having verified nothing")
        return 2
    if not literals and not seg_literals:
        print("api-conformance-check: found no route literals in the sources; "
              "this check would report every documented path as missing or none "
              "at all depending on the spec, and neither answer is about the code")
        return 2

    missing = [p for p in paths if not implemented(p, literals, seg_literals)]
    if missing:
        print("api-conformance-check: FAIL — documented paths with no route handler:")
        for p in missing:
            print(f"  /v1{p}")
        print("Add a handler in src/kb/http/, or remove the path from api/openapi-v1.yaml.")
        return 1

    print(f"api-conformance-check: ok ({len(paths)} documented endpoints all routed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
