#!/usr/bin/env python3
"""Wait for the fleet to recover, then run the reddit10 cost-savings benchmark.

Polls agent health via tiny probe dispatches. Fires the real run only when the
required primary (gpt-5.6-sol via `codex`) AND >=2 distinct workers return live
content. Probes codex first and short-circuits, so a still-broken primary does
not hammer the worker breakers. Bounded by --max-hours; on timeout it exits
non-zero with a status line so the run can be rescheduled.
"""
from __future__ import annotations
import argparse, json, subprocess, sys, time
from pathlib import Path

PRIMARY_AGENT = "codex"
PRIMARY_MODEL = "gpt-5.6-sol"
WORKERS = ["local"]
AIMEE = "aimee"


def _run(*args, timeout=60):
    return subprocess.run([AIMEE, *args], capture_output=True, text=True, timeout=timeout)


def _probe(agent: str) -> bool:
    """Dispatch a tiny job to `agent`; True iff it finishes with non-empty content."""
    try:
        p = _run("--json", "delegate", "draft", "Reply with one word: OK",
                 "--via", agent, "--persona", "engineer", "--no-tools", "--background")
        jid = json.loads(p.stdout).get("job_id")
        if not jid:
            return False
    except Exception:
        return False
    t0 = time.time()
    while time.time() - t0 < 70:
        time.sleep(4)
        try:
            s = _run("--json", "delegate", "status", str(jid), "--full", timeout=30)
            d = json.loads(s.stdout)
            st = d.get("job_status")
            if st in ("done", "failed", "error"):
                return st == "done" and bool((d.get("result") or "").strip())
        except Exception:
            pass
    return False


def _healthy_workers() -> list[str]:
    return [w for w in WORKERS if _probe(w)]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--regions", default="benchmarks/results/swebench_supervised/regions")
    ap.add_argument("--output", default="benchmarks/results/cost_savings/reddit10.json")
    ap.add_argument("--interval", type=int, default=300, help="seconds between poll cycles")
    ap.add_argument("--max-hours", type=float, default=8.0)
    ap.add_argument("--min-workers", type=int, default=2)
    args = ap.parse_args()

    deadline = time.time() + args.max_hours * 3600
    cycle = 0
    while time.time() < deadline:
        cycle += 1
        ts = time.strftime("%H:%M:%S")
        if not _probe(PRIMARY_AGENT):
            print(f"[{ts}] cycle {cycle}: primary {PRIMARY_MODEL} still down; waiting {args.interval}s",
                  flush=True)
            time.sleep(args.interval)
            continue
        workers = _healthy_workers()
        if len(workers) < args.min_workers:
            print(f"[{ts}] cycle {cycle}: primary UP but only {len(workers)} workers healthy "
                  f"({workers}); need {args.min_workers}; waiting {args.interval}s", flush=True)
            time.sleep(args.interval)
            continue

        n = min(3, len(workers))
        print(f"[{ts}] FLEET READY: primary={PRIMARY_MODEL}, workers={workers}, n={n} -> running reddit10",
              flush=True)
        cmd = ["python3", "benchmarks/coding/bench_cost_savings.py",
               "--regions", args.regions,
               "--primary", PRIMARY_AGENT, "--primary-model", PRIMARY_MODEL,
               "--pool", ",".join(workers), "--n", str(n),
               "--output", args.output]
        r = subprocess.run(cmd)
        print(f"benchmark exited {r.returncode}; output -> {args.output}", flush=True)
        sys.exit(r.returncode)

    print(f"TIMEOUT after {args.max_hours}h: fleet did not recover (primary {PRIMARY_MODEL}). "
          f"Reschedule await_fleet_and_run.py.", flush=True)
    sys.exit(2)


if __name__ == "__main__":
    main()
