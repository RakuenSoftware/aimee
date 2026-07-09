#!/usr/bin/env python3
"""rank-fit.py: fitter sidecar for the KB-hybrid learned linear ranker.

Closes the Calibrate half of the ranking substrate: reads the joined
feature/outcome training view (materialized in SQL by the C side, §1 of
docs/proposals/pending/learning-to-rank-weight-fitting.md) as a JSON batch on
stdin, fits a pointwise linear model over the v1 feature set, and emits a
feature-keyed weights blob on stdout — exactly the shape
`kb_ranker_model_load` parses and `kb_ranker_model_write` persists.

Stdio-JSON contract, mirroring scripts/embed-minilm.py /
scripts/guardrails-semantic.py:

  input  (stdin):
    {"feature_set_version": "v1",
     "objective": "pointwise",
     "min_groups": 8,
     "rows": [{"group": "<retrieval_event_id>",
               "features": {"lex.cos":.., "dense.cos":.., "temp.recency":..,
                            "sketch.frequency_kind_scope":.., "sketch.distinct_sources_hll":..},
               "label": 0|1}, ...]}

  output (stdout), on a successful fit:
    {"status": "ok",
     "weights": {"dense.cos":.., "lex.cos":.., "temp.recency":..,
                 "sketch.frequency_kind_scope":.., "sketch.distinct_sources_hll":..},
     "fit_metrics": {"objective":"pointwise", "n_groups":N, "n_rows":M,
                     "n_positive":P, "loss":.., "iterations":..}}

  output on refusal (never ship a garbage model — keep the default):
    {"status": "refused", "reason": "<below_floor|version_mismatch|degenerate>", ...}

Discipline (§2 of the proposal):
  * refuse when feature_set_version != the version this sidecar knows how to fit;
  * refuse when the number of labelled groups is below `min_groups`
    (avoid overfitting a thin log);
  * refuse when labels are degenerate (all one class) — nothing to learn.

CPU-only and dependency-light: numpy is used when importable, otherwise a
hand-rolled pure-Python fit runs. Consistent with the CPU-first curator profile.
"""
import json
import math
import sys

# The v1 feature set, in a fixed order. These keys are the contract with
# kb_ranker_model_load (src/kb/kb_ranker.c) — the ranker looks up exactly these.
FEATURES = [
    "dense.cos",
    "lex.cos",
    "temp.recency",
    "sketch.frequency_kind_scope",
    "sketch.distinct_sources_hll",
]

# The only feature-set version this sidecar's model matches (model_kind=linear).
SUPPORTED_FEATURE_SET_VERSION = "v1"

# Fit hyperparameters for the hand-rolled logistic regression. L2 keeps a thin
# log from blowing a coefficient up; the fit is on RAW feature values (no
# standardization) so the emitted weights apply directly in score_candidate.
L2 = 1e-3
LR = 0.5
MAX_ITERS = 2000
CONVERGE_EPS = 1e-7


def _refuse(reason, **extra):
    out = {"status": "refused", "reason": reason}
    out.update(extra)
    return out


def _feature_vector(features):
    """Extract the fixed-order raw feature vector; missing keys default to 0.0."""
    return [float(features.get(k, 0.0) or 0.0) for k in FEATURES]


def _fit_numpy(X, y):
    import numpy as np

    Xm = np.asarray(X, dtype=float)
    ym = np.asarray(y, dtype=float)
    n, d = Xm.shape
    w = np.zeros(d)
    b = 0.0
    loss = 0.0
    it = 0
    prev = float("inf")
    for it in range(1, MAX_ITERS + 1):
        z = Xm @ w + b
        p = 1.0 / (1.0 + np.exp(-z))
        err = p - ym
        grad_w = (Xm.T @ err) / n + L2 * w
        grad_b = float(err.mean())
        w -= LR * grad_w
        b -= LR * grad_b
        eps = 1e-12
        loss = float(-(ym * np.log(p + eps) + (1 - ym) * np.log(1 - p + eps)).mean()
                     + 0.5 * L2 * float(w @ w))
        if abs(prev - loss) < CONVERGE_EPS:
            break
        prev = loss
    return list(w), loss, it


def _fit_pure(X, y):
    """Pure-Python batch-gradient logistic regression (no numpy)."""
    n = len(X)
    d = len(FEATURES)
    w = [0.0] * d
    b = 0.0
    loss = 0.0
    it = 0
    prev = float("inf")
    for it in range(1, MAX_ITERS + 1):
        grad_w = [0.0] * d
        grad_b = 0.0
        loss_acc = 0.0
        for xi, yi in zip(X, y):
            z = b + sum(w[j] * xi[j] for j in range(d))
            # numerically stable sigmoid
            if z >= 0:
                p = 1.0 / (1.0 + math.exp(-z))
            else:
                ez = math.exp(z)
                p = ez / (1.0 + ez)
            err = p - yi
            for j in range(d):
                grad_w[j] += err * xi[j]
            grad_b += err
            eps = 1e-12
            loss_acc += -(yi * math.log(p + eps) + (1 - yi) * math.log(1 - p + eps))
        for j in range(d):
            grad_w[j] = grad_w[j] / n + L2 * w[j]
            w[j] -= LR * grad_w[j]
        b -= LR * (grad_b / n)
        reg = 0.5 * L2 * sum(wj * wj for wj in w)
        loss = loss_acc / n + reg
        if abs(prev - loss) < CONVERGE_EPS:
            break
        prev = loss
    return w, loss, it


