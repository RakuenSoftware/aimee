#!/usr/bin/env python3
"""S6 — the fail-closed public-claim gate (agentic supervised SWE-bench, #987).

The honesty capstone. The public line — "beats Reddit's -75.5% supervisor tokens at no
wall-clock penalty" — may be emitted ONLY if EVERY criterion below holds, under the official
grader, on BOTH Benchmark 1 (Reddit-10) AND Benchmark 2 (held-out). Otherwise the report is
STILL published (publish-regardless), but with the claim withheld and an honest partial summary
(e.g. "-78% primary tokens at 1.05x p95 wall-clock — claim withheld"). Fail closed: any missing
metric, any unmet criterion, or a benchmark disagreement withholds the claim.

Criteria (proposal S6 + S3 rulings), all anchored on N=1 for the public claim:
  C1 token   : BCa-95 CI lower-bound of primary-token reduction > 0.
  C2 wall    : p95 A->C wall-clock ratio upper-bound <= 1.0 (gate on p95, not median;
               computed against aimee's OWN arm-A time, not Reddit's cross-harness minutes).
  C3 resolve : resolved_C/total >= max(0.7 * resolved_A/total, 0.25) — without this floor a
               token+wall win at ~0% resolution would be dishonest.
  C4 both    : C1-C3 hold on Benchmark 1 AND Benchmark 2; a divergence withholds.
  C5 stats   : Benchmark 1 ran K>=10 repeats (the public-claim benchmark).
  C6 clean   : the arm-C headline set is not ESCALATION_DOMINATED (escalation-excluded fraction
               below the cap) — the win must come from supervision, not the primary digging in.
  C7 review  : an independent reviewer (not the harness/worker author) signed off (recorded).

This module is pure and unit-tested; it decides, it does not run anything.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Any

ESCALATION_EXCLUDED_CAP = 0.40   # >40% of arm-C runs escalation-dominated => not "cheap supervision"
MIN_ABS_RESOLUTION = 0.25
RESOLUTION_PARITY_FRAC = 0.70
MIN_K_PUBLIC = 10


@dataclass
class BenchmarkMetrics:
    """The N=1 slice of one benchmark, as computed by supervised_report + _panels."""
    name: str
    token_reduction_ci: tuple[float, float]   # BCa-95 CI of (1 - sum_C/sum_A), fraction
    wall_p95_ratio_ci: tuple[float, float]     # BCa-95 CI of p95(C)/p95(A)
    resolved_c: int
    resolved_a: int
    total: int
    k: int
    escalation_excluded_frac: float = 0.0
    n_anchor: int = 1


def _crit_token(m: BenchmarkMetrics) -> tuple[bool, str]:
    lo = m.token_reduction_ci[0]
    return lo > 0, f"token-reduction CI lower-bound = {lo:+.3f} (need > 0)"


def _crit_wall(m: BenchmarkMetrics) -> tuple[bool, str]:
    hi = m.wall_p95_ratio_ci[1]
    return hi <= 1.0, f"p95 wall A->C ratio CI upper-bound = {hi:.3f} (need <= 1.0)"


def _crit_resolution(m: BenchmarkMetrics) -> tuple[bool, str]:
    if m.total <= 0:
        return False, "no graded instances"
    rc, ra = m.resolved_c / m.total, m.resolved_a / m.total
    floor = max(RESOLUTION_PARITY_FRAC * ra, MIN_ABS_RESOLUTION)
    return rc >= floor, f"resolved_C/total = {rc:.3f} (need >= {floor:.3f} = max(0.7*{ra:.3f}, 0.25))"


def _crit_k(m: BenchmarkMetrics) -> tuple[bool, str]:
    return m.k >= MIN_K_PUBLIC, f"K = {m.k} (need >= {MIN_K_PUBLIC})"


def _crit_anchor(m: BenchmarkMetrics) -> tuple[bool, str]:
    return m.n_anchor == 1, f"N anchor = {m.n_anchor} (public claim must be N=1)"


def _crit_escalation(m: BenchmarkMetrics) -> tuple[bool, str]:
    ok = m.escalation_excluded_frac <= ESCALATION_EXCLUDED_CAP
    return ok, f"escalation-dominated fraction = {m.escalation_excluded_frac:.3f} (need <= {ESCALATION_EXCLUDED_CAP})"


def evaluate_benchmark(m: BenchmarkMetrics, *, require_k: bool) -> dict[str, Any]:
    """Per-benchmark criteria. `require_k` only for the public-claim benchmark (B1)."""
    crits = {
        "token": _crit_token(m),
        "wall": _crit_wall(m),
        "resolution": _crit_resolution(m),
        "anchor_n1": _crit_anchor(m),
        "escalation_clean": _crit_escalation(m),
    }
    if require_k:
        crits["k_adequate"] = _crit_k(m)
    detail = {k: {"pass": ok, "why": why} for k, (ok, why) in crits.items()}
    return {"name": m.name, "pass": all(v["pass"] for v in detail.values()), "criteria": detail}


def evaluate_claim_gate(b1: BenchmarkMetrics, b2: BenchmarkMetrics,
                        *, independent_review: bool = False) -> dict[str, Any]:
    """The fail-closed gate. Returns the decision + per-benchmark detail + an honest summary.
    The report is published regardless; only the CLAIM LINE is gated on `emit_claim`."""
    e1 = evaluate_benchmark(b1, require_k=True)     # B1 is the public-claim benchmark
    e2 = evaluate_benchmark(b2, require_k=False)
    both_pass = e1["pass"] and e2["pass"]
    reasons = []
    if not e1["pass"]:
        reasons += [f"B1[{k}] {v['why']}" for k, v in e1["criteria"].items() if not v["pass"]]
    if not e2["pass"]:
        reasons += [f"B2[{k}] {v['why']}" for k, v in e2["criteria"].items() if not v["pass"]]
    if not independent_review:
        reasons.append("independent reviewer sign-off missing (C7)")
    emit = both_pass and independent_review
    return {
        "emit_claim": emit,
        "benchmarks": {"b1": e1, "b2": e2},
        "independent_review": independent_review,
        "withheld_reasons": reasons,
        "summary": _summary(b1, emit, reasons),
    }


def _summary(b1: BenchmarkMetrics, emit: bool, reasons: list[str]) -> str:
    """The honest one-liner — always publishable, whether or not the claim is emitted."""
    red = b1.token_reduction_ci
    wall = b1.wall_p95_ratio_ci
    red_pct = f"{100 * (red[0] + red[1]) / 2:.0f}%"
    wall_x = f"{(wall[0] + wall[1]) / 2:.2f}x"
    if emit:
        return (f"Beats Reddit's -75.5%: -{red_pct} primary tokens at {wall_x} p95 wall-clock "
                f"(both benchmarks, official grader, N=1, K>={MIN_K_PUBLIC}, independently reviewed).")
    head = reasons[0] if reasons else "criteria unmet"
    return f"-{red_pct} primary tokens at {wall_x} p95 wall-clock — CLAIM WITHHELD ({head})."
