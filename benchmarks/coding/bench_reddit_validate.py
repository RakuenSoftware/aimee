#!/usr/bin/env python3
"""Validation re-run of reddit10 (SWE-bench Lite, single-file) with BOTH corrected
investigations wired in: manager-only TOKENS (ledger) and queue-free TIMING (duration_ms).

Purpose: confirm the framework reproduces the EXPECTED single-file behavior, now that both
metrics are fixed:
  - TOKENS  : manager (codex select) << codex-solo  -> large savings (expected, ~-70%)
  - TIMING  : delegate best-of-N latency >= codex-solo latency -> NO speedup (expected on
              single-file: cheap workers are slower per call AND there is an extra select
              step; there is nothing to decompose in one file).

Metric = token_audit.duration_ms (verified queue-free). Same ledger-bracketing, sole tenant,
one instance at a time.

Arms (per instance):
  DEFAULT : codex solves the file.
            default_tokens  = codex tokens ;  default_latency = codex duration_ms
  DELEGATE: n cheap workers draft the file in parallel, then codex selects among them.
            manager_tokens  = codex select tokens (workers excluded)
            delegate_latency = max(cheap draft duration_ms) + codex select duration_ms
"""
from __future__ import annotations
import argparse, glob, json, os, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
os.environ.setdefault("AIMEE_BENCH_POLL_WORKERS", "64")
os.environ.setdefault("AIMEE_BENCH_POLL_PERIOD", "1")
from benchmarks.coding import bench_swebench_supervised as B

FLEET_SSH = os.environ.get("AIMEE_FLEET_SSH", "admin@192.168.1.254")
FLEET_DB = os.environ.get("AIMEE_FLEET_DB", "/mnt/media/.plugins/aimee-server/home/aimee.db")
PIN, POUT = 1.25 / 1e6, 10.0 / 1e6

# argv: <db> <lo> <hi> <model|*>. hi<0 => watermark. Returns duration_ms list + token sums
# for the model (or all models if '*') in (lo,hi].
_REMOTE = r'''
import sqlite3, sys, json
db, lo, hi, model = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
c = sqlite3.connect(db)
if hi < 0:
    print(json.dumps({"maxid": c.execute("SELECT COALESCE(MAX(id),0) FROM token_audit").fetchone()[0]})); sys.exit()
if model == "*":
    rows = c.execute("SELECT duration_ms, prompt_tokens, completion_tokens FROM token_audit "
                     "WHERE usage_kind='realized' AND id>? AND id<=?", (lo, hi)).fetchall()
else:
    rows = c.execute("SELECT duration_ms, prompt_tokens, completion_tokens FROM token_audit "
                     "WHERE model=? AND usage_kind='realized' AND id>? AND id<=?", (model, lo, hi)).fetchall()
print(json.dumps({"durations_ms": [r[0] for r in rows],
                  "prompt": sum(r[1] for r in rows), "completion": sum(r[2] for r in rows), "n": len(rows)}))
'''


def _ledger(*argv):
    p = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", FLEET_SSH,
                        "python3", "-", FLEET_DB, *map(str, argv)],
                       input=_REMOTE, capture_output=True, text=True, timeout=45)
    if p.returncode != 0:
        raise RuntimeError(f"ledger ssh failed: {p.stderr.strip()}")
    return json.loads(p.stdout)


def _maxid():
    return _ledger(0, -1, "_")["maxid"]


def _settle(prev=None, quiet_s=4.0, timeout_s=45.0):
    last, since, t0 = _maxid(), time.time(), time.time()
    while time.time() - t0 < timeout_s:
        time.sleep(2)
        cur = _maxid()
        if cur == last:
            if time.time() - since >= quiet_s:
                return cur
        else:
            last, since = cur, time.time()
    return last


def _run(fleet, items, model):
    lo = _settle()
    res = fleet.collect(fleet.dispatch_all(items))
    hi = _settle(lo)
    return res, _ledger(lo, hi, model)


