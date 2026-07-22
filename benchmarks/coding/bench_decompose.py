#!/usr/bin/env python3
"""Subtask-decomposition vs default (and best-of-N) with HONEST per-task latency.

Timing is server-side per-job duration (updated_at - created_at from each delegate
job's status), NOT client poll arrival -- so it is per-task and load-independent, and
we run with caps >= demand so no queue wait pollutes it. Latency is the arm's CRITICAL
PATH; token cost counts ONLY the manager (primary/codex) calls.

Arms (all on single-file regions -- see the note in the writeup: file-split is a no-op
on single-file lite tasks, so we test the within-file subtask split):

  P default  : codex solves the whole task.           latency = solve
               tokens  = codex solve
  G subtask  : codex PLANS k independent sub-edits ->  latency = plan + max(step) + merge
               k cheap workers implement them in       tokens  = codex plan + merge
               parallel -> codex MERGES.                        (workers are cheap models,
                                                                 excluded by the model filter)

Expectation on lite: G's fixed plan+merge overhead (two codex calls on the critical
path) likely exceeds any parallel-step win on a ~10-line single-file patch. We measure
it rather than assume it.

Example:
  python3 benchmarks/coding/bench_decompose.py \
    --regions benchmarks/results/swebench_supervised/regions \
    --primary codex --primary-model gpt-5.6-sol \
    --pool MiniMax-M3,mimo-v2.5-pro,kimi-k2.7-code \
    --output benchmarks/results/cost_savings/reddit10_decompose.json
"""
from __future__ import annotations
import argparse, glob, json, os, re, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
os.environ.setdefault("AIMEE_BENCH_CACHE_BUST", "1")
os.environ.setdefault("AIMEE_BENCH_POLL_WORKERS", "64")
os.environ.setdefault("AIMEE_BENCH_POLL_PERIOD", "1")
from benchmarks.coding import bench_swebench_supervised as B
from benchmarks.coding import cost_savings as C
from benchmarks.coding.bench_cost_savings import _maxid, _settle_maxid, _arm_tokens


# ------------------------------------------------------------- prompts --------
def _plan_prompt(inst: dict) -> str:
    return (f"You are fixing a bug in {inst['repo']}, file {inst['file']}.\n"
            f"Decompose the fix into 2-4 INDEPENDENT sub-edits that can each be "
            f"implemented separately without seeing the others. Output ONLY a JSON array "
            f'of short imperative step descriptions, e.g. ["...","..."].\n\n'
            f"## Issue\n{inst['problem']}\n\n## Code of {inst['file']}\n"
            f"```python\n{inst['region']}\n```")


def _step_prompt(inst: dict, step: str) -> str:
    return (f"You are fixing a bug in {inst['repo']}, file {inst['file']}.\n"
            f"Implement ONLY this sub-edit: {step}\n"
            f"Return ONLY a unified git diff (```diff fenced) for this change, with a/ and "
            f"b/ prefixes and correct context lines.\n\n"
            f"## Issue\n{inst['problem']}\n\n## Code of {inst['file']}\n"
            f"```python\n{inst['region']}\n```")


def _merge_prompt(inst: dict, partials: list[str]) -> str:
    body = "\n\n".join(f"### Partial {i + 1}\n```diff\n{d}\n```" for i, d in enumerate(partials))
    return (f"Merge these partial unified diffs for {inst['file']} into ONE coherent unified "
            f"diff, resolving any overlap. Return ONLY the merged diff (```diff fenced).\n\n{body}")


def _parse_steps(text: str, maxk: int = 4) -> list[str]:
    m = re.search(r"\[.*\]", text, re.DOTALL)
    if m:
        try:
            arr = json.loads(m.group(0))
            steps = [str(s).strip() for s in arr if str(s).strip()][:maxk]
            if steps:
                return steps
        except Exception:
            pass
    lines = [ln.strip("-*0123456789. ").strip() for ln in text.splitlines() if ln.strip()]
    return lines[:maxk] or ["Implement the fix described in the issue."]


