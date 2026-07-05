#!/usr/bin/env python3
"""S4 — report panels for the agentic supervised SWE-bench benchmark (#987).

The proposal + S1/S3 roundtables specified report surfaces the base `supervised_report` does not
yet carry. These are the pure, composable aggregators that produce them; they compose onto a
report dict without touching the already-tested base module. All deterministic (seeded), so a
rerun on the same data yields identical output.

Panels (each maps to a ratified ruling):
  - bca_ci ....................... BCa-95 bootstrap CI for the token-reduction headline (proposal
                                   S6 asked for BCa, not plain percentile).
  - pareto_panel ................. dominance across (arm x N x reducers) configs over
                                   (primary tokens, wall p95, resolution) — the single most
                                   informative view for the claim.
  - failure_mode_breakdown ....... per-arm counts of worker-dropout / wrong-diff / mis-selection
                                   / grader-flake / environment, so a low resolution is
                                   attributable to a cause, not left ambiguous.
  - selection_skill .............. oracle-best-of-N vs actual-best-of-N + skill ratio (S3 Q3):
                                   separates supervisor selection skill from worker diversity.
  - escalation_split ............. partitions the headline from ESCALATION_DOMINATED runs
                                   (>40% escalation tokens), which are excluded from the claim.
  - context_size_distribution .... p50/p95/max of the supervisor context + the per-turn token
                                   curve — context drift is where dishonest savings hide (S3 Q4).
  - wall_two_clock ............... total (queue+work) vs work-only p50/p95/p99 (proposal Q5).
  - check_arm_parity ............. the S1/S3 Q6 invariants: on a trivial input arm A and arm C
                                   produce the same patch and token accounting differs in the
                                   EXPECTED direction (C primary << A total; no escalation).
"""
from __future__ import annotations

import math
import random
from typing import Any


# ------------------------------------------------------------ percentiles ------
def _pct(sorted_vals: list[float], q: float) -> float:
    if not sorted_vals:
        return 0.0
    i = min(len(sorted_vals) - 1, int(round(q * (len(sorted_vals) - 1))))
    return sorted_vals[i]


def dist(values: list[float]) -> dict[str, Any]:
    """p50/p95/p99/max/mean/n over a list (empty -> zeros)."""
    if not values:
        return {"n": 0}
    s = sorted(values)
    return {"n": len(s), "p50": round(_pct(s, 0.50), 3), "p95": round(_pct(s, 0.95), 3),
            "p99": round(_pct(s, 0.99), 3), "max": round(s[-1], 3),
            "mean": round(sum(s) / len(s), 3)}


# ------------------------------------------------------------ BCa CI -----------
def _norm_cdf(z: float) -> float:
    return 0.5 * (1.0 + math.erf(z / math.sqrt(2.0)))


