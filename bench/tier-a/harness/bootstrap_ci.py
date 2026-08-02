"""How big does a difference have to be on this gold set before it means anything?

Every comparison in this benchmark has been reported as a bare F1 delta, and
several of them are two triples wide. With 67 gold triples over 69 scored notes,
one triple is worth roughly 0.01 F1, so a 0.02 difference is two facts and a
0.03 difference is three. That is not obviously distinguishable from which notes
happened to get written.

This computes the actual uncertainty two ways:

  single run   - resample the 69 notes with replacement, recompute F1. Gives the
                 CI on one model's score.
  paired delta - resample the SAME note indices for both runs and take the
                 difference. This is the right test for "is A better than B",
                 and it is much tighter than comparing two separate CIs, because
                 note difficulty cancels: a hard note that hurts A hurts B in the
                 same resample. Overlapping single-run CIs do NOT imply the
                 difference is insignificant, which is the usual way this gets
                 read wrong.

Scoring is score.py's own matching, imported rather than reimplemented, so this
cannot drift from what the scorer says.
"""
import argparse
import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prompt  # noqa: E402
import score as S  # noqa: E402


def per_note_counts(gold_path, pred_path, lenient=False):
    """Return [(id, tp, fp, fn), ...] using the scorer's own logic."""
    gold_rows = [json.loads(l) for l in open(gold_path) if l.strip()]
    pred_rows = [json.loads(l) for l in open(pred_path) if l.strip()]

    # Honour the same exclusion the scorer applies, or the denominators differ.
    excluded = {r["id"] for r in gold_rows if r.get("excluded")}
    gold_rows = [r for r in gold_rows if r["id"] not in excluded]
    pred_rows = [r for r in pred_rows if r["id"] not in excluded]

    gnote = {r["id"]: S.ground_text(r["note"]) for r in gold_rows}
    for r in pred_rows:
        nn = gnote[r["id"]]
        r["pred_grounded"] = [
            t for t in (r.get("pred_nofloor") or [])
            if S.grounded(t.get("subject"), nn) and S.grounded(t.get("object"), nn)
        ]

    S.USE_ALT = True
    S.SYMMETRIC = prompt.symmetric_relations()
    S.INVERSES = prompt.inverse_relations()

    gold = S.load_triples(gold_rows, "gold")
    pred = S.load_triples(pred_rows, "pred_grounded", canonicalize=True)

    out = []
    for r in gold_rows:
        nid = r["id"]
        g, p = gold.get(nid, []), pred.get(nid, [])
        tp, used = S.match_note(p, g, lenient)
        out.append((nid, tp, len(p) - len(used), len(g) - tp))
    return out


def f1(tp, fp, fn):
    if tp == 0:
        return 0.0
    prec, rec = tp / (tp + fp), tp / (tp + fn)
    return 2 * prec * rec / (prec + rec)


def boot_single(counts, b, rng):
    n = len(counts)
    vals = []
    for _ in range(b):
        idx = [rng.randrange(n) for _ in range(n)]
        tp = sum(counts[i][1] for i in idx)
        fp = sum(counts[i][2] for i in idx)
        fn = sum(counts[i][3] for i in idx)
        vals.append(f1(tp, fp, fn))
    vals.sort()
    return vals[int(.025 * b)], vals[int(.975 * b)]


def boot_paired(ca, cb, b, rng):
    """CI on F1(a) - F1(b), resampling the same notes for both."""
    by_a = {c[0]: c for c in ca}
    by_b = {c[0]: c for c in cb}
    ids = [i for i in by_a if i in by_b]
    n = len(ids)
    vals = []
    for _ in range(b):
        idx = [rng.randrange(n) for _ in range(n)]
        ta = fa = na = tb = fb = nb = 0
        for i in idx:
            k = ids[i]
            ta += by_a[k][1]; fa += by_a[k][2]; na += by_a[k][3]
            tb += by_b[k][1]; fb += by_b[k][2]; nb += by_b[k][3]
        vals.append(f1(ta, fa, na) - f1(tb, fb, nb))
    vals.sort()
    lo, hi = vals[int(.025 * b)], vals[int(.975 * b)]
    crosses = lo <= 0 <= hi
    return lo, hi, crosses, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gold", required=True)
    ap.add_argument("--pred", action="append", required=True,
                    help="label=path; repeat. First is the reference for deltas.")
    ap.add_argument("--boot", type=int, default=5000)
    ap.add_argument("--seed", type=int, default=20260801,
                    help="fixed so the interval is reproducible; a CI that moves "
                         "between runs of the analysis is not a CI.")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    runs = []
    for spec in args.pred:
        label, _, path = spec.partition("=")
        counts = per_note_counts(args.gold, path)
        tp = sum(c[1] for c in counts)
        fp = sum(c[2] for c in counts)
        fn = sum(c[3] for c in counts)
        lo, hi = boot_single(counts, args.boot, rng)
        runs.append((label, counts, f1(tp, fp, fn), lo, hi, tp, fp, fn))

    print(f"{'run':30} {'F1':>7}  {'95% CI':>16}  {'tp':>3} {'fp':>3} {'fn':>3}")
    for label, _, v, lo, hi, tp, fp, fn in runs:
        print(f"{label:30} {v:7.4f}  [{lo:.4f},{hi:.4f}]  {tp:3d} {fp:3d} {fn:3d}")

    if len(runs) > 1:
        print(f"\nPaired deltas vs {runs[0][0]} (same notes resampled for both):")
        print(f"{'comparison':40} {'delta':>8}  {'95% CI':>18}  verdict")
        for label, counts, v, *_ in runs[1:]:
            lo, hi, crosses, n = boot_paired(counts, runs[0][1], args.boot, rng)
            verdict = "INDISTINGUISHABLE" if crosses else "significant"
            print(f"{label + ' - ' + runs[0][0]:40} {v - runs[0][2]:8.4f}  "
                  f"[{lo:+.4f},{hi:+.4f}]  {verdict}")
        print(f"\nn = {n} notes resampled, {args.boot} bootstrap replicates.")


if __name__ == "__main__":
    main()
