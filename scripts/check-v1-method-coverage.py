#!/usr/bin/env python3
"""WP1.fin coverage gate for the aimee-server `/v1` op-parity buildout.

Every NDJSON method in `server_dispatch_table[]` (src/server/server.c) must
either have a first-class `/v1` route or appear on one of the explicit lists
below (a dedicated non-op handler, or a documented deliberate exclusion). This
fails CI if a new RPC method is added without a `/v1` route and without a
conscious decision to exclude it — i.e. it stops op-parity from silently
regressing.

A method is considered routed when it appears as the `op` twin of a route row
in src/server/server_http_routes.inc (the declarative registry). Routes backed
by a bespoke handler with no op twin (op == NULL) are listed in DEDICATED.

See docs/v1-op-parity-buildout.md for the convention and the exclusion
rationale. Run via `make v1-method-coverage-check` (wired into `lint`).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DISPATCH = ROOT / "src" / "server" / "server.c"
REGISTRY = ROOT / "src" / "server" / "server_http_routes.inc"

METHOD_RE = re.compile(r'"([a-z_]+\.[a-z_]+)"')

# Methods served by a first-class /v1 route via a bespoke handler whose route
# row carries op == NULL (so they don't show up as an op twin in the registry).
DEDICATED = {
    "server.health",       # GET /v1/health (rh_health) + GET /v1/server/health
    "memory.recall",       # POST /v1/memory/recall (rh_memory_recall)
    "kb.search",           # POST /v1/kb/search (rh_kb_search)
    "kb.status",           # GET /v1/kb/status (rh_kb_status)
    "chat.send_stream",    # /v1/chat/* + /v1/completions/responses/embeddings
    "session.list",        # GET /v1/sessions (rh_sessions_list)
    "session.attach",      # POST /v1/sessions/{id}/attach
    "session.detach",      # POST /v1/sessions/{id}/detach
    "session.presence",    # also routed (op twin); kept for clarity
    "primary.get",         # GET /v1/sessions/{id}/primary
    "primary.set",         # POST /v1/sessions/{id}/primary
    "primary.clear",       # DELETE /v1/sessions/{id}/primary
    "runner.poll",         # POST /v1/runner/poll
    "runner.respond",      # POST /v1/runner/respond
}

# Deliberate exclusions — NOT given a first-class /v1 route. Keep in sync with
# the exclusions table in docs/v1-op-parity-buildout.md.
# (none) — every dispatch method now has a first-class /v1 route. The harness
# hooks (hooks.pre/post/session_start) and the tool-execution bridge
# (tool.execute) became first-class routes (POST /v1/hooks/*, /v1/tools/execute)
# so /v1/rpc could be retired; CAP_TOOL_EXECUTE keeps them local/full-tier only.
# pipeline.* — the roundtable authoring pipeline control surface is CLI/MCP-only
# in v1 (docs/proposals/done/agent-roundtable-authoring-pipeline.md §5/§12,
# decision #45). HTTP is deliberately deferred: it must NOT collide with the
# existing KB/corpus `GET /v1/pipeline/status`, so if exposed later it uses a
# distinct `/v1/roundtable/pipelines/...` namespace. Gate resolution is also
# operator/local-only (#53), which the bearer-scoped HTTP surface does not model.
EXCLUDED: set = {
    "pipeline.start",
    "pipeline.status",
    "pipeline.list",
    "pipeline.cancel",
    "pipeline.resume",
    "pipeline.advance",
    "pipeline.gate",
    # Turn control, not a data op: cancels an in-flight chat turn for a session
    # (owner-authz). Reachable over the NDJSON socket and the gateway /stop path;
    # no dedicated /v1 op twin, mirroring pipeline.cancel/resume above.
    "chat.graceful_cancel",
}
# NB: the long-running / LLM methods (kb.build/ingest/update, graph.sync_code,
# index.scan, memory.benchmark, curator.synthesize, rules.generate, eval.run)
# were previously excluded; they are now first-class via rh_dispatch_op_async
# (a queued run handle polled at GET /v1/runs/{id}) so they count as op-routed.


def dispatch_methods() -> set:
    """Method names registered in server_dispatch_table[]."""
    text = DISPATCH.read_text(encoding="utf-8")
    m = re.search(r"server_dispatch_table\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        print("check-v1-method-coverage: FAIL — could not find server_dispatch_table[]")
        sys.exit(2)
    body = m.group(1)
    return {
        mm.group(1)
        for line in body.splitlines()
        if (mm := re.match(r'\s*\{\s*"([a-z_]+\.[a-z_]+)"\s*,', line))
    }


def routed_ops() -> set:
    """Method op-twins on route rows in the declarative registry."""
    ops = set()
    for line in REGISTRY.read_text(encoding="utf-8").splitlines():
        if "RM_EXACT" not in line and "RM_PREFIX" not in line:
            continue
        # The op twin is the only "<a>.<b>" token on a row (the path starts
        # with '/v1/' and so never matches METHOD_RE).
        ops.update(METHOD_RE.findall(line))
    return ops


def main() -> int:
    methods = dispatch_methods()
    covered = routed_ops() | DEDICATED | EXCLUDED
    missing = sorted(methods - covered)
    if missing:
        print("check-v1-method-coverage: FAIL — NDJSON methods with no /v1 route "
              "and no documented exclusion:")
        for m in missing:
            print(f"  {m}")
        print("Add a route row in src/server/server_http_routes.inc (+ an "
              "OpenAPI path), or add the method to DEDICATED/EXCLUDED in "
              "scripts/check-v1-method-coverage.py with a rationale "
              "(see docs/v1-op-parity-buildout.md).")
        return 1
    routed = len(methods & routed_ops())
    print(f"check-v1-method-coverage: ok ({len(methods)} dispatch methods — "
          f"{routed} op-routed, {len(methods & DEDICATED)} dedicated, "
          f"{len(methods & EXCLUDED)} excluded)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