# ------------------------------------------------------------- arms -----------
def arm_default(insts, fleet, primary):
    items = [(i["instance_id"], primary, B._cb(B._solve_prompt(i), f"D:{i['instance_id']}")) for i in insts]
    res = fleet.collect(fleet.dispatch_all(items))
    dur = dict(fleet.durations)
    return {iid: {"diff": B._extract_diff(r) if ok else "", "latency_s": dur.get(iid)}
            for iid, (r, ok) in res.items()}


def arm_subtask(insts, fleet, primary, pool):
    byid = {i["instance_id"]: i for i in insts}
    # Phase 1: manager plans k independent sub-edits.
    pitems = [(i["instance_id"], primary, B._cb(_plan_prompt(i), f"Gplan:{i['instance_id']}")) for i in insts]
    pres = fleet.collect(fleet.dispatch_all(pitems))
    pdur = dict(fleet.durations)
    steps = {iid: _parse_steps(pres.get(iid, ("", False))[0]) for iid in byid}
    # Phase 2: cheap workers implement the steps in parallel (round-robin over the pool).
    sitems = []
    for iid, sl in steps.items():
        for k, st in enumerate(sl):
            sitems.append(((iid, k), pool[k % len(pool)], B._cb(_step_prompt(byid[iid], st), f"Gstep:{iid}:{k}")))
    sres = fleet.collect(fleet.dispatch_all(sitems))
    sdur = dict(fleet.durations)
    partials = {iid: [] for iid in byid}
    stepmax = {iid: 0.0 for iid in byid}
    for (iid, k), (r, ok) in sres.items():
        d = B._extract_diff(r) if ok else ""
        if d:
            partials[iid].append(d)
        du = sdur.get((iid, k))
        if du is not None:
            stepmax[iid] = max(stepmax[iid], du)
    # Phase 3: manager merges the partial diffs.
    mitems = [(iid, primary, B._cb(_merge_prompt(byid[iid], ps), f"Gmerge:{iid}"))
              for iid, ps in partials.items() if ps]
    mres = fleet.collect(fleet.dispatch_all(mitems))
    mdur = dict(fleet.durations)
    recs = {}
    for iid in byid:
        r, ok = mres.get(iid, ("", False))
        cp = (pdur.get(iid) or 0) + stepmax[iid] + (mdur.get(iid) or 0)
        recs[iid] = {"diff": B._extract_diff(r) if ok else "", "n_steps": len(steps[iid]),
                     "plan_s": pdur.get(iid), "step_max_s": round(stepmax[iid], 1),
                     "merge_s": mdur.get(iid), "latency_s": round(cp, 1)}
    return recs


# ------------------------------------------------------------- multi-file ----
def _files_of(inst) -> list[dict]:
    """Normalize single-file (file/region) and multi-file (files:[...]) instances."""
    if inst.get("files"):
        return inst["files"]
    return [{"file": inst["file"], "region": inst["region"]}]


def _solve_prompt_multi(inst) -> str:
    fs = _files_of(inst)
    blocks = "\n\n".join(f"### {f['file']}\n```python\n{f['region']}\n```" for f in fs)
    return (f"You are fixing a bug in {inst['repo']}. The fix spans {len(fs)} files. Return a "
            f"SINGLE unified git diff (```diff fenced) covering ALL necessary changes across "
            f"every file, with a/ and b/ prefixes and correct paths.\n\n## Issue\n{inst['problem']}"
            f"\n\n## Files\n{blocks}")


def _localize_prompt(inst) -> str:
    # Names only, not file bodies: the manager just decides which file does what; the
    # per-file worker has the full region. Keeps this critical-path call small and fast.
    fs = _files_of(inst)
    names = "\n".join(f"- {f['file']}" for f in fs)
    return (f"You are fixing a bug in {inst['repo']} that spans these files. For EACH file, give "
            f"a one-line instruction for what to change in it to fix the issue "
            f'(or "SKIP" if it needs no change). Output ONLY a JSON object {{"<file>": "<instruction>"}}.'
            f"\n\n## Issue\n{inst['problem']}\n\n## Files\n{names}")


def _file_prompt(inst, f, instruction) -> str:
    return (f"You are fixing a bug in {inst['repo']}, file {f['file']}.\n"
            f"Make this change: {instruction}\n"
            f"Return ONLY a unified git diff (```diff fenced) for {f['file']}, with a/ and b/ "
            f"prefixes and correct context lines.\n\n## Issue\n{inst['problem']}\n\n"
            f"## Code of {f['file']}\n```python\n{f['region']}\n```")