def _norm_ppf(p: float) -> float:
    """Inverse normal CDF (Acklam's rational approximation). Good to ~1e-9."""
    if p <= 0.0:
        return -math.inf
    if p >= 1.0:
        return math.inf
    a = [-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
         1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00]
    b = [-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
         6.680131188771972e+01, -1.328068155288572e+01]
    c = [-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
         -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00]
    d = [7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
         3.754408661907416e+00]
    plow, phigh = 0.02425, 1 - 0.02425
    if p < plow:
        q = math.sqrt(-2 * math.log(p))
        return (((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) / ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1)
    if p > phigh:
        q = math.sqrt(-2 * math.log(1 - p))
        return -(((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) / ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1)
    q = p - 0.5
    r = q * q
    return (((((a[0]*r+a[1])*r+a[2])*r+a[3])*r+a[4])*r+a[5])*q / (((((b[0]*r+b[1])*r+b[2])*r+b[3])*r+b[4])*r+1)


def bca_ci(values: list[float], iters: int = 2000, alpha: float = 0.05,
           seed: int = 42) -> tuple[float, float]:
    """Bias-corrected accelerated (BCa) bootstrap CI for the mean. Deterministic (seeded).
    Falls back to percentile bounds when acceleration is degenerate (all values equal)."""
    if not values:
        return (0.0, 0.0)
    if len(values) == 1:
        return (values[0], values[0])
    n = len(values)
    theta = sum(values) / n
    rng = random.Random(seed)
    boots = sorted(sum(values[rng.randrange(n)] for _ in range(n)) / n for _ in range(iters))
    # bias-correction z0
    n_less = sum(1 for b in boots if b < theta)
    prop = n_less / iters
    if prop <= 0 or prop >= 1:
        return (round(boots[int((alpha / 2) * iters)], 3),
                round(boots[int((1 - alpha / 2) * iters)], 3))
    z0 = _norm_ppf(prop)
    # acceleration via jackknife
    jack = [(sum(values) - values[i]) / (n - 1) for i in range(n)]
    jbar = sum(jack) / n
    num = sum((jbar - j) ** 3 for j in jack)
    den = 6.0 * (sum((jbar - j) ** 2 for j in jack) ** 1.5)
    a = num / den if den else 0.0
    zl, zh = _norm_ppf(alpha / 2), _norm_ppf(1 - alpha / 2)

    def _adj(z):
        return _norm_cdf(z0 + (z0 + z) / (1 - a * (z0 + z)))

    lo_q, hi_q = _adj(zl), _adj(zh)
    lo = boots[min(iters - 1, max(0, int(lo_q * iters)))]
    hi = boots[min(iters - 1, max(0, int(hi_q * iters)))]
    return (round(lo, 3), round(hi, 3))


# ------------------------------------------------------------ pareto -----------
def pareto_panel(configs: list[dict[str, Any]]) -> dict[str, Any]:
    """configs: dicts with keys {name, primary_tokens, wall_p95, resolve_rate}. Marks each as
    Pareto-optimal or dominated (minimize tokens, minimize wall, MAXIMIZE resolve). Returns the
    annotated list + the optimal frontier names."""
    def dominates(x, y):
        better_or_eq = (x["primary_tokens"] <= y["primary_tokens"] and
                        x["wall_p95"] <= y["wall_p95"] and
                        x["resolve_rate"] >= y["resolve_rate"])
        strictly = (x["primary_tokens"] < y["primary_tokens"] or
                    x["wall_p95"] < y["wall_p95"] or
                    x["resolve_rate"] > y["resolve_rate"])
        return better_or_eq and strictly

    annotated = []
    for c in configs:
        dominated_by = [o["name"] for o in configs if o is not c and dominates(o, c)]
        annotated.append({**c, "pareto_optimal": not dominated_by,
                          "dominated_by": dominated_by})
    frontier = [c["name"] for c in annotated if c["pareto_optimal"]]
    return {"configs": annotated, "frontier": frontier}


# ------------------------------------------------------------ failure modes ----
FAILURE_MODES = ("worker_dropout", "wrong_diff", "mis_selection", "grader_flake",
                 "environment", "ok")


def failure_mode_breakdown(records: list[dict[str, Any]]) -> dict[str, dict[str, int]]:
    """Per-arm counts of each failure mode (records carry an optional 'failure_mode' field;
    a resolved record with no mode counts as 'ok')."""
    out: dict[str, dict[str, int]] = {}
    for r in records:
        if r.get("invalid"):
            continue
        arm = r.get("arm", "?")
        mode = r.get("failure_mode") or ("ok" if r.get("resolved") else "wrong_diff")
        out.setdefault(arm, {m: 0 for m in FAILURE_MODES})
        out[arm][mode] = out[arm].get(mode, 0) + 1
    return out


# ------------------------------------------------------------ selection skill --
def selection_skill(records: list[dict[str, Any]]) -> dict[str, Any]:
    """Oracle-best-of-N vs actual-best-of-N over arm-C records that carry the decomposition.
    A record supplies 'oracle_resolved' (any candidate passes the grader) and 'actual_resolved'
    (the SELECTED patch passes). skill_ratio = actual_rate / oracle_rate."""
    rel = [r for r in records if r.get("arm") == "C" and not r.get("invalid")
           and "oracle_resolved" in r and "actual_resolved" in r]
    n = len(rel)
    if not n:
        return {"n": 0}
    oracle = sum(1 for r in rel if r["oracle_resolved"])
    actual = sum(1 for r in rel if r["actual_resolved"])
    orate, arate = oracle / n, actual / n
    return {"n": n, "oracle_pass_rate": round(orate, 4), "actual_pass_rate": round(arate, 4),
            "selection_skill_ratio": round(arate / orate, 4) if orate else None,
            "recoverable_gap": oracle - actual}


# ------------------------------------------------------------ escalation split -
def escalation_split(records: list[dict[str, Any]]) -> dict[str, Any]:
    """Partition arm-C records into the headline set vs ESCALATION_DOMINATED (>40% escalation
    tokens). The headline claim is computed over the non-dominated set only."""
    c = [r for r in records if r.get("arm") == "C" and not r.get("invalid")]
    dominated = [r["instance_id"] for r in c if r.get("escalation_dominated")]
    headline = [r["instance_id"] for r in c if not r.get("escalation_dominated")]
    return {"headline_n": len(headline), "escalation_dominated_n": len(dominated),
            "escalation_dominated": dominated,
            "excluded_frac": round(len(dominated) / len(c), 4) if c else 0.0}


# ------------------------------------------------------------ context drift ----
def context_size_distribution(records: list[dict[str, Any]]) -> dict[str, Any]:
    """Distribution of the supervisor's peak context size across instances + the aggregate
    per-turn token curve (records carry 'primary_context_tokens' and 'primary_tokens_by_turn')."""
    peaks = [r["primary_context_tokens"] for r in records
             if r.get("arm") == "C" and r.get("primary_context_tokens") is not None]
    curves = [r["primary_tokens_by_turn"] for r in records
              if r.get("arm") == "C" and r.get("primary_tokens_by_turn")]
    max_turns = max((len(c) for c in curves), default=0)
    by_turn = []
    for t in range(max_turns):
        vals = [c[t] for c in curves if t < len(c)]
        by_turn.append(round(sum(vals) / len(vals), 1) if vals else 0.0)
    return {"peak_context": dist(peaks), "mean_tokens_by_turn": by_turn}


# ------------------------------------------------------------ two wall clocks --
def wall_two_clock(records: list[dict[str, Any]], arm: str) -> dict[str, Any]:
    """Total (queue+work) vs work-only wall-clock distributions for an arm (proposal Q5).
    The claim gate reads the TOTAL p95; work-only is reported alongside for honesty."""
    rel = [r for r in records if r.get("arm") == arm and not r.get("invalid")]
    total = [float(r["wall_s"]) for r in rel if r.get("wall_s") is not None]
    work = [float(r["wall_work_s"]) for r in rel if r.get("wall_work_s") is not None]
    return {"arm": arm, "total": dist(total), "work": dist(work)}


# ------------------------------------------------------------ arm parity -------
def check_arm_parity(rec_a: dict[str, Any], rec_c: dict[str, Any],
                     *, token_ratio_floor: int = 10) -> dict[str, Any]:
    """S1/S3 Q6: on a trivial deterministic input, assert arm A and arm C are consistent.
    Returns each invariant's pass/fail so a CI test can assert them all. A transport difference
    must never masquerade as an algorithmic one."""
    a_total = int(rec_a.get("supervisor_input_tokens", 0)) + int(rec_a.get("supervisor_output_tokens", 0)) \
        + int(rec_a.get("worker_input_tokens", 0)) + int(rec_a.get("worker_output_tokens", 0))
    c_primary = int(rec_c.get("supervisor_input_tokens", 0)) + int(rec_c.get("supervisor_output_tokens", 0))
    same_patch = rec_a.get("patch_fingerprint") == rec_c.get("patch_fingerprint")
    same_resolution = rec_a.get("resolved") == rec_c.get("resolved")
    # C's primary spend should be an order of magnitude below A's total on a trivial task.
    cheap = c_primary * token_ratio_floor <= a_total if a_total else c_primary == 0
    no_escalation = int(rec_c.get("escalations", 0)) == 0
    inv = {"same_patch": same_patch, "same_resolution": same_resolution,
           "c_primary_much_cheaper": cheap, "no_escalation_on_trivial": no_escalation}
    inv["all_pass"] = all(inv.values())
    return inv


# ------------------------------------------------------------ compose ----------
def augment_report(report: dict[str, Any], records: list[dict[str, Any]]) -> dict[str, Any]:
    """Attach the S4 panels to a base report dict (from supervised_report.build_report)."""
    report = dict(report)
    report["failure_modes"] = failure_mode_breakdown(records)
    report["selection_skill"] = selection_skill(records)
    report["escalation"] = escalation_split(records)
    report["context_drift"] = context_size_distribution(records)
    report["wall_two_clock"] = {a: wall_two_clock(records, a)
                                for a in sorted({r.get("arm") for r in records if r.get("arm")})}
    return report
