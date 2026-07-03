#!/usr/bin/env python3
"""Supervised SWE-bench benchmark: measures how much of the EXPENSIVE primary's
token spend aimee offloads onto a cheap/free delegate fleet, and whether it does
so without a wall-clock penalty, at equal resolution.

Two arms per instance (same code region given to both):
  A  primary_alone  - the expensive primary solves the task itself.
  C  supervised      - N diverse cheap/local workers attempt it CONCURRENTLY, then
                       the primary reviews the small candidate patches and selects
                       (or synthesizes) the best. The primary never reads the code,
                       only short candidate diffs, so its token cost stays tiny.

Reports the PRIMARY-agent token reduction (A vs C), per-arm wall-clock, and the
official SWE-bench resolution per arm. Delegate/worker tokens are free and reported
separately, not counted against the primary reduction.

Pipeline:
  1. swebench_supervised_prep.py  -> per-instance code regions (reddit10 | lite:N | all)
  2. this driver                   -> dispatch arms via the `aimee` CLI, collect patches
  3. official SWE-bench Docker grader (bench_swebench._grade_with_harness)
  4. supervised_report.py          -> the comparison table

Primary tokens are read from the aimee token_audit ledger (model=<primary>, realized,
within each arm's time window); pass --token-db <aimee.db> on the aimee host, else
token columns are left null and only speed/resolution are reported.

FAKE mode (AIMEE_BENCH_FAKE_AGENT=1) synthesizes arm outputs for CI (no live aimee).
"""
from __future__ import annotations
import argparse, glob, json, os, re, sqlite3, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from benchmarks.coding import supervised_report

_FAKE = os.environ.get("AIMEE_BENCH_FAKE_AGENT") == "1"
_FAKE_GRADER = os.environ.get("AIMEE_BENCH_FAKE_GRADER") == "1"


# ---------------------------------------------------------------- prompts -----
def _solve_prompt(inst: dict) -> str:
    return (f"You are fixing a bug in {inst['repo']}, file: {inst['file']}\n\n"
            f"## Issue\n{inst['problem']}\n\n## Code of {inst['file']}\n"
            f"```python\n{inst['region']}\n```\n\nReturn ONLY a unified git diff "
            f"(```diff fenced) that implements the fix for {inst['file']}, with a/ "
            f"and b/ prefixes and correct context lines.")


def _select_prompt(inst: dict, candidates: list[tuple[str, str]]) -> str:
    body = "".join(f"\n### Candidate {n + 1} (by {w})\n```diff\n{d}\n```\n"
                   for n, (w, d) in enumerate(candidates))
    return ("You are a senior engineer reviewing patches from your team for a bug. Pick the ONE "
            "candidate that correctly and completely fixes the issue, or if none is fully correct, "
            "output a corrected unified diff. Output ONLY the chosen/corrected unified git diff "
            f"(```diff fenced).\n\n## Issue\n{inst['problem']}\n## File: {inst['file']}\n"
            f"## Candidate patches{body}")


def _extract_diff(text: str) -> str:
    m = re.search(r"```diff\s*\n(.*?)```", text, re.DOTALL)
    if m:
        return m.group(1).strip()
    m = re.search(r"(diff --git[\s\S]*|--- a/[\s\S]*)", text)
    return m.group(1).strip() if m else ""