def _parse_localize(text, files):
    m = re.search(r"\{.*\}", text, re.DOTALL)
    out = {}
    if m:
        try:
            d = json.loads(m.group(0))
            for f in files:
                v = d.get(f["file"])
                if v and "skip" not in str(v).lower():
                    out[f["file"]] = str(v)
        except Exception:
            pass
    if not out:  # fallback: everyone edits their file
        out = {f["file"]: "Apply the fix described in the issue to this file." for f in files}
    return out


def arm_default_multi(insts, fleet, primary):
    items = [(i["instance_id"], primary, B._cb(_solve_prompt_multi(i), f"Dm:{i['instance_id']}")) for i in insts]
    res = fleet.collect(fleet.dispatch_all(items))
    dur = dict(fleet.durations)
    return {iid: {"diff": B._extract_diff(r) if ok else "", "latency_s": dur.get(iid)}
            for iid, (r, ok) in res.items()}


def arm_filesplit(insts, fleet, primary, pool, localize=False):
    """Pure per-file parallelism (default): one worker per file, each gets the problem + its
    own file region and produces that file's diff, all in parallel; concat (files are
    independent). latency = max(T_file). Both arms already know the file set, so no manager
    'which files' pass is needed for a fair monolithic-vs-parallel comparison.

    localize=True adds a manager pre-pass that assigns a per-file instruction -- but that
    pass is a full reasoning call (31-132s here) that serializes in front of the parallel
    work and dominates the critical path, so it is OFF by default."""
    byid = {i["instance_id"]: i for i in insts}
    ldur = {}
    plan = {}
    if localize:
        litems = [(i["instance_id"], primary, B._cb(_localize_prompt(i), f"Floc:{i['instance_id']}")) for i in insts]
        lres = fleet.collect(fleet.dispatch_all(litems))
        ldur = dict(fleet.durations)
        plan = {iid: _parse_localize(lres.get(iid, ("", False))[0], _files_of(byid[iid])) for iid in byid}
    else:
        for iid in byid:
            plan[iid] = {f["file"]: "Apply the part of the fix described in the issue that belongs "
                                    "in this file." for f in _files_of(byid[iid])}
    witems = []
    for iid in byid:
        for k, (fp, instr) in enumerate(plan[iid].items()):
            fobj = next(f for f in _files_of(byid[iid]) if f["file"] == fp)
            witems.append(((iid, k), pool[k % len(pool)], B._cb(_file_prompt(byid[iid], fobj, instr), f"Ffile:{iid}:{k}")))
    wres = fleet.collect(fleet.dispatch_all(witems))
    wdur = dict(fleet.durations)
    diffs = {iid: [] for iid in byid}
    filemax = {iid: 0.0 for iid in byid}
    for (iid, k), (r, ok) in wres.items():
        d = B._extract_diff(r) if ok else ""
        if d:
            diffs[iid].append(d)
        du = wdur.get((iid, k))
        if du is not None:
            filemax[iid] = max(filemax[iid], du)
    recs = {}
    for iid in byid:
        cp = (ldur.get(iid) or 0) + filemax[iid]
        recs[iid] = {"diff": "\n".join(diffs[iid]), "n_files": len(plan[iid]),
                     "localize_s": ldur.get(iid), "file_max_s": round(filemax[iid], 1),
                     "latency_s": round(cp, 1)}
    return recs