def _cost(l):
    return PIN * l["prompt"] + POUT * l["completion"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--regions", default="benchmarks/results/swebench_supervised/regions")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--primary", default="codex")
    ap.add_argument("--primary-model", default="gpt-5.6-sol")
    ap.add_argument("--pool", default="MiniMax-M3,mimo-v2.5-pro,kimi-k2.7-code")
    ap.add_argument("--n", type=int, default=3)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    pool = [w.strip() for w in args.pool.split(",") if w.strip()]
    insts = [json.load(open(f)) for f in sorted(glob.glob(str(Path(args.regions) / "*.json")))]
    if args.limit:
        insts = insts[:args.limit]
    nonce = f"redditval-{os.getpid()}-{int(time.time())}"
    for i in insts:
        i["problem"] = f"[{nonce}]\n{i['problem']}"
    fleet = B.Fleet(os.environ.get("AIMEE_BENCH_CLIENT", "aimee"))
    print(f"{len(insts)} instances; sole-tenant; tokens=manager-only, timing=duration_ms", file=sys.stderr)

    per = {}
    for idx, inst in enumerate(insts, 1):
        iid = inst["instance_id"]
        print(f"[{idx}/{len(insts)}] {iid}", file=sys.stderr)
        # DEFAULT: codex solves the file.
        _, dman = _run(fleet, [(iid, args.primary, B._solve_prompt(inst))], args.primary_model)
        d_lat = dman["durations_ms"][0] if dman["durations_ms"] else None
        # DELEGATE phase 1: n cheap workers draft (parallel).
        witems = [((iid, k), pool[k % len(pool)], B._solve_prompt(inst)) for k in range(args.n)]
        wres, wall = _run(fleet, witems, "*")  # all-model durations of the candidate wave
        cands = [(pool[k % len(pool)], B._extract_diff(wres[(iid, k)][0]))
                 for k in range(args.n) if wres[(iid, k)][1] and B._extract_diff(wres[(iid, k)][0])]
        cand_lat = max(wall["durations_ms"]) if wall["durations_ms"] else None
        # DELEGATE phase 2: codex selects among candidates.
        sel_lat = mgr = None
        if cands:
            _, sman = _run(fleet, [(iid, args.primary, B._select_prompt(inst, cands))], args.primary_model)
            sel_lat = sman["durations_ms"][0] if sman["durations_ms"] else None
            mgr = sman
        deleg_lat = (cand_lat or 0) + (sel_lat or 0) if (cand_lat and sel_lat) else None
        per[iid] = {
            "default_tokens": dman["prompt"] + dman["completion"], "default_cost": round(_cost(dman), 4),
            "default_latency_ms": d_lat,
            "manager_tokens": (mgr["prompt"] + mgr["completion"]) if mgr else None,
            "manager_cost": round(_cost(mgr), 4) if mgr else None,
            "cand_latency_ms": cand_lat, "select_latency_ms": sel_lat, "delegate_latency_ms": deleg_lat,
            "n_candidates": len(cands),
        }
        tok = per[iid]
        tcut = (100 * (tok["default_tokens"] - tok["manager_tokens"]) / tok["default_tokens"]
                if tok["manager_tokens"] else None)
        spd = (d_lat / deleg_lat) if (d_lat and deleg_lat) else None
        print(f"    tokens default={tok['default_tokens']} manager={tok['manager_tokens']} "
              f"({tcut:+.0f}% )  latency default={d_lat}ms delegate={deleg_lat}ms "
              f"(speedup {spd:.2f}x)" if (tcut is not None and spd) else f"    (partial)", file=sys.stderr)

    def agg(k):
        return sum(v[k] for v in per.values() if v.get(k))
    dt, mt = agg("default_tokens"), agg("manager_tokens")
    dl = [v["default_latency_ms"] for v in per.values() if v["default_latency_ms"]]
    gl = [v["delegate_latency_ms"] for v in per.values() if v["delegate_latency_ms"]]
    speeds = [v["default_latency_ms"] / v["delegate_latency_ms"] for v in per.values()
              if v["default_latency_ms"] and v["delegate_latency_ms"]]
    summary = {
        "token_cut_pct": round(100 * (dt - mt) / dt, 1) if dt else None,
        "default_tokens": dt, "manager_tokens": mt,
        "median_default_latency_ms": sorted(dl)[len(dl) // 2] if dl else None,
        "median_delegate_latency_ms": sorted(gl)[len(gl) // 2] if gl else None,
        "median_speedup": round(sorted(speeds)[len(speeds) // 2], 2) if speeds else None,
        "delegate_faster_count": sum(1 for s in speeds if s > 1), "n_speed": len(speeds),
    }
    Path(args.output).write_text(json.dumps({"summary": summary, "per_instance": per}, indent=2))
    print(f"\nSUMMARY tokens -{summary['token_cut_pct']}%  |  latency median "
          f"default={summary['median_default_latency_ms']}ms delegate={summary['median_delegate_latency_ms']}ms "
          f"median_speedup={summary['median_speedup']}x  (delegate faster on "
          f"{summary['delegate_faster_count']}/{summary['n_speed']})", file=sys.stderr)
    print(f"written: {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
