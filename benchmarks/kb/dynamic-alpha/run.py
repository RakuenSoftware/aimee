#!/usr/bin/env python3
"""Dynamic-alpha fusion A/B benchmark.

Runs each fixture query against a live aimee-kb `POST /v1/search` in one or more
fusion modes (rrf / static_alpha / dynamic_alpha), scores the ranked hits against
`expected_top_path_substring`, and reports — per query and per fixture *shape* —
whether dynamic_alpha helped, hurt, or was neutral versus the rrf baseline.

The whole point (per the charter fixtures) is that dynamic alpha is expected to be
a *mixed* result: it should win on `positive` (lexical/identifier) queries without
regressing the `false_positive_guard` (semantic) and `regression` canaries. This
harness measures exactly that split instead of a single average.

Usage:
    python3 run.py --endpoint http://127.0.0.1:8741 --project aimee
    python3 run.py --endpoint http://192.168.1.254:8741 --k 5 --out report.json

No third-party deps (urllib only), so it runs anywhere python3 does — the .254
host, CI, or a laptop with LAN access to the KB.
"""
import argparse
import json
import os
import sys
import urllib.request
import urllib.error

MODES = ["rrf", "static_alpha", "dynamic_alpha"]
BASELINE = "rrf"


def search(endpoint, project, query, mode, k, bearer=None, timeout=45):
    """POST /v1/search; return (ordered list of hit paths, fusion_mode_used, alpha)."""
    body = json.dumps(
        {"query": query, "project": project, "max_results": k, "fusion_mode": mode}
    ).encode()
    req = urllib.request.Request(
        endpoint.rstrip("/") + "/v1/search", data=body, method="POST"
    )
    req.add_header("Content-Type", "application/json")
    if bearer:
        req.add_header("Authorization", "Bearer " + bearer)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            data = json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        return None, "http_%d" % e.code, None
    except Exception as e:  # noqa: BLE001 — a bad query shouldn't abort the sweep
        return None, "error:%s" % e, None
    hits = data.get("hits") or data.get("results") or []
    paths = [
        (h.get("artifact_id") or h.get("path") or h.get("file") or h.get("node_key") or "")
        for h in hits
    ]
    return paths, data.get("fusion_mode_used", mode), data.get("fusion_alpha")


def first_rank(paths, needle):
    """1-based rank of the first hit whose path contains needle, or None."""
    for i, p in enumerate(paths):
        if needle and needle in p:
            return i + 1
    return None


def verdict(base_rank, cand_rank):
    """Compare a candidate mode's rank against the baseline's (lower rank = better)."""
    if base_rank is None and cand_rank is None:
        return "both_miss"
    if cand_rank is None:
        return "worse"  # baseline found it, candidate didn't
    if base_rank is None:
        return "better"  # candidate found it, baseline didn't
    if cand_rank < base_rank:
        return "better"
    if cand_rank > base_rank:
        return "worse"
    return "same"


