#!/usr/bin/env python3
"""bandit_replay.py: Offline counterfactual replay using IPW and synthetic control.

Usage:
  python3 tools/bandit_replay.py --decisions path/to/decisions.json \
      --arm arm_static_alpha --estimator ipw

  python3 tools/bandit_replay.py --decisions path/to/decisions.json \
      --bump-before 2026-01-01T00:00:00 --bump-after 2026-02-01T00:00:00 \
      --estimator synthetic_control

Reads closed decisions (from kb_store_bandit_decisions_export output or a JSON file) and
computes counterfactual reward estimates.

See docs/proposals/accepted/contextual-bandits-and-counterfactual-replay.md
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from typing import Any


# ---- IPW estimator ----

def _ipw_estimate(
    decisions: list[dict],
    target_arm: str,
    weight_cap: float = 10.0,
) -> dict:
    """Inverse-Propensity-Weighted reward estimate for target_arm.

    V_IPW(a*) = (1/T) sum_t  1[a_t == a*] * r_t / pi(a_t | x_t)

    Weights are capped at weight_cap to control variance.
    Returns estimate, variance, n_used, and a confidence band.
    """
    weighted_rewards: list[float] = []
    n_matched = 0
    n_total = len(decisions)

    for d in decisions:
        arm = d.get("arm_id", "")
        reward_raw = d.get("reward")
        propensity = d.get("propensity", 1.0)

        if reward_raw is None:
            continue
        try:
            reward = float(reward_raw)
        except (TypeError, ValueError):
            continue

        if arm != target_arm:
            continue

        n_matched += 1
        propensity = max(float(propensity), 1e-6)
        weight = min(1.0 / propensity, weight_cap)
        weighted_rewards.append(weight * reward)

    if not weighted_rewards:
        return {
            "estimator": "ipw",
            "target_arm": target_arm,
            "status": "insufficient_data",
            "n_matched": n_matched,
            "n_total": n_total,
        }

    T = n_total or 1
    v_hat = sum(weighted_rewards) / T
    variance = sum((w - v_hat) ** 2 for w in weighted_rewards) / max(T - 1, 1)
    se = math.sqrt(variance / max(len(weighted_rewards), 1))
    ci_half = 1.96 * se

    return {
        "estimator": "ipw",
        "target_arm": target_arm,
        "status": "ok",
        "v_hat": v_hat,
        "variance": variance,
        "ci_low": v_hat - ci_half,
        "ci_high": v_hat + ci_half,
        "n_matched": n_matched,
        "n_total": n_total,
        "weight_cap": weight_cap,
    }


# ---- Synthetic control estimator ----

def _mean(vals: list[float]) -> float:
    return sum(vals) / len(vals) if vals else 0.0


def _synthetic_control(
    decisions: list[dict],
    bump_before: str,
    bump_after: str,
    treatment_arm: str | None = None,
) -> dict:
    """Synthetic-control bump attribution.

    Compares post-bump reward against a synthetic baseline built from
    decisions in the pre-bump window (same surface, non-bumped).

    bump_before: ISO timestamp — end of pre-bump window.
    bump_after:  ISO timestamp — start of post-bump window.
    treatment_arm: if set, restrict to that arm; otherwise use all arms.
    """
    pre: list[float] = []
    post: list[float] = []

    for d in decisions:
        ts = d.get("decided_at", "")
        reward_raw = d.get("reward")
        if reward_raw is None:
            continue
        try:
            reward = float(reward_raw)
        except (TypeError, ValueError):
            continue

        if treatment_arm and d.get("arm_id") != treatment_arm:
            continue

        if ts < bump_before:
            pre.append(reward)
        elif ts >= bump_after:
            post.append(reward)

    if len(pre) < 5:
        return {
            "estimator": "synthetic_control",
            "status": "insufficient_data",
            "reason": f"pre-bump window has only {len(pre)} decisions (need >=5)",
            "bump_before": bump_before,
            "bump_after": bump_after,
        }

    if len(post) < 5:
        return {
            "estimator": "synthetic_control",
            "status": "insufficient_data",
            "reason": f"post-bump window has only {len(post)} decisions (need >=5)",
            "bump_before": bump_before,
            "bump_after": bump_after,
        }

    baseline = _mean(pre)
    post_mean = _mean(post)
    lift = post_mean - baseline
    se_pre = math.sqrt(sum((x - baseline) ** 2 for x in pre) / max(len(pre) - 1, 1))
    se_post = math.sqrt(sum((x - post_mean) ** 2 for x in post) / max(len(post) - 1, 1))
    pooled_se = math.sqrt(se_pre ** 2 / len(pre) + se_post ** 2 / len(post))
    z = lift / pooled_se if pooled_se > 0 else 0.0

    return {
        "estimator": "synthetic_control",
        "status": "ok",
        "bump_before": bump_before,
        "bump_after": bump_after,
        "treatment_arm": treatment_arm,
        "baseline_mean": baseline,
        "post_mean": post_mean,
        "lift": lift,
        "lift_z": z,
        "significant": abs(z) >= 1.96,
        "n_pre": len(pre),
        "n_post": len(post),
    }


# ---- main ----

def main() -> None:
    parser = argparse.ArgumentParser(description="Counterfactual bandit replay")
    parser.add_argument("--decisions", required=True,
                        help="Path to JSON file with closed decisions array")
    parser.add_argument("--estimator", choices=["ipw", "synthetic_control"],
                        default="ipw")
    parser.add_argument("--arm", default=None,
                        help="Target arm ID (required for IPW)")
    parser.add_argument("--weight-cap", type=float, default=10.0,
                        help="IPW weight cap (default: 10.0)")
    parser.add_argument("--bump-before", default=None,
                        help="ISO timestamp: end of pre-bump window (synthetic_control)")
    parser.add_argument("--bump-after", default=None,
                        help="ISO timestamp: start of post-bump window (synthetic_control)")
    args = parser.parse_args()

    with open(args.decisions) as fh:
        decisions = json.load(fh)

    if not isinstance(decisions, list):
        print(json.dumps({"status": "error", "message": "expected JSON array of decisions"}))
        sys.exit(1)

    if args.estimator == "ipw":
        if not args.arm:
            print(json.dumps({"status": "error", "message": "--arm required for IPW"}))
            sys.exit(1)
        result = _ipw_estimate(decisions, args.arm, args.weight_cap)

    else:
        if not args.bump_before or not args.bump_after:
            print(json.dumps({"status": "error",
                              "message": "--bump-before and --bump-after required"}))
            sys.exit(1)
        result = _synthetic_control(decisions, args.bump_before, args.bump_after, args.arm)

    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
