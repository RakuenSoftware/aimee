#!/usr/bin/env python3
"""Report builder for the supervised SWE-bench benchmark.

Turns per-(instance, arm) measurement records into the Reddit-comparable table
plus the honest aggregates the roundtable required. Pure functions only — no I/O
beyond string formatting — so the unit tests can exercise the math directly.

The experiment compares three arms per SWE-bench instance:
  A  primary_alone       — the expensive supervisor (the primary agent) solves
                           the whole task itself. This is the token BASELINE.
  B  supervised_serial   — supervisor + ONE delegate worker (Reddit parity).
  C  supervised_parallel — supervisor + N parallel free/subscription workers.

"Supervisor" tokens are the primary agent's OWN spend (token_audit rows with an
empty delegation_id); "worker" tokens are the delegate children (delegation_id
set), which are priced at $0. The whole point of the experiment is the drop in
SUPERVISOR tokens between arm A (do everything) and arm C (orchestrate), while
resolution holds — and, unlike the Reddit result, WITHOUT a wall-clock penalty
because the workers run in parallel.

Record schema (one dict per instance/arm/compaction cell):
  {
    "instance_id": str,
    "arm": "A" | "B" | "C",
    "compaction": bool,                 # S4 tool-result compaction on/off
    "supervisor_input_tokens": int,     # token_audit, delegation_id empty
    "supervisor_output_tokens": int,
    "worker_input_tokens": int,         # token_audit, delegation_id set ($0)
    "worker_output_tokens": int,
    "wall_s": float,                    # prompt submit -> patch ready for grading
    "resolved": bool | None,            # official SWE-bench Docker grader
    "n_workers": int,                   # parallelism actually used this cell
    "provider_errors": dict,            # optional {provider: {"429": n, "5xx": n}}
    "invalid": bool,                    # patch-conflict / ownership violation -> excluded
  }
"""

from __future__ import annotations

import math
import random
import statistics
from typing import Any, Iterable

ARM_LABELS = {
    "A": "primary_alone",
    "B": "supervised_serial",
    "C": "supervised_parallel",
}


def pct_reduction(baseline: float, value: float) -> float | None:
    """Percent reduction of `value` relative to `baseline`.

    Positive = value is smaller than baseline (a saving). Returns None when the
    baseline is zero (reduction undefined), so callers never divide by zero or
    print a fake 0%/100%.
    """
    if baseline == 0:
        return None
    return round(100.0 * (baseline - value) / baseline, 1)


def _sup_total(rec: dict[str, Any]) -> int:
    return int(rec.get("supervisor_input_tokens", 0)) + int(rec.get("supervisor_output_tokens", 0))


def _worker_total(rec: dict[str, Any]) -> int:
    return int(rec.get("worker_input_tokens", 0)) + int(rec.get("worker_output_tokens", 0))


def _total_llm(rec: dict[str, Any]) -> int:
    return _sup_total(rec) + _worker_total(rec)


def _bootstrap_ci(
    values: list[float], iters: int = 2000, alpha: float = 0.05, seed: int = 42
) -> tuple[float, float]:
    """Percentile bootstrap CI for the mean. Deterministic (seeded) so a rerun of
    the report on the same data yields identical bounds."""
    if not values:
        return (0.0, 0.0)
    if len(values) == 1:
        return (values[0], values[0])
    rng = random.Random(seed)
    n = len(values)
    means = []
    for _ in range(iters):
        sample = [values[rng.randrange(n)] for _ in range(n)]
        means.append(sum(sample) / n)
    means.sort()
    lo = means[int((alpha / 2) * iters)]
    hi = means[int((1 - alpha / 2) * iters)]
    return (round(lo, 3), round(hi, 3))


def _index(records: Iterable[dict[str, Any]], compaction: bool) -> dict[tuple[str, str], dict]:
    """Index valid records by (instance_id, arm) for the chosen compaction setting."""
    out: dict[tuple[str, str], dict] = {}
    for r in records:
        if r.get("invalid"):
            continue
        if bool(r.get("compaction", False)) != compaction:
            continue
        out[(r["instance_id"], r["arm"])] = r
    return out


