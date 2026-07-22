#!/usr/bin/env python3
"""Guard: the /v1 route table's first-match dispatch stays order-correct.

`server_http_routes.c` resolves a request by scanning `g_v1_routes[]` top-to-
bottom and returning the FIRST row whose verb+path match (RM_EXACT exact, or
RM_PREFIX by `strncmp` + a single `{id}` segment + optional suffix). When two
rows can both match one request, ARRAY ORDER silently decides the winner — e.g.
`GET /v1/workflow/items/all` (exact) vs the `GET /v1/workflow/items/` prefix
(which would treat "all" as an {id}). The exact row must appear first, or the
dedicated endpoint is shadowed by the generic one.

This check makes that ordering constraint explicit and machine-checked:
  1. parse g_v1_routes[] in source order,
  2. replicate the C matcher,
  3. probe one representative request per route,
  4. for every request that matches >1 row, require the FIRST match to be the
     MOST SPECIFIC row (exact > prefix+suffix > bare prefix; longer path wins).

It fails if a new overlapping route is added in the wrong order, or an existing
overlap is reordered so the generic row shadows the specific one. It is also the
prerequisite for ever generating g_v1_routes from the route descriptor: a
generator can only be trusted to emit rows in a safe order once "safe order" is
defined and enforced here.

Run:  scripts/check-v1-route-order.py   (exit 0 = ok, 1 = order hazard)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REGISTRY = ROOT / "src" / "server" / "server_http_routes.c"

ROW_RE = re.compile(
    r'\{\s*'
    r'"(GET|POST|PUT|DELETE)"\s*,\s*'      # verb
    r'"([^"]+)"\s*,\s*'                    # path
    r'(NULL|"[^"]*")\s*,\s*'               # suffix
    r'(RM_EXACT|RM_PREFIX)\s*,\s*'         # match kind
    r'(?:NULL|"[^"]*")\s*,\s*'             # op (unused here)
    r'[^,}]+?\s*,\s*'                      # caps (unused here)
    r'(?:NULL|rh_\w+)\s*\}'                # handler (unused here)
)


def _array_body(text):
    start = text.index("g_v1_routes[] = {")
    open_brace = text.index("{", start)
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace + 1:i]
    raise SystemExit("check-v1-route-order: unterminated g_v1_routes[] array")


def _routes():
    body = _array_body(REGISTRY.read_text(encoding="utf-8"))
    out = []
    for verb, path, suffix, kind in ROW_RE.findall(body):
        out.append({
            "verb": verb,
            "path": path,
            "suffix": None if suffix == "NULL" else suffix[1:-1],
            "exact": kind == "RM_EXACT",
        })
    return out


def matches(verb, path, r):
    """Faithful port of the C matcher in server_http_routes.c."""
    if verb != r["verb"]:
        return False
    if r["exact"]:
        return path == r["path"]
    if not path.startswith(r["path"]):
        return False
    rest = path[len(r["path"]):]
    slash = rest.find("/")
    if r["suffix"] is not None:
        return slash != -1 and rest[slash:] == r["suffix"]
    return bool(rest) and slash == -1


def specificity(r):
    """Lower = more specific: exact < prefix+suffix < bare prefix; longer path wins."""
    tier = 0 if r["exact"] else (1 if r["suffix"] is not None else 2)
    return (tier, -len(r["path"]), -len(r["suffix"] or ""))


def probe_request(r):
    """A representative request path that this route matches.

    One probe per route is sufficient because a route's match decision depends
    only on (verb, prefix string, slash structure, suffix string) — never on the
    id's characters — so a single fixed-token id fully characterizes the route's
    structural match class. This holds while every suffix is a single segment
    (as they all are: /gate, /events, /proposal, ...). A *multi-segment* suffix
    (one containing an embedded '/…/') could in theory create an overlap that
    neither participant's canonical probe lands in; add cross-probing here if
    such a suffix is ever introduced."""
    if r["exact"]:
        return r["verb"], r["path"]
    return r["verb"], r["path"] + "PROBEID" + (r["suffix"] or "")


def main():
    routes = _routes()
    overlaps = []
    failures = []
    for r in routes:
        verb, path = probe_request(r)
        hits = [i for i, rt in enumerate(routes) if matches(verb, path, rt)]
        if len(hits) <= 1:
            continue
        winner = routes[hits[0]]
        best = min((routes[i] for i in hits), key=specificity)
        entry = (verb, path, [routes[i] for i in hits])
        # De-dupe: an overlap surfaces from each participant's probe.
        key = (verb, tuple(sorted(hits)))
        overlaps.append((key, entry))
        if specificity(winner) != specificity(best):
            failures.append((verb, path, winner, best))

    seen = set()
    uniq = []
    for key, entry in overlaps:
        if key in seen:
            continue
        seen.add(key)
        uniq.append(entry)

    if failures:
        print("check-v1-route-order: FAIL — a generic route shadows a more-specific "
              "one (first-match wins, but a more-specific row appears later):",
              file=sys.stderr)
        for verb, path, winner, best in failures:
            print(f"  {verb} {path}: matched {winner['path']!r} first, but "
                  f"{best['path']!r} is more specific — move it earlier.",
                  file=sys.stderr)
        sys.exit(1)

    detail = "; ".join(
        f"{v} {p} -> {hits[0]['path']!r} (over {len(hits)} matches)"
        for v, p, hits in uniq) or "none"
    print(f"check-v1-route-order: ok ({len(routes)} routes, "
          f"{len(uniq)} order-dependent overlap(s), all specific-first: {detail})")


if __name__ == "__main__":
    main()
