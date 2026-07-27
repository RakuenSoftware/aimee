#!/usr/bin/env python3
"""Guard that handlers on async (rh_dispatch_op_async) /v1 routes reply inline.

Those routes are driven by the op-run bridge in server_http.c: it hands the
dispatch a socketpair, calls server_dispatch(), then shuts the write end and
reads whatever is there. A handler that returns without having written its
reply -- typically by handing the connection fd to a detached thread -- produces
nothing for that read, and the run fails with "rpc produced no response".

That is not hypothetical. index.scan hit it once and was converted to reply
inline (see the comment on handle_index_scan). The same defect then sat
unnoticed in kb.build, kb.update, kb.ingest, kb.docs.push and graph.sync_code,
which all replied from a detached thread via a shared spawn helper: the entire
KB write surface returned "rpc produced no response" over /v1, for thin clients
and the browser alike.

Because the detach lived in a *helper*, scanning each handler body would have
missed it. So this check is file-granular: a source file that defines any
handler bound to an async route must not create detached threads at all. If a
handler there genuinely needs to run work off-thread, it must still write its
own reply before returning.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROUTES = ROOT / "src" / "server" / "server_http_routes.c"
DISPATCH = ROOT / "src" / "server" / "server.c"
SERVER_DIR = ROOT / "src" / "server"

# {"POST", "/v1/kb/build", NULL, RM_EXACT, "kb.build", 0, rh_dispatch_op_async},
ASYNC_ROUTE_RE = re.compile(
    r'\{\s*"(?:GET|POST|PUT|DELETE)"\s*,\s*"[^"]+"\s*,[^}]*?"([a-z0-9_.]+)"\s*,[^}]*?rh_dispatch_op_async\s*\}'
)
# {"kb.build", handle_kb_build},
BINDING_RE = re.compile(r'\{\s*"([a-z0-9_.]+)"\s*,\s*(handle_[a-z0-9_]+)\s*\}')


def async_methods():
    return set(ASYNC_ROUTE_RE.findall(ROUTES.read_text(encoding="utf-8")))


def handler_for_method(bindings, method):
    return bindings.get(method)


def main() -> int:
    methods = async_methods()
    if not methods:
        print("check-async-op-handlers: FAIL — found no rh_dispatch_op_async routes; "
              "has the route table or this check's pattern changed?")
        return 1

    bindings = dict(BINDING_RE.findall(DISPATCH.read_text(encoding="utf-8")))

    # Which files define the handlers bound to async methods?
    handlers = {}
    for m in sorted(methods):
        h = handler_for_method(bindings, m)
        if h:
            handlers[h] = m

    defining = {}  # file -> [(handler, method), ...]
    define_re = {h: re.compile(r'^\s*int\s+' + re.escape(h) + r'\s*\(', re.M) for h in handlers}
    for path in sorted(SERVER_DIR.glob("*.c")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for h, rx in define_re.items():
            if rx.search(text):
                defining.setdefault(path, []).append((h, handlers[h]))

    failures = []
    for path, owned in sorted(defining.items()):
        text = path.read_text(encoding="utf-8", errors="replace")
        if "pthread_detach" in text:
            names = ", ".join(sorted(m for _, m in owned))
            failures.append((path.relative_to(ROOT), names))

    if failures:
        print("check-async-op-handlers: FAIL — these files define handlers on async "
              "/v1 routes AND create detached threads. A detached reply is written "
              "after the op-run bridge has already read and closed its socketpair, "
              "so the run fails with \"rpc produced no response\". Reply inline.")
        for rel, names in failures:
            print(f"  {rel}  (async methods: {names})")
        return 1

    print(f"check-async-op-handlers: ok ({len(methods)} async routes, "
          f"{len(handlers)} bound handlers across {len(defining)} file(s), all reply inline)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