def _fit_pairwise(groups):
    """RankNet-style pairwise logistic on within-query (positive > negative)
    pairs. Optimises ORDERING rather than absolute labels, so:
      * features that are constant within a query get ~0 weight (they can't
        discriminate) — which is exactly why turn-level all-`accepted` labels
        carry no pointwise signal but the per-doc contrast here does;
      * it is robust to the heavily positive-skewed live label distribution.
    Each pair's weight is the product of the two rows' weights, so an IPW /
    propensity weight on the outcomes flows straight through. Returns
    (w, n_pairs, usable_groups), or None when no query has both classes.
    `groups`: dict group -> list of (feature_vec, label01, weight)."""
    pairs = []  # (x_pos, x_neg, weight)
    usable = 0
    for items in groups.values():
        pos = [it for it in items if it[1] == 1]
        neg = [it for it in items if it[1] == 0]
        if pos and neg:
            usable += 1
        for p in pos:
            for ng in neg:
                pairs.append((p[0], ng[0], p[2] * ng[2]))
    if not pairs:
        return None

    d = len(FEATURES)
    w = [0.0] * d
    npair = len(pairs)
    for _ in range(MAX_ITERS):
        grad = [L2 * w[j] for j in range(d)]
        for xp, xn, wt in pairs:
            diff = sum(w[j] * (xp[j] - xn[j]) for j in range(d))
            # dL/d(diff) for log(1+exp(-diff)) is -sigmoid(-diff); stable form.
            s = 1.0 / (1.0 + math.exp(diff)) if diff > -30.0 else 1.0
            for j in range(d):
                grad[j] += (-s * (xp[j] - xn[j]) * wt) / npair
        for j in range(d):
            w[j] -= LR * grad[j]
    return w, npair, usable


def fit(req):
    fsv = req.get("feature_set_version", "")
    if fsv != SUPPORTED_FEATURE_SET_VERSION:
        return _refuse("version_mismatch", batch_feature_set_version=fsv,
                       supported=SUPPORTED_FEATURE_SET_VERSION)

    objective = req.get("objective", "pointwise")
    if objective not in ("pointwise", "pairwise"):
        return _refuse("unsupported_objective", objective=objective)

    rows = req.get("rows", [])
    if not isinstance(rows, list):
        return _refuse("bad_request", detail="rows must be a list")

    min_groups = int(req.get("min_groups", 8))

    # Parse once into groups of (feature_vec, label, weight). `weight` carries an
    # optional IPW/propensity or confidence weight (default 1.0).
    groups = {}
    n_positive = 0
    for r in rows:
        g = str(r.get("group", ""))
        label = 1 if float(r.get("label", 0) or 0) > 0.5 else 0
        weight = float(r.get("weight", 1.0) or 1.0)
        if weight <= 0.0:
            weight = 1.0
        groups.setdefault(g, []).append((_feature_vector(r.get("features") or {}), label, weight))
        n_positive += label

    n_groups = len(groups)
    n_rows = sum(len(v) for v in groups.values())

    if n_groups < min_groups:
        return _refuse("below_floor", n_groups=n_groups, min_groups=min_groups,
                       n_rows=n_rows)

    if objective == "pairwise":
        res = _fit_pairwise(groups)
        if res is None:
            # No query has both a used and an unused candidate — no ordering to learn.
            return _refuse("insufficient_pairs", n_groups=n_groups, n_rows=n_rows,
                           n_positive=n_positive)
        w, n_pairs, usable = res
        weights = {FEATURES[j]: round(float(w[j]), 6) for j in range(len(FEATURES))}
        return {
            "status": "ok",
            "weights": weights,
            "fit_metrics": {
                "objective": "pairwise",
                "feature_set_version": fsv,
                "n_groups": n_groups,
                "n_rows": n_rows,
                "n_positive": n_positive,
                "n_pairs": n_pairs,
                "usable_groups": usable,
            },
        }

    # ---- pointwise ----
    # Degenerate label distribution — a single class carries no pointwise signal.
    if n_positive == 0 or n_positive == n_rows:
        return _refuse("degenerate", n_positive=n_positive, n_rows=n_rows,
                       n_groups=n_groups)

    X = [x for items in groups.values() for (x, _, _) in items]
    y = [lab for items in groups.values() for (_, lab, _) in items]
    try:
        w, loss, iters = _fit_numpy(X, y)
    except Exception:
        w, loss, iters = _fit_pure(X, y)

    weights = {FEATURES[j]: round(float(w[j]), 6) for j in range(len(FEATURES))}
    return {
        "status": "ok",
        "weights": weights,
        "fit_metrics": {
            "objective": objective,
            "feature_set_version": fsv,
            "n_groups": n_groups,
            "n_rows": n_rows,
            "n_positive": n_positive,
            "loss": round(float(loss), 6),
            "iterations": int(iters),
        },
    }


def main():
    raw = sys.stdin.read()
    try:
        req = json.loads(raw) if raw.strip() else {}
    except json.JSONDecodeError as e:
        json.dump({"status": "error", "error": "invalid JSON: %s" % e}, sys.stdout)
        return 0
    try:
        out = fit(req)
    except Exception as e:  # never crash the caller — refuse loudly instead
        out = {"status": "error", "error": str(e)}
    json.dump(out, sys.stdout)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