def _crit(recs):
    xs = [r["latency_s"] for r in recs.values() if r.get("latency_s") is not None]
    return {"max_s": round(max(xs), 1) if xs else None,
            "median_s": round(sorted(xs)[len(xs) // 2], 1) if xs else None,
            "mean_s": round(sum(xs) / len(xs), 1) if xs else None}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--regions", default="benchmarks/results/swebench_supervised/regions")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--only", default="", help="comma-separated instance_ids to keep")
    ap.add_argument("--primary", default="codex")
    ap.add_argument("--primary-model", default="gpt-5.6-sol")
    ap.add_argument("--pool", default="MiniMax-M3,mimo-v2.5-pro,kimi-k2.7-code")
    ap.add_argument("--aimee-bin", default=os.environ.get("AIMEE_BENCH_CLIENT", "aimee"))
    ap.add_argument("--price-in", type=float, default=C.DEFAULT_PRICE["input_per_mtok"])
    ap.add_argument("--price-out", type=float, default=C.DEFAULT_PRICE["output_per_mtok"])
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    pool = [w.strip() for w in args.pool.split(",") if w.strip()]
    insts = [json.load(open(f)) for f in sorted(glob.glob(str(Path(args.regions) / "*.json")))]
    if args.only:
        keep = {s.strip() for s in args.only.split(",") if s.strip()}
        insts = [i for i in insts if i["instance_id"] in keep]
    if args.limit:
        insts = insts[:args.limit]
    if not insts:
        raise SystemExit(f"no regions in {args.regions}")
    nonce = f"decomp-{os.getpid()}-{int(time.time())}"
    for i in insts:
        i["problem"] = f"[{nonce}]\n{i['problem']}"
    price = {"input_per_mtok": args.price_in, "output_per_mtok": args.price_out}
    cin, cout = args.price_in / 1e6, args.price_out / 1e6
    fleet = B.Fleet(args.aimee_bin)
    print(f"{len(insts)} instances; primary={args.primary}; pool={pool}", file=sys.stderr)
    _maxid()  # confirm ledger reachable

    multi = any(i.get("files") for i in insts)
    split_name = "file-split (localize -> per-file workers -> concat)" if multi \
        else "subtask-split (plan -> parallel steps -> merge)"

    print(f"=== ARM P: default (codex solves whole task, {'multi-file' if multi else 'single-file'}) ===",
          file=sys.stderr)
    p_lo = _maxid()
    P = arm_default_multi(insts, fleet, args.primary) if multi else arm_default(insts, fleet, args.primary)
    p_hi = _settle_maxid(p_lo)
    p_tok = _arm_tokens(p_lo, p_hi, args.primary_model)

    print(f"=== ARM G: {split_name} ===", file=sys.stderr)
    g_lo = _settle_maxid(_maxid())
    G = arm_filesplit(insts, fleet, args.primary, pool) if multi \
        else arm_subtask(insts, fleet, args.primary, pool)
    g_hi = _settle_maxid(g_lo)
    g_tok = _arm_tokens(g_lo, g_hi, args.primary_model)  # manager (codex) calls only

    p_cost = cin * p_tok.realized_prompt + cout * p_tok.realized_completion
    g_cost = cin * g_tok.realized_prompt + cout * g_tok.realized_completion
    p_time, g_time = _crit(P), _crit(G)

    result = {
        "provenance": {"primary": args.primary, "primary_model": args.primary_model,
                       "pool": pool, "instances": [i["instance_id"] for i in insts],
                       "timing": "server-side per-job duration (updated_at-created_at); critical path",
                       "split_arm": split_name,
                       "ledger_windows": {"default": [p_lo, p_hi], "split": [g_lo, g_hi]}},
        "tokens": {"default": vars(p_tok), "split_manager": vars(g_tok)},
        "cost_usd": {"default": round(p_cost, 4), "split_manager": round(g_cost, 4)},
        "latency_s": {"default": p_time, "split": g_time},
        "per_instance": {iid: {"default_latency_s": P.get(iid, {}).get("latency_s"),
                               "split_latency_s": G.get(iid, {}).get("latency_s"),
                               **{k: v for k, v in G.get(iid, {}).items() if k != "diff"}}
                         for iid in [i["instance_id"] for i in insts]},
    }
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2))

    dt = 100 * (p_tok.realized_total - g_tok.realized_total) / p_tok.realized_total if p_tok.realized_total else 0
    print(f"\ntokens : default={p_tok.realized_total:,} manager(split)={g_tok.realized_total:,} ({-dt:+.1f}%)",
          file=sys.stderr)
    print(f"cost   : default=${p_cost:.4f} split=${g_cost:.4f}", file=sys.stderr)
    print(f"latency: default max={p_time['max_s']}s med={p_time['median_s']}s  "
          f"split max={g_time['max_s']}s med={g_time['median_s']}s", file=sys.stderr)
    print(f"written: {out}", file=sys.stderr)


if __name__ == "__main__":
    main()