def main():
    ap = argparse.ArgumentParser(description="Dynamic-alpha fusion A/B benchmark")
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--endpoint", default="http://127.0.0.1:8741",
                    help="aimee-kb base URL (POST /v1/search)")
    ap.add_argument("--project", default="aimee", help="KB project scope")
    ap.add_argument("--fixtures", default=os.path.join(here, "fixtures.json"))
    ap.add_argument("--k", type=int, default=5, help="cutoff for P@k / hit@k")
    ap.add_argument("--modes", default=",".join(MODES),
                    help="comma-separated fusion modes to run")
    ap.add_argument("--bearer", default=os.environ.get("AIMEE_KB_BEARER"),
                    help="optional bearer token for the KB endpoint")
    ap.add_argument("--out", default=None, help="write the full JSON report here")
    args = ap.parse_args()

    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    with open(args.fixtures) as f:
        fixtures = json.load(f)["fixtures"]

    per_fixture = []
    agg = {m: {"hit1": 0, "hitk": 0, "mrr": 0.0, "used": None} for m in modes}
    for fx in fixtures:
        q, needle, shape = fx["query"], fx.get("expected_top_path_substring", ""), fx.get("shape", "")
        row = {"id": fx["id"], "shape": shape, "query": q, "expected": needle, "modes": {}}
        for m in modes:
            paths, used, alpha = search(args.endpoint, args.project, q, m, args.k, args.bearer)
            rank = first_rank(paths or [], needle)
            agg[m]["used"] = used
            if rank == 1:
                agg[m]["hit1"] += 1
            if rank is not None and rank <= args.k:
                agg[m]["hitk"] += 1
                agg[m]["mrr"] += 1.0 / rank
            row["modes"][m] = {"rank": rank, "used": used, "alpha": alpha,
                               "top": (paths or [])[:3]}
        # dynamic-vs-baseline verdict, if both present
        if BASELINE in row["modes"] and "dynamic_alpha" in row["modes"]:
            row["dynamic_vs_rrf"] = verdict(row["modes"][BASELINE]["rank"],
                                            row["modes"]["dynamic_alpha"]["rank"])
        per_fixture.append(row)

    n = len(fixtures)
    report = {
        "endpoint": args.endpoint, "project": args.project, "k": args.k,
        "n_fixtures": n,
        "aggregate": {
            m: {"p_at_1": round(agg[m]["hit1"] / n, 3) if n else 0.0,
                "p_at_k": round(agg[m]["hitk"] / n, 3) if n else 0.0,
                "mrr": round(agg[m]["mrr"] / n, 3) if n else 0.0,
                "fusion_mode_used": agg[m]["used"]}
            for m in modes
        },
        "fixtures": per_fixture,
    }

    # Per-shape dynamic-vs-rrf split — the mixed-result picture.
    by_shape = {}
    for row in per_fixture:
        v = row.get("dynamic_vs_rrf")
        if v is None:
            continue
        by_shape.setdefault(row["shape"], {"better": 0, "worse": 0, "same": 0, "both_miss": 0})
        by_shape[row["shape"]][v] += 1
    report["dynamic_vs_rrf_by_shape"] = by_shape

    # --- human summary ---
    print("Dynamic-alpha fusion A/B — %s (project=%s, k=%d, n=%d)"
          % (args.endpoint, args.project, args.k, n))
    print("\nAggregate (P@1 / P@k / MRR):")
    for m in modes:
        a = report["aggregate"][m]
        print("  %-14s P@1=%.3f  P@%d=%.3f  MRR=%.3f  (used=%s)"
              % (m, a["p_at_1"], args.k, a["p_at_k"], a["mrr"], a["fusion_mode_used"]))
    print("\ndynamic_alpha vs rrf, by fixture shape:")
    for shape, c in by_shape.items():
        print("  %-22s better=%d worse=%d same=%d both_miss=%d"
              % (shape, c["better"], c["worse"], c["same"], c["both_miss"]))
    print("\nPer fixture (rank in each mode; lower is better, - = miss):")
    for row in per_fixture:
        ranks = "  ".join("%s=%s" % (m, row["modes"][m]["rank"] if row["modes"][m]["rank"] else "-")
                          for m in modes)
        print("  [%-22s %-20s] %s  -> %s"
              % (row["shape"], row["id"], ranks, row.get("dynamic_vs_rrf", "")))

    if args.out:
        with open(args.out, "w") as f:
            json.dump(report, f, indent=2)
        print("\nwrote %s" % args.out)

    # Non-zero exit if dynamic hurt a guard/regression fixture (a real signal for CI).
    hurt_guards = sum(
        1 for row in per_fixture
        if row["shape"] in ("false_positive_guard", "regression")
        and row.get("dynamic_vs_rrf") == "worse"
    )
    return 1 if hurt_guards else 0


if __name__ == "__main__":
    sys.exit(main())