# ------------------------------------------------------------- aimee CLI ------
class Fleet:
    """Dispatch delegate turns via the `aimee` CLI and poll them CONCURRENTLY so
    wall-clock reflects true parallelism (not sequential per-job blocking)."""

    def __init__(self, aimee_bin: str, timeout_s: float = 420.0) -> None:
        self.aimee = aimee_bin
        self.timeout_s = timeout_s

    def dispatch(self, worker: str, prompt: str) -> tuple:
        p = subprocess.run(
            [self.aimee, "--json", "delegate", "draft", prompt, "--via", worker,
             "--persona", "engineer", "--no-tools", "--background"],
            capture_output=True, text=True, timeout=90)
        try:
            return json.loads(p.stdout).get("job_id"), time.time()
        except Exception:
            return None, time.time()

    def _status(self, jid) -> tuple:
        p = subprocess.run([self.aimee, "--json", "delegate", "status", str(jid), "--full"],
                           capture_output=True, text=True, timeout=30)
        try:
            d = json.loads(p.stdout)
            return d.get("job_status", ""), d.get("result", "")
        except Exception:
            return "", ""

    def collect(self, jobs: dict) -> tuple:
        """jobs: key -> (job_id, dispatch_time). Poll all concurrently; return
        ({key: (result_text, latency_s)}, first_dispatch, last_done)."""
        pending = {k: (j, t) for k, (j, t) in jobs.items() if j}
        firstd = min((t for _, t in pending.values()), default=time.time())
        out, lastdone = {}, firstd
        term = {"done", "failed", "error", "partial"}
        while pending:
            for k in list(pending):
                jid, td = pending[k]
                st, res = self._status(jid)
                if st in term:
                    out[k] = (res, round(time.time() - td, 1))
                    lastdone = max(lastdone, time.time())
                    del pending[k]
                elif time.time() - td > self.timeout_s:
                    out[k] = ("", round(time.time() - td, 1))
                    del pending[k]
            if pending:
                time.sleep(3)
        for k in jobs:
            out.setdefault(k, ("", 0.0))
        return out, firstd, lastdone


# ------------------------------------------------------------- token meter ----
def _primary_tokens(db, model, lo, hi):
    if not db or not Path(db).exists():
        return None
    c = sqlite3.connect(db)
    r = c.execute(
        "SELECT COUNT(*), COALESCE(SUM(prompt_tokens),0), COALESCE(SUM(completion_tokens),0) "
        "FROM token_audit WHERE model=? AND usage_kind='realized' "
        "AND created_at>=datetime(?, 'unixepoch') AND created_at<datetime(?, 'unixepoch')",
        (model, int(lo), int(hi) + 1)).fetchone()
    return {"calls": r[0], "input": r[1], "output": r[2], "total": r[1] + r[2]}


# ------------------------------------------------------------- arms -----------
def run_arm_A(insts, fleet, primary):
    jobs = {i["instance_id"]: fleet.dispatch(primary, _solve_prompt(i)) for i in insts}
    res, f, l = fleet.collect(jobs)
    return {iid: {"diff": _extract_diff(r), "wall": w} for iid, (r, w) in res.items()}, f, l


def run_arm_C(insts, fleet, primary, pool, n):
    order = {}  # (iid,k) -> worker name, for candidate labelling
    attempts, wi = {}, 0
    for i in insts:
        for k in range(n):
            w = pool[wi % len(pool)]
            order[(i["instance_id"], k)] = w
            attempts[(i["instance_id"], k)] = fleet.dispatch(w, _solve_prompt(i))
            wi += 1
    ares, wf, wl = fleet.collect(attempts)
    cand = {i["instance_id"]: [] for i in insts}
    for (iid, k), (r, _) in ares.items():
        d = _extract_diff(r)
        if d:
            cand[iid].append((order[(iid, k)], d))
    byid = {i["instance_id"]: i for i in insts}
    seljobs = {iid: fleet.dispatch(primary, _select_prompt(byid[iid], cs))
               for iid, cs in cand.items() if cs}
    sel_start = time.time()
    sres, _, sl = fleet.collect(seljobs)
    patches = {}
    for i in insts:
        iid = i["instance_id"]
        r, w = sres.get(iid, ("", 0.0))
        patches[iid] = {"diff": _extract_diff(r), "n_candidates": len(cand[iid]), "wall": w}
    return patches, wf, wl, sel_start, sl


# ------------------------------------------------------------- grading -------
def _grade(records, arm, out_dir, target):
    if _FAKE_GRADER:
        for r in records.values():
            r["resolved"] = None
        return
    from benchmarks.coding.bench_swebench import _grade_with_harness, _build_prediction, _write_predictions
    preds = [_build_prediction(iid, f"{target}-{arm}", r["diff"]) for iid, r in records.items() if r["diff"]]
    for r in records.values():
        r.setdefault("resolved", False)
    if not preds:
        return
    pp = out_dir / f"pred_{arm}.jsonl"
    _write_predictions(pp, preds)
    try:
        resolved, _ = _grade_with_harness(pp, f"{target}_{arm}_{int(time.time())}")
    except RuntimeError as e:
        print(f"WARNING grader unavailable: {e}", file=sys.stderr)
        return
    for iid, r in records.items():
        r["resolved"] = (iid in resolved) if r["diff"] else False