def _arm_pair_table(
    idx: dict[tuple[str, str], dict], baseline_arm: str, treatment_arm: str
) -> dict[str, Any]:
    """Per-instance + aggregate supervisor/total-token reduction of `treatment_arm`
    vs `baseline_arm`, over instances present in BOTH arms (paired)."""
    instances = sorted({i for (i, a) in idx if a == baseline_arm} & {i for (i, a) in idx if a == treatment_arm})
    rows = []
    sum_base_in = sum_base_out = sum_treat_in = sum_treat_out = 0
    sum_base_total_llm = sum_treat_total_llm = 0
    in_ratios: list[float] = []
    out_ratios: list[float] = []
    for inst in instances:
        b = idx[(inst, baseline_arm)]
        t = idx[(inst, treatment_arm)]
        bi, bo = int(b["supervisor_input_tokens"]), int(b["supervisor_output_tokens"])
        ti, to = int(t["supervisor_input_tokens"]), int(t["supervisor_output_tokens"])
        sum_base_in += bi
        sum_base_out += bo
        sum_treat_in += ti
        sum_treat_out += to
        sum_base_total_llm += _total_llm(b)
        sum_treat_total_llm += _total_llm(t)
        ri = pct_reduction(bi, ti)
        ro = pct_reduction(bo, to)
        if ri is not None:
            in_ratios.append(ri)
        if ro is not None:
            out_ratios.append(ro)
        rows.append(
            {
                "instance_id": inst,
                "supervisor_input_reduction_pct": ri,
                "supervisor_output_reduction_pct": ro,
                "baseline_supervisor_tokens": bi + bo,
                "treatment_supervisor_tokens": ti + to,
                "treatment_worker_tokens": _worker_total(t),
            }
        )
    return {
        "baseline_arm": baseline_arm,
        "treatment_arm": treatment_arm,
        "n_instances": len(instances),
        "rows": rows,
        # Micro (sum-based) aggregate — the honest headline: total tokens saved,
        # not the mean of per-task percentages (which small-token tasks distort).
        "micro_supervisor_input_reduction_pct": pct_reduction(sum_base_in, sum_treat_in),
        "micro_supervisor_output_reduction_pct": pct_reduction(sum_base_out, sum_treat_out),
        "micro_supervisor_total_reduction_pct": pct_reduction(
            sum_base_in + sum_base_out, sum_treat_in + sum_treat_out
        ),
        # Macro (mean of per-task ratios) — reported alongside, labelled distinctly.
        "macro_supervisor_input_reduction_pct": round(statistics.fmean(in_ratios), 1) if in_ratios else None,
        "macro_supervisor_output_reduction_pct": round(statistics.fmean(out_ratios), 1) if out_ratios else None,
        "macro_input_ci": _bootstrap_ci(in_ratios) if in_ratios else None,
        # Total-LLM delta (supervisor + free workers): keeps us honest that the
        # compute did not vanish — it MOVED onto $0 workers.
        "baseline_total_llm_tokens": sum_base_total_llm,
        "treatment_total_llm_tokens": sum_treat_total_llm,
        "total_llm_reduction_pct": pct_reduction(sum_base_total_llm, sum_treat_total_llm),
        "baseline_supervisor_tokens": sum_base_in + sum_base_out,
        "treatment_supervisor_tokens": sum_treat_in + sum_treat_out,
    }


def _wall_summary(idx: dict[tuple[str, str], dict], arm: str) -> dict[str, Any]:
    walls = [float(r["wall_s"]) for (i, a), r in idx.items() if a == arm and r.get("wall_s") is not None]
    if not walls:
        return {"arm": arm, "n": 0}
    walls_sorted = sorted(walls)
    return {
        "arm": arm,
        "n": len(walls),
        "total_s": round(sum(walls), 1),
        "median_s": round(statistics.median(walls), 1),
        "p95_s": round(walls_sorted[min(len(walls) - 1, int(round(0.95 * (len(walls) - 1))))], 1),
    }


