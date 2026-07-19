#!/usr/bin/env python3
"""Supervised SWE-bench benchmark (single-shot diff generation).

Measures how much of the EXPENSIVE primary's token spend aimee offloads onto a
cheap/free delegate fleet when the task is a one-shot "here is the code, produce a
diff" request, plus wall-clock and official SWE-bench resolution.

SCOPE / HONESTY: this is a **single-shot diff-generation** benchmark (both arms run
`--no-tools`, one prompt -> one diff). It is NOT the tool-using agentic workflow the
Reddit result measured, so it is not a like-for-like reproduction of that experiment;
treat the Reddit number as motivation, not a head-to-head baseline. An agentic version
(workers explore/edit/test the repo, primary supervises) is tracked separately as the
real parity claim. See SUPERVISED_SWEBENCH.md.

Two arms per instance (same code region given to both):
  A  primary_alone  - the expensive primary produces the diff itself.
  C  supervised      - N distinct cheap/local workers attempt it CONCURRENTLY, then the
                       primary reviews the candidate diffs and selects/synthesizes the
                       best (best-of-N). It reads candidate diffs, not the code region.

Primary tokens are read from the token_audit ledger scoped to the EXACT primary job ids
this run dispatched (delegation_id ends in `-<job_id>`), so the number is the primary's
own turns for this run only - no time-window or cross-agent contamination. Worker tokens
are free and reported separately. FAKE mode (AIMEE_BENCH_FAKE_AGENT=1) synthesizes arm
outputs for CI.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import random
import re
import sqlite3
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from benchmarks.coding import supervised_report

_FAKE = os.environ.get("AIMEE_BENCH_FAKE_AGENT") == "1"
_FAKE_GRADER = os.environ.get("AIMEE_BENCH_FAKE_GRADER") == "1"
_POLL_WORKERS = int(os.environ.get("AIMEE_BENCH_POLL_WORKERS", "16"))
_POLL_PERIOD = float(os.environ.get("AIMEE_BENCH_POLL_PERIOD", "2"))
_SCHEMA_VERSION = 2

# When set, append a unique marker to every dispatched prompt so the server-side
# draft/summarize/format result cache (keyed on (role, prompt), model-agnostic)
# never returns a shared hit. Required for a truthful cold best-of-N cost/time
# run: without it, Arm S's worker candidates collapse onto Arm P's cached codex
# drafts (and onto each other), so the cheap workers never actually run.
_CACHE_BUST = os.environ.get("AIMEE_BENCH_CACHE_BUST") == "1"


def _cb(prompt: str, tag: str) -> str:
    return f"{prompt}\n\n<!-- cachebust:{tag} -->" if _CACHE_BUST else prompt


def _ts_delta(created: str, updated: str):
    """Server-side job duration in seconds from status timestamps ('YYYY-MM-DD HH:MM:SS').
    Returns None if either is missing/unparseable. Second-granularity but per-job and
    load-independent -- unlike client poll arrival, which is quantized to the poll period
    and synchronized across a batch."""
    from datetime import datetime
    fmt = "%Y-%m-%d %H:%M:%S"
    try:
        c = datetime.strptime(created[:19], fmt)
        u = datetime.strptime(updated[:19], fmt)
        d = (u - c).total_seconds()
        return round(d, 1) if d >= 0 else None
    except Exception:
        return None


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


_DIFF_LINE = re.compile(r"^(diff --git |index |--- |\+\+\+ |@@ |[ +\-\\]|rename |similarity |"
                        r"new file |deleted file |old mode |new mode |Binary )")


def _extract_diff(text: str) -> str:
    """Pull a unified diff out of a worker response and STOP at the first line that is
    not part of the diff, so trailing prose is never submitted as a patch (M5)."""
    m = re.search(r"```diff\s*\n(.*?)```", text, re.DOTALL)
    if m:
        return m.group(1).strip()
    lines = text.splitlines()
    start = next((i for i, ln in enumerate(lines)
                  if ln.startswith("diff --git ") or ln.startswith("--- ")), None)
    if start is None:
        return ""
    out = []
    for ln in lines[start:]:
        if ln == "" or _DIFF_LINE.match(ln):
            out.append(ln)
        else:
            break
    return "\n".join(out).strip()


# ------------------------------------------------------------- aimee CLI ------
class Fleet:
    """Dispatch and poll delegate turns via the `aimee` CLI. Dispatch AND polling are
    THREAD-PARALLEL so wall-clock reflects true fleet concurrency, not driver serialism
    (C2/M1/M3). Only `done` is a success terminal; `failed`/`error`/`partial` are
    discarded (M2)."""

    _SUCCESS = "done"
    _TERMINAL = {"done", "failed", "error", "partial"}

    def __init__(self, aimee_bin: str, timeout_s: float = 420.0) -> None:
        self.aimee = aimee_bin
        self.timeout_s = timeout_s

    def _dispatch1(self, worker: str, prompt: str):
        # Prompt via stdin, not argv -- multi-file solve prompts run to hundreds of KB
        # and overflow the OS argument-length limit (Errno 7) when passed positionally.
        p = subprocess.run(
            [self.aimee, "--json", "delegate", "draft", "--prompt-stdin", "--via", worker,
             "--persona", "engineer", "--no-tools", "--background"],
            input=prompt, capture_output=True, text=True, timeout=90)
        try:
            return json.loads(p.stdout).get("job_id")
        except Exception:
            return None

    def dispatch_all(self, items: list[tuple]) -> dict:
        """items: list of (key, worker, prompt). Returns {key: job_id}. Parallel."""
        with ThreadPoolExecutor(max_workers=_POLL_WORKERS) as ex:
            jids = list(ex.map(lambda it: self._dispatch1(it[1], it[2]), items))
        return {it[0]: j for it, j in zip(items, jids)}

    def _status(self, jid):
        p = subprocess.run([self.aimee, "--json", "delegate", "status", str(jid), "--full"],
                           capture_output=True, text=True, timeout=30)
        try:
            d = json.loads(p.stdout)
            return (d.get("job_status", ""), d.get("result", ""),
                    d.get("created_at", ""), d.get("updated_at", ""))
        except Exception:
            return "", "", "", ""

    def collect(self, jobs: dict, t0: float | None = None) -> dict:
        """jobs: {key: job_id}. Poll concurrently until all terminal/timeout. Returns
        {key: (result_or_'', ok_bool)} where ok=True only for a `done` terminal.

        Records per-key wall arrival time (seconds since t0, default: collect start)
        into self.arrivals for every key that reaches a terminal state, so callers can
        recover time-to-first-viable-response. Arrival granularity == the poll period."""
        t0 = time.time() if t0 is None else t0
        self.arrivals = {}
        self.durations = {}  # per-key server-side duration (updated_at - created_at), seconds
        pending = {k: (j, time.time()) for k, j in jobs.items() if j}
        out = {}
        while pending:
            with ThreadPoolExecutor(max_workers=_POLL_WORKERS) as ex:
                sts = dict(zip(pending, ex.map(lambda k: self._status(pending[k][0]), pending)))
            for k, (st, res, cat, uat) in sts.items():
                jid, td = pending[k]
                if st in self._TERMINAL:
                    out[k] = (res if st == self._SUCCESS else "", st == self._SUCCESS)
                    self.arrivals[k] = round(time.time() - t0, 2)
                    self.durations[k] = _ts_delta(cat, uat)
                    del pending[k]
                elif time.time() - td > self.timeout_s:
                    out[k] = ("", False)
                    del pending[k]
            if pending:
                time.sleep(_POLL_PERIOD)
        for k in jobs:
            out.setdefault(k, ("", False))
        return out


# ------------------------------------------------------------- token meter ----
def _primary_tokens(db, model, job_ids):
    """Sum the primary model's realized tokens for EXACTLY the given primary job ids.
    A delegate turn's delegation_id is `deleg-<n>-<ts>-<job_id>`, so we match the
    trailing `-<job_id>`. No time window -> no cross-run/cross-agent contamination (C1)."""
    if not db or not Path(db).exists() or not job_ids:
        return None
    con = sqlite3.connect(db)
    likes = " OR ".join(["delegation_id LIKE ?"] * len(job_ids))
    params = [model] + [f"%-{j}" for j in job_ids]
    r = con.execute(
        f"SELECT COUNT(*), COALESCE(SUM(prompt_tokens),0), COALESCE(SUM(completion_tokens),0) "
        f"FROM token_audit WHERE model=? AND usage_kind='realized' AND ({likes})", params).fetchone()
    return {"calls": r[0], "input": r[1], "output": r[2], "total": r[1] + r[2],
            "job_ids": list(job_ids)}


# ------------------------------------------------------------- arms -----------
def run_arm_A(insts, fleet, primary):
    items = [(i["instance_id"], primary, _cb(_solve_prompt(i), f"P:{i['instance_id']}")) for i in insts]
    t0 = time.perf_counter()
    tw0 = time.time()
    jobs = fleet.dispatch_all(items)
    res = fleet.collect(jobs, tw0)
    wall = round(time.perf_counter() - t0, 1)
    arr = dict(fleet.arrivals)
    # solve_s: wall time from dispatch to this instance's single-shot solve completing.
    recs = {iid: {"diff": _extract_diff(r) if ok else "", "solve_s": arr.get(iid)}
            for iid, (r, ok) in res.items()}
    return recs, wall, [j for j in jobs.values() if j]


def _pick_workers(pool, n, seed):
    """N DISTINCT workers for one instance, seeded for reproducibility (M6)."""
    rng = random.Random(seed)
    p = list(pool)
    rng.shuffle(p)
    return p[:min(n, len(p))]


def run_arm_C_candidates(insts, fleet, pool, n, seed):
    """Phase 1 of the supervised arm: N workers race to draft a candidate per instance.
    Returns (cand, first_viable_s, wall, jobs). Worker tokens are NOT the manager cost
    (they belong to the cheap pool -- even when a worker happens to be the same model as
    the manager), so bench_cost_savings brackets this phase's ledger window separately
    and does NOT count it toward cost. first_viable_s is the delegate runtime metric."""
    t0 = time.perf_counter()
    tw0 = time.time()
    items, order = [], {}
    for i in insts:
        for k, w in enumerate(_pick_workers(pool, n, f"{seed}:{i['instance_id']}")):
            key = (i["instance_id"], k)
            order[key] = w
            items.append((key, w, _cb(_solve_prompt(i), f"S:{i['instance_id']}:{k}")))
    ajobs = fleet.dispatch_all(items)
    ares = fleet.collect(ajobs, tw0)
    cand_arr = dict(fleet.arrivals)  # (iid,k) -> wall seconds from candidate dispatch
    cand = {i["instance_id"]: [] for i in insts}
    first_viable = {i["instance_id"]: None for i in insts}
    for (iid, k), (r, ok) in ares.items():
        d = _extract_diff(r) if ok else ""
        if d:
            cand[iid].append((order[(iid, k)], d))
            a = cand_arr.get((iid, k))
            if a is not None and (first_viable[iid] is None or a < first_viable[iid]):
                first_viable[iid] = a
    wall = round(time.perf_counter() - t0, 1)
    return cand, first_viable, wall, [j for j in ajobs.values() if j]


def run_arm_C_select(insts, fleet, primary, cand):
    """Phase 2: the MANAGER (primary) selects among each instance's candidates. This is
    the ONLY phase whose primary-model tokens count as the delegate cost. Returns
    (recs, wall, jobs)."""
    t0 = time.perf_counter()
    byid = {i["instance_id"]: i for i in insts}
    sel_items = [(iid, primary, _cb(_select_prompt(byid[iid], cs), f"Ssel:{iid}"))
                 for iid, cs in cand.items() if cs]
    sjobs = fleet.dispatch_all(sel_items)
    sres = fleet.collect(sjobs)
    recs = {}
    for i in insts:
        iid = i["instance_id"]
        r, ok = sres.get(iid, ("", False))
        recs[iid] = {"diff": _extract_diff(r) if ok else "", "n_candidates": len(cand[iid])}
    wall = round(time.perf_counter() - t0, 1)
    return recs, wall, [j for j in sjobs.values() if j]


def run_arm_C(insts, fleet, primary, pool, n, seed):
    """Full supervised arm = candidates + manager selection (single ledger bracket).
    bench_cost_savings calls the two phases directly so it can bracket the manager
    window separately; other callers use this convenience wrapper."""
    cand, first_viable, w1, _ = run_arm_C_candidates(insts, fleet, pool, n, seed)
    recs, w2, sjobs = run_arm_C_select(insts, fleet, primary, cand)
    for iid in recs:
        recs[iid]["first_viable_s"] = first_viable.get(iid)
    return recs, round(w1 + w2, 1), sjobs


# ------------------------------------------------------------- grading -------
def _grade(records, arm, out_dir, target):
    for r in records.values():
        r["resolved"] = None  # None = not graded (no diff, or grader unavailable)
    if _FAKE_GRADER:
        return
    if _FAKE:
        raise SystemExit("refusing to grade fake agent output against the real harness; "
                         "set AIMEE_BENCH_FAKE_GRADER=1 too (m1)")
    from benchmarks.coding.bench_swebench import _grade_with_harness, _build_prediction, _write_predictions
    preds = [_build_prediction(iid, f"{target}-{arm}", r["diff"]) for iid, r in records.items() if r["diff"]]
    if not preds:
        return
    for iid, r in records.items():
        if r["diff"]:
            r["resolved"] = False  # submitted but not yet confirmed resolved
    pp = out_dir / f"pred_{arm}.jsonl"
    _write_predictions(pp, preds)
    try:
        resolved, _ = _grade_with_harness(pp, f"{target}_{arm}_{int(time.time())}")
    except RuntimeError as e:
        print(f"WARNING grader unavailable: {e}", file=sys.stderr)
        for r in records.values():
            r["resolved"] = None
        return
    for iid, r in records.items():
        if r["diff"]:
            r["resolved"] = iid in resolved


# ------------------------------------------------------------- fake ----------
def _fake_record(iid, arm, idx, n=3):
    if arm == "A":
        return {"diff": "diff --git a/x b/x\n", "resolved": True}
    return {"diff": "diff --git a/x b/x\n", "n_candidates": n, "resolved": True}


def _provenance(args, pool, n_instances):
    def _git(*a):
        try:
            return subprocess.run(["git", *a], capture_output=True, text=True, timeout=10).stdout.strip()
        except Exception:
            return None
    return {"schema_version": _SCHEMA_VERSION, "aimee_commit": _git("rev-parse", "HEAD"),
            "primary_agent": args.primary, "primary_model": args.primary_model,
            "pool_agents": pool, "n": args.n, "instances": n_instances,
            "single_shot": True, "started_at": int(time.time())}


# ------------------------------------------------------------- main ----------
def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--regions", default="benchmarks/results/swebench_supervised/regions")
    ap.add_argument("--arms", default="A,C")
    ap.add_argument("--primary", default=os.environ.get("AIMEE_BENCH_PRIMARY", "codex"),
                    help="the expensive manager agent (its tokens are what we reduce)")
    ap.add_argument("--pool", default=os.environ.get("AIMEE_BENCH_POOL",
                    "glm-5.2,mimo-2.5,mistral,mimo-2.5-pro,minimax,local-synth"),
                    help="cheap/local worker fleet for arm C (must NOT contain --primary)")
    ap.add_argument("--n", type=int, default=3, help="best-of-N workers per instance (arm C)")
    ap.add_argument("--primary-model", default=os.environ.get("AIMEE_BENCH_PRIMARY_MODEL", ""),
                    help="token_audit model name of the primary; required for token measurement")
    ap.add_argument("--token-db", default=os.environ.get("AIMEE_DB", ""),
                    help="path to aimee.db (DB1) to read primary tokens")
    ap.add_argument("--aimee-bin", default=os.environ.get("AIMEE_BENCH_CLIENT", "./aimee"))
    ap.add_argument("--target", default="aimee-supervised")
    ap.add_argument("--seed", type=int, default=1729)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    arms = [a.strip().upper() for a in args.arms.split(",") if a.strip()]
    pool = [w.strip() for w in args.pool.split(",") if w.strip()]
    if args.primary in pool:
        raise SystemExit(f"--primary '{args.primary}' must not appear in --pool (m11 footgun)")

    if _FAKE:
        insts = [{"instance_id": f"inst-{i}", "repo": "x/y", "file": "x.py",
                  "region": "code", "problem": "bug"} for i in range(3)]
    else:
        insts = [json.load(open(f)) for f in sorted(glob.glob(str(Path(args.regions) / "*.json")))]
        if not args.token_db:
            print("WARNING: --token-db not set; primary token reduction will be unavailable", file=sys.stderr)
        if args.token_db and not args.primary_model:
            raise SystemExit("--primary-model is required with --token-db (to scope the ledger)")
    if not insts:
        raise SystemExit(f"no regions in {args.regions}; run swebench_supervised_prep.py first")
    print(f"{len(insts)} instances; primary={args.primary}; pool={pool}; n={args.n}", file=sys.stderr)

    fleet = None if _FAKE else Fleet(args.aimee_bin)
    out_dir = Path(args.output).parent
    out_dir.mkdir(parents=True, exist_ok=True)
    result = {"provenance": _provenance(args, pool, len(insts)),
              "instances": [i["instance_id"] for i in insts], "primary": args.primary,
              "pool": pool, "n": args.n, "arms": {}, "primary_tokens": {}}

    if "A" in arms:
        print("=== ARM A: primary solves (single-shot) ===", file=sys.stderr)
        if _FAKE:
            A = {i["instance_id"]: _fake_record(i["instance_id"], "A", k, args.n) for k, i in enumerate(insts)}
            result["arms"]["A"] = {"records": A, "wall_total": 0.0}
        else:
            A, wall, jids = run_arm_A(insts, fleet, args.primary)
            _grade(A, "A", out_dir, args.target)
            result["primary_tokens"]["A"] = _primary_tokens(args.token_db, args.primary_model, jids)
            result["arms"]["A"] = {"records": A, "wall_total": wall}

    if "C" in arms:
        print("=== ARM C: supervised best-of-N + selection (single-shot) ===", file=sys.stderr)
        if _FAKE:
            C = {i["instance_id"]: _fake_record(i["instance_id"], "C", k, args.n) for k, i in enumerate(insts)}
            result["arms"]["C"] = {"records": C, "wall_total": 0.0}
        else:
            C, wall, jids = run_arm_C(insts, fleet, args.primary, pool, args.n, args.seed)
            _grade(C, "C", out_dir, args.target)
            result["primary_tokens"]["C"] = _primary_tokens(args.token_db, args.primary_model, jids)
            result["arms"]["C"] = {"records": C, "wall_total": wall}

    result["summary"] = supervised_report.summarize_arms(result)
    Path(args.output).write_text(json.dumps(result, indent=2))
    print("\n" + supervised_report.render_supervised(result), file=sys.stderr)
    print(f"written to {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