# ------------------------------------------------------------- fake ----------
def _fake_record(iid, arm, idx):
    if arm == "A":
        return {"diff": "diff --git a/x b/x\n", "wall": 130.0 + idx, "resolved": True}
    return {"diff": "diff --git a/x b/x\n", "wall": 40.0 + idx, "n_candidates": 3, "resolved": True}


# ------------------------------------------------------------- main ----------
def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--regions", default="benchmarks/results/swebench_supervised/regions")
    ap.add_argument("--arms", default="A,C")
    ap.add_argument("--primary", default=os.environ.get("AIMEE_BENCH_PRIMARY", "codex"),
                    help="the expensive manager agent (its tokens are what we reduce)")
    ap.add_argument("--pool", default=os.environ.get("AIMEE_BENCH_POOL",
                    "gpu-gemma4,glm-5.2,mimo-2.5,mistral,mimo-2.5-pro,minimax,local-synth"),
                    help="cheap/local worker fleet for arm C")
    ap.add_argument("--n", type=int, default=3, help="best-of-N workers per instance (arm C)")
    ap.add_argument("--primary-model", default=os.environ.get("AIMEE_BENCH_PRIMARY_MODEL", "gpt-5.5"),
                    help="token_audit model name of the primary, for token measurement")
    ap.add_argument("--token-db", default=os.environ.get("AIMEE_DB", ""),
                    help="path to aimee.db (DB1) to read primary tokens; empty = skip")
    ap.add_argument("--aimee-bin", default=os.environ.get("AIMEE_BENCH_CLIENT", "./aimee"))
    ap.add_argument("--target", default="aimee-supervised")
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    arms = [a.strip().upper() for a in args.arms.split(",") if a.strip()]
    pool = [w.strip() for w in args.pool.split(",") if w.strip()]

    if _FAKE:
        insts = [{"instance_id": f"inst-{i}", "repo": "x/y", "file": "x.py",
                  "region": "code", "problem": "bug"} for i in range(3)]
    else:
        insts = [json.load(open(f)) for f in sorted(glob.glob(str(Path(args.regions) / "*.json")))]
    if not insts:
        raise SystemExit(f"no regions in {args.regions}; run swebench_supervised_prep.py first")
    print(f"{len(insts)} instances; primary={args.primary}; pool={pool}; n={args.n}", file=sys.stderr)

    fleet = None if _FAKE else Fleet(args.aimee_bin)
    out_dir = Path(args.output).parent
    out_dir.mkdir(parents=True, exist_ok=True)
    result = {"instances": [i["instance_id"] for i in insts], "primary": args.primary,
              "pool": pool, "n": args.n, "arms": {}, "primary_tokens": {}}

    if "A" in arms:
        print("=== ARM A: primary solves ===", file=sys.stderr)
        if _FAKE:
            A = {i["instance_id"]: _fake_record(i["instance_id"], "A", k) for k, i in enumerate(insts)}
            result["arms"]["A"] = {"records": A, "wall_total": 130.0}
        else:
            A, f, l = run_arm_A(insts, fleet, args.primary)
            _grade(A, "A", out_dir, args.target)
            result["primary_tokens"]["A"] = _primary_tokens(args.token_db, args.primary_model, f, l)
            result["arms"]["A"] = {"records": A, "wall_total": round(l - f, 1)}

    if "C" in arms:
        print("=== ARM C: supervised (best-of-N + selection) ===", file=sys.stderr)
        if _FAKE:
            C = {i["instance_id"]: _fake_record(i["instance_id"], "C", k) for k, i in enumerate(insts)}
            result["arms"]["C"] = {"records": C, "wall_total": 40.0}
        else:
            C, wf, wl, sel_start, sl = run_arm_C(insts, fleet, args.primary, pool, args.n)
            _grade(C, "C", out_dir, args.target)
            result["primary_tokens"]["C"] = _primary_tokens(args.token_db, args.primary_model, sel_start, sl)
            result["arms"]["C"] = {"records": C, "wall_total": round(sl - wf, 1),
                                   "worker_wall": round(wl - wf, 1),
                                   "select_wall": round(sl - sel_start, 1)}

    result["summary"] = supervised_report.summarize_arms(result)
    Path(args.output).write_text(json.dumps(result, indent=2))
    print("\n" + supervised_report.render_supervised(result), file=sys.stderr)
    print(f"written to {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