def _resolution(idx: dict[tuple[str, str], dict], arm: str) -> dict[str, Any]:
    graded = [(i, a) for (i, a) in idx if a == arm and idx[(i, a)].get("resolved") is not None]
    n = len(graded)
    resolved = sum(1 for k in graded if idx[k]["resolved"])
    return {
        "arm": arm,
        "graded": n,
        "resolved": resolved,
        "resolve_rate": round(resolved / n, 4) if n else None,
    }


def build_report(records: list[dict[str, Any]], *, compaction: bool = True) -> dict[str, Any]:
    """Assemble the full report for one compaction setting. Pairs arms by instance
    so A/B/C are compared on the SAME tasks."""
    idx = _index(records, compaction=compaction)
    arms_present = sorted({a for (_, a) in idx})

    report: dict[str, Any] = {
        "compaction": compaction,
        "arms_present": arms_present,
        "arm_labels": {a: ARM_LABELS.get(a, a) for a in arms_present},
        "comparisons": {},
        "wall_clock": {a: _wall_summary(idx, a) for a in arms_present},
        "resolution": {a: _resolution(idx, a) for a in arms_present},
    }

    # Reddit-comparable: B-vs-A (parity) and the headline C-vs-A (aimee's edge).
    for treat in ("B", "C"):
        if "A" in arms_present and treat in arms_present:
            report["comparisons"][f"{treat}_vs_A"] = _arm_pair_table(idx, "A", treat)

    # Speed: ratios vs the A baseline and vs the serial B (parallelism payoff).
    wc = report["wall_clock"]
    speed: dict[str, Any] = {}
    if "A" in wc and wc["A"].get("total_s"):
        base = wc["A"]["total_s"]
        for arm in arms_present:
            if wc[arm].get("total_s"):
                speed[f"{arm}_over_A_walltime_ratio"] = round(wc[arm]["total_s"] / base, 3)
    if "B" in wc and wc["B"].get("total_s") and "C" in wc and wc["C"].get("total_s"):
        speed["C_over_B_walltime_ratio"] = round(wc["C"]["total_s"] / wc["B"]["total_s"], 3)
        speed["parallel_speedup_vs_serial"] = round(wc["B"]["total_s"] / wc["C"]["total_s"], 2)
    report["speed"] = speed
    return report


def compaction_lever(records: list[dict[str, Any]], arm: str = "C") -> dict[str, Any]:
    """Marginal effect of the S4 tool-result compaction: paired supervisor-input
    delta (compaction OFF vs ON) on the same instances of one arm."""
    on = _index(records, compaction=True)
    off = _index(records, compaction=False)
    instances = sorted(
        {i for (i, a) in on if a == arm} & {i for (i, a) in off if a == arm}
    )
    sum_on_in = sum(int(on[(i, arm)]["supervisor_input_tokens"]) for i in instances)
    sum_off_in = sum(int(off[(i, arm)]["supervisor_input_tokens"]) for i in instances)
    return {
        "arm": arm,
        "n_instances": len(instances),
        "supervisor_input_tokens_compaction_off": sum_off_in,
        "supervisor_input_tokens_compaction_on": sum_on_in,
        # Positive = compaction reduced supervisor input tokens.
        "compaction_input_reduction_pct": pct_reduction(sum_off_in, sum_on_in),
    }


def _fmt_pct(v: float | None) -> str:
    return "n/a" if v is None else f"{v:+.1f}%".replace("+-", "-")


