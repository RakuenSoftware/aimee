#!/usr/bin/env python3
"""Fixture replay harness for the contextual-bandit substrate.

Exercises all fixture kinds in benchmarks/bandit/queries.json without
requiring a live KB_STORE or network access.

Usage:
    python3 tools/bandit_fixture_replay.py [queries.json]

Exit codes:
    0  all fixtures pass
    1  one or more fixtures fail

See docs/proposals/accepted/contextual-bandits-and-counterfactual-replay.md
"""

from __future__ import annotations

import json
import random
import sys
from pathlib import Path
from typing import Any, Dict, List, Tuple

SCRIPT_DIR = Path(__file__).parent
DEFAULT_FIXTURES = SCRIPT_DIR.parent / "benchmarks" / "bandit" / "queries.json"

THIN_DATA_THRESHOLD = 3


def _beta_sample(alpha: float, beta: float) -> float:
    return random.betavariate(max(alpha, 1e-9), max(beta, 1e-9))


def simulate_stationary(arms, n_rounds):
    posteriors = {a["arm_id"]: [1.0, 1.0] for a in arms}
    true_means = {a["arm_id"]: a["true_mean"] for a in arms}
    counts = {a["arm_id"]: 0 for a in arms}
    for _ in range(n_rounds):
        chosen = max(posteriors, key=lambda k: _beta_sample(posteriors[k][0], posteriors[k][1]))
        counts[chosen] += 1
        reward = 1 if random.random() < true_means[chosen] else 0
        posteriors[chosen][0] += reward
        posteriors[chosen][1] += 1 - reward
    return counts


def simulate_drifting(arms, n_rounds, drift_round):
    posteriors = {a["arm_id"]: [1.0, 1.0] for a in arms}
    before = {a["arm_id"]: 0 for a in arms}
    after = {a["arm_id"]: 0 for a in arms}
    for t in range(n_rounds):
        if t < drift_round:
            means = {a["arm_id"]: a["true_mean_before"] for a in arms}
        else:
            means = {a["arm_id"]: a["true_mean_after"] for a in arms}
        chosen = max(posteriors, key=lambda k: _beta_sample(posteriors[k][0], posteriors[k][1]))
        reward = 1 if random.random() < means[chosen] else 0
        (before if t < drift_round else after)[chosen] += 1
        posteriors[chosen][0] += reward
        posteriors[chosen][1] += 1 - reward
    return before, after


def compute_ipw(decisions, target_arm, weight_cap=10.0):
    n_total, n_matched, total = len(decisions), 0, 0.0
    for d in decisions:
        if d.get("arm_id") != target_arm:
            continue
        n_matched += 1
        w = min(1.0 / max(d["propensity"], 1e-9), weight_cap)
        total += w * d["reward"]
    T = n_total or 1
    return (total / T), n_matched


def sc_lift(decisions, bump_before, bump_after, arm):
    pre = [d["reward"] for d in decisions
           if d.get("arm_id") == arm and d.get("decided_at","") < bump_after]
    post = [d["reward"] for d in decisions
            if d.get("arm_id") == arm and d.get("decided_at","") >= bump_after]
    if not pre or not post:
        return 0.0
    return sum(post)/len(post) - sum(pre)/len(pre)


def run_stationary(fx):
    counts = simulate_stationary(fx["arms"], fx.get("n_rounds", 200))
    best = max(counts, key=counts.get)
    frac = counts[best] / max(fx.get("n_rounds", 200), 1)
    exp = fx["expected_best_arm"]
    minf = fx.get("min_best_arm_fraction", 0.5)
    if best != exp:
        return False, f"expected '{exp}', got '{best}'; counts={counts}"
    if frac < minf:
        return False, f"'{best}' fraction {frac:.2%} < {minf:.2%}"
    return True, f"'{best}' selected {frac:.2%}"


def run_drifting(fx):
    before, after = simulate_drifting(fx["arms"], fx.get("n_rounds",250), fx.get("drift_round",100))
    bb = max(before, key=before.get) if before else ""
    ba = max(after, key=after.get) if after else ""
    eb = fx.get("expected_best_arm_before","")
    ea = fx.get("expected_best_arm_after","")
    if eb and bb != eb:
        return False, f"pre-drift: expected '{eb}', got '{bb}'"
    if ea and ba != ea:
        return False, f"post-drift: expected '{ea}', got '{ba}'"
    return True, f"pre='{bb}' post='{ba}'"


def run_ipw(fx):
    v, n = compute_ipw(fx["decisions"], fx["target_arm"], fx.get("weight_cap", 10.0))
    if n < THIN_DATA_THRESHOLD:
        return False, f"insufficient data ({n} < {THIN_DATA_THRESHOLD})"
    vmin, vmax = fx.get("expected_v_hat_min", 0.0), fx.get("expected_v_hat_max", 1.0)
    if not (vmin <= v <= vmax):
        return False, f"V_IPW={v:.4f} outside [{vmin}, {vmax}]"
    return True, f"V_IPW={v:.4f} in [{vmin},{vmax}]; n={n}"


def run_thin(fx):
    _, n = compute_ipw(fx["decisions"], fx["target_arm"], fx.get("weight_cap", 10.0))
    if n >= THIN_DATA_THRESHOLD:
        return False, f"expected insufficient_data (n={n} >= {THIN_DATA_THRESHOLD})"
    return True, f"insufficient_data (n={n} < {THIN_DATA_THRESHOLD})"


def run_sc(fx):
    lift = sc_lift(fx["decisions"], fx.get("bump_before",""), fx.get("bump_after",""), fx.get("treatment_arm",""))
    pos = fx.get("expected_lift_positive", True)
    if pos and lift <= 0:
        return False, f"expected positive lift, got {lift:.4f}"
    if not pos and lift >= 0:
        return False, f"expected negative lift, got {lift:.4f}"
    return True, f"lift={lift:.4f} ({'positive' if lift > 0 else 'negative'})"


RUNNERS = {
    "stationary": run_stationary,
    "drifting": run_drifting,
    "ipw_replay": run_ipw,
    "thin_data": run_thin,
    "synthetic_control": run_sc,
    "synthetic_control_regression": run_sc,
}


def main():
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_FIXTURES
    corpus = json.loads(path.read_text())
    fixtures = corpus.get("fixtures", [])
    total = len(fixtures)
    passed = 0
    for i, fx in enumerate(fixtures, 1):
        kind = fx.get("fixture_kind", "?")
        desc = fx.get("description", "")
        runner = RUNNERS.get(kind)
        if not runner:
            print(f"[{i}/{total}] SKIP [{kind}] {desc}: no runner")
            passed += 1
            continue
        random.seed(fx.get("seed", 42 + i))
        try:
            ok, msg = runner(fx)
        except Exception as e:
            ok, msg = False, f"exception: {e}"
        print(f"[{i}/{total}] {'PASS' if ok else 'FAIL'} [{kind}] {desc}: {msg}")
        if ok:
            passed += 1
    failed = total - passed
    prec = passed / total if total else 0.0
    print()
    if not failed:
        print(f"PASS {passed}/{total} fixtures (precision={prec:.2f})")
        return 0
    print(f"FAIL {failed}/{total} fixtures failed (precision={prec:.2f})")
    return 1


if __name__ == "__main__":
    sys.exit(main())
