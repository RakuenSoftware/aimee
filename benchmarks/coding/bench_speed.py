#!/usr/bin/env python3
"""PRE-REGISTERED speed test. Run ONCE; accept the result whatever it is.

QUESTION
  On a K-file task, does solving each file in a separate PARALLEL call beat one model
  solving all K files in a single monolithic call?

METRIC  (this is the whole point of the redo)
  Per-call latency = token_audit.duration_ms. Verified queue-free: it is a CLOCK_MONOTONIC
  timer wrapped ONLY around the provider HTTP round-trip (src/server/agent_runtime.c:1141-1200),
  written to token_audit.duration_ms (agent_logging.c:307). It EXCLUDES aimee-side
  queue/admission wait. (The job status created_at->updated_at span was proven to INCLUDE
  queue wait -- agent_jobs.c:95 inserts the row 'pending' at enqueue -- so it is NOT used.)
  Because duration_ms is queue-free, PARALLEL latency = max(duration_ms over the K calls) is
  the true "if all K ran at once" wall time, independent of the fleet's concurrency cap.

ARMS  (same model both sides -> isolates decomposition+parallelism, not worker speed)
  MONOLITHIC : codex solves ALL K files in one call.     latency = duration_ms of that 1 row
  PARALLEL   : codex solves EACH file in its own call.    latency = max(duration_ms over K rows)
  No manager/localize pass: both arms already know the file set (files are in both prompts),
  and localize was separately shown to be a serial reasoning bottleneck.

FAIRNESS  (locked; no post-hoc changes)
  - one instance at a time (sole ledger tenant)
  - both arms get identical file regions
  - latency strictly from ledger duration_ms (ms), reported per instance
  - accept the aggregate regardless of direction

PREDICTION (recorded up front)
  parallel latency ~ the slowest single-file call; monolithic ~ one big call. Expect parallel
  faster when K is large and files are comparable; marginal or worse for small K or when
  per-file fixed overhead (re-reading the problem) dominates.

Example:
  python3 benchmarks/coding/bench_speed.py \
    --regions benchmarks/results/swebench_multifile/regions \
    --primary codex --primary-model gpt-5.6-sol \
    --output benchmarks/results/swebench_multifile/speed_prereg.json
"""
from __future__ import annotations
import argparse, glob, json, os, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
os.environ.setdefault("AIMEE_BENCH_POLL_WORKERS", "64")
os.environ.setdefault("AIMEE_BENCH_POLL_PERIOD", "1")
from benchmarks.coding import bench_swebench_supervised as B
from benchmarks.coding import bench_decompose as D

FLEET_SSH = os.environ.get("AIMEE_FLEET_SSH", "admin@192.168.1.254")
FLEET_DB = os.environ.get("AIMEE_FLEET_DB", "/mnt/media/.plugins/aimee-server/home/aimee.db")

# Runs on the fleet host. argv: <db> <lo> <hi> <model>. hi<0 => watermark. Returns the list
# of duration_ms (queue-free provider latencies) plus token sums for the model in (lo,hi].
_REMOTE = r'''
import sqlite3, sys, json
db, lo, hi, model = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
c = sqlite3.connect(db)
if hi < 0:
    print(json.dumps({"maxid": c.execute("SELECT COALESCE(MAX(id),0) FROM token_audit").fetchone()[0]})); sys.exit()
rows = c.execute("SELECT duration_ms, prompt_tokens, completion_tokens FROM token_audit "
                 "WHERE model=? AND usage_kind='realized' AND id>? AND id<=?",
                 (model, lo, hi)).fetchall()
print(json.dumps({"durations_ms": [r[0] for r in rows],
                  "prompt": sum(r[1] for r in rows), "completion": sum(r[2] for r in rows),
                  "n": len(rows)}))
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


def _settle(prev, quiet_s=4.0, timeout_s=45.0):
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


def _run_and_measure(fleet, items, model):
    """Dispatch items (list of (key, worker, prompt)), wait, return the ledger duration_ms
    list + token sums for `model` over exactly this arm's window."""
    lo = _settle(_maxid())
    fleet.collect(fleet.dispatch_all(items))
    hi = _settle(lo)
    return _ledger(lo, hi, model)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--regions", default="benchmarks/results/swebench_multifile/regions")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--only", default="")
    ap.add_argument("--primary", default="codex")
    ap.add_argument("--primary-model", default="gpt-5.6-sol")
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    insts = [json.load(open(f)) for f in sorted(glob.glob(str(Path(args.regions) / "*.json")))]
    if args.only:
        keep = {s.strip() for s in args.only.split(",") if s.strip()}
        insts = [i for i in insts if i["instance_id"] in keep]
    if args.limit:
        insts = insts[:args.limit]
    if not insts:
        raise SystemExit("no instances")
    nonce = f"speed-{os.getpid()}-{int(time.time())}"
    for i in insts:
        i["problem"] = f"[{nonce}]\n{i['problem']}"
    fleet = B.Fleet(os.environ.get("AIMEE_BENCH_CLIENT", "aimee"))

    print(f"{len(insts)} instances; sole-tenant, one at a time; latency=ledger duration_ms",
          file=sys.stderr)
    per = {}
    for idx, inst in enumerate(insts, 1):
        iid = inst["instance_id"]
        files = D._files_of(inst)
        print(f"[{idx}/{len(insts)}] {iid} ({len(files)} files)", file=sys.stderr)
        # MONOLITHIC: one codex call, all files.
        mono = _run_and_measure(
            fleet, [(iid, args.primary, D._solve_prompt_multi(inst))], args.primary_model)
        # PARALLEL: one codex call per file.
        pitems = [((iid, k), args.primary,
                   D._file_prompt(inst, f, "Apply the part of the fix described in the issue "
                                           "that belongs in this file."))
                  for k, f in enumerate(files)]
        par = _run_and_measure(fleet, pitems, args.primary_model)
        mono_ms = mono["durations_ms"][0] if mono["durations_ms"] else None
        par_ms = max(par["durations_ms"]) if par["durations_ms"] else None
        speedup = round(mono_ms / par_ms, 2) if (mono_ms and par_ms) else None
        per[iid] = {"files": len(files),
                    "monolithic_ms": mono_ms, "parallel_max_ms": par_ms,
                    "parallel_calls_ms": sorted(par["durations_ms"], reverse=True),
                    "speedup": speedup,
                    "mono_tokens": mono["prompt"] + mono["completion"],
                    "parallel_tokens": par["prompt"] + par["completion"]}
        print(f"    monolithic={mono_ms}ms  parallel_max={par_ms}ms  speedup={speedup}x",
              file=sys.stderr)

    sp = [v["speedup"] for v in per.values() if v["speedup"]]
    faster = sum(1 for s in sp if s > 1)
    summary = {"instances": len(per), "with_data": len(sp),
               "parallel_faster_count": faster,
               "median_speedup": round(sorted(sp)[len(sp) // 2], 2) if sp else None,
               "mean_speedup": round(sum(sp) / len(sp), 2) if sp else None}
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"methodology": "pre-registered; latency=ledger duration_ms "
                               "(queue-free); same model both arms; no localize; sole tenant",
                               "summary": summary, "per_instance": per}, indent=2))
    print(f"\nSUMMARY: parallel faster on {faster}/{len(sp)}; "
          f"median {summary['median_speedup']}x, mean {summary['mean_speedup']}x", file=sys.stderr)
    print(f"written: {out}", file=sys.stderr)


if __name__ == "__main__":
    main()