def render_markdown(report: dict[str, Any], reddit_baseline: dict[str, Any] | None = None) -> str:
    """Render the Reddit-style comparison table + aggregates as markdown."""
    lines: list[str] = []
    comp = "on" if report["compaction"] else "off"
    lines.append(f"## Supervised SWE-bench — supervisor-token reduction (compaction {comp})\n")

    headline = report["comparisons"].get("C_vs_A")
    if headline:
        lines.append(
            f"**Headline (arm C — {report['arm_labels'].get('C','')} — vs arm A baseline), "
            f"{headline['n_instances']} paired instances:**\n"
        )
        lines.append(
            f"- Supervisor tokens: baseline **{headline['baseline_supervisor_tokens']:,}** → "
            f"delegated **{headline['treatment_supervisor_tokens']:,}**  "
            f"(**{_fmt_pct(headline['micro_supervisor_total_reduction_pct'])} reduction**, sum-based)\n"
            f"  - input {_fmt_pct(headline['micro_supervisor_input_reduction_pct'])} · "
            f"output {_fmt_pct(headline['micro_supervisor_output_reduction_pct'])}\n"
            f"- Total-LLM tokens (supervisor + free workers): "
            f"{headline['baseline_total_llm_tokens']:,} → {headline['treatment_total_llm_tokens']:,} "
            f"({_fmt_pct(headline['total_llm_reduction_pct'])}) — the compute MOVED onto $0 workers, "
            f"it did not vanish\n"
        )

    for key in ("B_vs_A", "C_vs_A"):
        cmp = report["comparisons"].get(key)
        if not cmp:
            continue
        lines.append(f"\n### {cmp['treatment_arm']} vs {cmp['baseline_arm']} — per-instance\n")
        lines.append("| Instance | Supervisor input Δ | Supervisor output Δ |")
        lines.append("|---|---|---|")
        for row in cmp["rows"]:
            lines.append(
                f"| {row['instance_id']} | {_fmt_pct(row['supervisor_input_reduction_pct'])} "
                f"| {_fmt_pct(row['supervisor_output_reduction_pct'])} |"
            )
        lines.append(
            f"| **aggregate (sum-based)** | **{_fmt_pct(cmp['micro_supervisor_input_reduction_pct'])}** "
            f"| **{_fmt_pct(cmp['micro_supervisor_output_reduction_pct'])}** |"
        )
        lines.append(
            f"| _macro (mean of ratios)_ | _{_fmt_pct(cmp['macro_supervisor_input_reduction_pct'])}_ "
            f"| _{_fmt_pct(cmp['macro_supervisor_output_reduction_pct'])}_ |"
        )

    # Speed
    wc = report["wall_clock"]
    lines.append("\n### Wall-clock (the axis Reddit lost — parallelism should win it back)\n")
    lines.append("| Arm | total s | median s | p95 s |")
    lines.append("|---|---|---|---|")
    for arm in report["arms_present"]:
        w = wc.get(arm, {})
        if w.get("n"):
            lines.append(
                f"| {arm} ({report['arm_labels'].get(arm,'')}) | {w.get('total_s','?')} "
                f"| {w.get('median_s','?')} | {w.get('p95_s','?')} |"
            )
    sp = report.get("speed", {})
    if sp:
        bits = []
        if "C_over_A_walltime_ratio" in sp:
            bits.append(f"C/A wall-time = **{sp['C_over_A_walltime_ratio']}×**")
        if "parallel_speedup_vs_serial" in sp:
            bits.append(f"parallel vs serial (B/C) = **{sp['parallel_speedup_vs_serial']}× faster**")
        if bits:
            lines.append("\n" + " · ".join(bits) + "\n")

    # Resolution parity
    lines.append("\n### Resolution parity (official SWE-bench grader — must hold C ≥ A)\n")
    lines.append("| Arm | resolved / graded | rate |")
    lines.append("|---|---|---|")
    for arm in report["arms_present"]:
        r = report["resolution"].get(arm, {})
        if r.get("graded"):
            lines.append(f"| {arm} | {r['resolved']} / {r['graded']} | {r.get('resolve_rate')} |")

    if reddit_baseline:
        lines.append("\n### vs the Reddit result\n")
        lines.append(
            f"- Reddit: −75.5% supervisor tokens, **3.75× slower**. "
            f"Ours: {_fmt_pct(headline['micro_supervisor_total_reduction_pct']) if headline else 'n/a'} "
            f"supervisor tokens at "
            f"{report.get('speed', {}).get('C_over_A_walltime_ratio', '?')}× wall-time vs solo.\n"
        )
    return "\n".join(lines) + "\n"


# ============================================================================
# Live supervised-run summary (arm A vs arm C, primary-token focus).
# Consumes the result dict produced by bench_swebench_supervised.py.
#
# resolved semantics per record: None = not graded (no patch produced, or grader
# unavailable); False = a patch was submitted but did not resolve; True = resolved.
# We report resolution against TWO denominators so worker failures cannot silently
# flatter the result (C5/M8):
#   resolved / submitted  - resolution given a patch was produced (skill)
#   resolved / instances  - end-to-end incl. worker failures (the honest public number)
# ============================================================================


