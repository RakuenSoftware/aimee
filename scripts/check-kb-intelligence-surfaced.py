#!/usr/bin/env python3
"""Gate: every kb /v1/intelligence/* route is reachable from the thin client.

The thin client is DB-/kb_client-free; it reaches kb capabilities only through
aimee-server dispatch methods. A kb intelligence endpoint added to
src/kb/http/kb_http.c with no aimee-server path is silently unreachable (this is
exactly the gap that left the bandit/optimize surface stranded behind a dead
cmd_kb handler).

This check enforces, for each `/v1/intelligence/<...>` route served by
kb_http.c, that aimee-server has a client path to it:

  1. src/modules/kb_client/kb_client.c has a wrapper whose body contains the route path, and
  2. that wrapper function is CALLED from a server handler (src/server/*.c other
     than kb_client.c) — i.e. it is actually surfaced, not just defined.

A route may opt out by putting the token `kb-direct` in a comment on the route
line or the line above (for capabilities intentionally served only on the kb
host). Run as part of `make lint`.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KB_HTTP = ROOT / "src" / "kb" / "http" / "kb_http.c"
KB_CLIENT = ROOT / "src" / "modules" / "kb_client" / "kb_client.c"
SERVER_DIR = ROOT / "src" / "server"

ROUTE_RE = re.compile(r'strcmp\(\s*path\s*,\s*"(/v1/intelligence/[^"]+)"\s*\)')


def kb_intelligence_routes():
    """Return {path: annotated_kb_direct} for each intelligence route."""
    lines = KB_HTTP.read_text(encoding="utf-8").splitlines()
    routes = {}
    for i, line in enumerate(lines):
        m = ROUTE_RE.search(line)
        if not m:
            continue
        context = line + (lines[i - 1] if i > 0 else "")
        routes[m.group(1)] = "kb-direct" in context
    return routes


def wrapper_for_path(path):
    """Name of the kb_client function whose body contains `path`, or None."""
    text = KB_CLIENT.read_text(encoding="utf-8")
    # Split into top-level functions by `^<type> name(...)\n{ ... }` — cheap
    # heuristic: scan for `path` and walk back to the enclosing function name.
    idx = text.find(f'"{path}"')
    if idx < 0:
        return None
    head = text[:idx]
    m = None
    for m in re.finditer(r'^[A-Za-z_][\w \*]*\b(kb_client_[A-Za-z0-9_]+)\s*\(', head, re.M):
        pass
    return m.group(1) if m else None


def wrapper_is_called(fn):
    """True if `fn` is referenced in any src/server/*.c other than kb_client.c."""
    for c in SERVER_DIR.glob("*.c"):
        if c.name == "kb_client.c":
            continue
        if fn in c.read_text(encoding="utf-8"):
            return True
    return False


def main():
    routes = kb_intelligence_routes()
    if not routes:
        print("check-kb-intelligence-surfaced: FAIL — no intelligence routes found "
              "(did kb_http.c move?)")
        return 1

    failures = []
    checked = 0
    for path, kb_direct in sorted(routes.items()):
        if kb_direct:
            continue
        checked += 1
        fn = wrapper_for_path(path)
        if not fn:
            failures.append(f"{path}: no kb_client wrapper references it "
                            f"(add one in src/modules/kb_client/kb_client.c, or mark the route kb-direct)")
            continue
        if not wrapper_is_called(fn):
            failures.append(f"{path}: wrapper {fn}() is never called by an aimee-server "
                            f"handler (add a dispatch handler + method, or mark the route kb-direct)")

    if failures:
        print("check-kb-intelligence-surfaced: FAIL — unreachable kb intelligence routes:")
        for f in failures:
            print(f"  {f}")
        return 1

    kb_direct_n = sum(1 for v in routes.values() if v)
    print(f"check-kb-intelligence-surfaced: ok ({checked} routes surfaced via aimee-server, "
          f"{kb_direct_n} kb-direct)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