def _arm_stats(arm: dict[str, Any], ptok: dict[str, Any] | None, n: int) -> dict[str, Any]:
    recs = arm.get("records", {})
    submitted = [r for r in recs.values() if r.get("diff")]
    graded = [r for r in submitted if r.get("resolved") is not None]
    resolved = sum(1 for r in graded if r.get("resolved"))
    cand = [r.get("n_candidates") for r in recs.values() if "n_candidates" in r]
    need = math.ceil(n / 2) if n else 0
    under = [c for c in cand if c is not None and c < need]
    return {
        "instances": len(recs),
        "submitted": len(submitted),
        "graded": len(graded),
        "resolved": resolved,
        "resolve_rate_submitted": round(resolved / len(graded), 4) if graded else None,
        "resolve_rate_instances": round(resolved / len(recs), 4) if recs else None,
        "wall_total_s": arm.get("wall_total"),
        "primary_tokens": (ptok or {}).get("total"),
        "primary_input": (ptok or {}).get("input"),
        "primary_output": (ptok or {}).get("output"),
        # Candidate-set health for arm C: a flaky fleet that returns <ceil(N/2)
        # candidates shrinks the selection prompt and would otherwise inflate the
        # reduction; surface it so it can't hide.
        "candidates_total": len(cand),
        "candidates_underpopulated": len(under),
        "candidates_min_required": need,
    }


def summarize_arms(result: dict[str, Any]) -> dict[str, Any]:
    arms = result.get("arms", {})
    ptok = result.get("primary_tokens", {})
    n = result.get("n", 0)
    out: dict[str, Any] = {"arms": {a: _arm_stats(arms[a], ptok.get(a), n) for a in arms}}
    a = out["arms"].get("A", {})
    c = out["arms"].get("C", {})
    if a.get("primary_tokens") and c.get("primary_tokens") is not None:
        out["primary_token_reduction_pct"] = pct_reduction(a["primary_tokens"], c["primary_tokens"])
    if a.get("wall_total_s") and c.get("wall_total_s"):
        out["walltime_ratio_C_over_A"] = round(c["wall_total_s"] / a["wall_total_s"], 3)
    if c.get("candidates_underpopulated"):
        out["warning"] = (f"{c['candidates_underpopulated']}/{c['candidates_total']} arm-C instances had "
                          f"< {c['candidates_min_required']} candidates; primary-token reduction is "
                          f"optimistic for those (fewer candidates -> shorter selection prompt).")
    return out


def render_supervised(result: dict[str, Any]) -> str:
    s = summarize_arms(result)
    L = ["## Supervised SWE-bench (single-shot) — primary(manager) tokens vs solo\n",
         f"instances: {len(result.get('instances', []))}  primary: {result.get('primary')}  "
         f"n(best-of): {result.get('n')}  pool: {', '.join(result.get('pool', []))}\n",
         "| Arm | primary tokens | wall (s) | resolved/submitted | resolved/instances |",
         "|---|---|---|---|---|"]
    labels = {"A": "A primary_alone", "C": "C supervised (best-of-N)"}
    for a in ("A", "C"):
        st = s["arms"].get(a)
        if not st:
            continue
        pt = st["primary_tokens"]
        L.append(f"| {labels.get(a, a)} | {pt if pt is not None else 'n/a'} | {st['wall_total_s']} | "
                 f"{st['resolved']}/{st['submitted']} | {st['resolved']}/{st['instances']} |")
    if s.get("primary_token_reduction_pct") is not None:
        L.append(f"\n**Primary-agent token reduction (A -> C): "
                 f"{s['primary_token_reduction_pct']:+.1f}%**")
    if s.get("walltime_ratio_C_over_A") is not None:
        r = s["walltime_ratio_C_over_A"]
        L.append(f"Wall-clock C/A: **{r}x** " + ("(faster)" if r < 1 else "(slower)"))
    if s.get("warning"):
        L.append(f"\n> ⚠ {s['warning']}")
    return "\n".join(L) + "\n"
