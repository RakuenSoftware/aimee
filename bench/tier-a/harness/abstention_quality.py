#!/usr/bin/env python3
"""Score the third of the corpus that strict F1 structurally cannot score.

WHY THIS EXISTS. Splitting the MTP pairs by category (harness/mtp_by_category.py)
showed three categories at exactly 0.0000 strict F1 in every arm: negation
(1318), transient (1391), ambiguous (506). They are 3215 of 10000 notes, 32.1%
of gold_large, and every one of them has EMPTY gold by construction. They are
abstention tests -- the correct answer is "no facts".

Strict F1 on such a row can never be anything but zero. tp is always 0 because
there is nothing to find, so F1 = 2*0/(0+fp+0) = 0 whether the model abstained
perfectly or hallucinated on every single row. Within a category slice the two
are indistinguishable. In the aggregate the rows are not quite invisible -- their
false positives inflate the global fp and drag F1 down -- but they can never
raise it. **A third of this corpus can only ever cost a model points, and
correct restraint on it is worth exactly nothing.**

So the metric these rows actually support is not F1. It is: how often does the
model invent a fact when the right answer is silence?

Reports per arm: rows, rows with any predicted fact (the false-positive rate),
and total spurious facts. Lower is better and 0 is achievable.
"""
import json, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def empty_gold_ids(gold_path):
    ids, cats = set(), {}
    for l in open(gold_path):
        r = json.loads(l)
        g = r["gold"]
        g = json.loads(g) if isinstance(g, str) else g
        if not g:
            ids.add(r["id"])
            cats[r["id"]] = r["category"]
    return ids, cats


def measure(pred_path, ids):
    rows = [json.loads(l) for l in open(pred_path)]
    sel = [r for r in rows if r["id"] in ids]
    if not sel:
        return None
    fp_rows = sum(1 for r in sel if r.get("pred"))
    spurious = sum(len(r.get("pred") or []) for r in sel)
    return len(sel), fp_rows, spurious


def main(argv):
    gold = argv[1] if len(argv) > 1 else os.path.join(ROOT, "data/corpora/v5/gold_large.jsonl")
    preds = argv[2:]
    if not preds:
        print(__doc__)
        print("usage: abstention_quality.py <gold.jsonl> <pred.jsonl>...")
        return 1
    ids, cats = empty_gold_ids(gold)
    total = sum(1 for _ in open(gold))
    print("%s: %d of %d notes (%.1f%%) have empty gold\n"
          % (os.path.basename(gold), len(ids), total, 100.0 * len(ids) / total))
    print("%-46s %7s %14s %10s" % ("arm", "rows", "invented", "facts"))
    for p in preds:
        m = measure(p, ids)
        if m is None:
            print("%-46s  -- no overlap with this gold tier" % os.path.basename(p))
            continue
        n, fp, sp = m
        print("%-46s %7d %7d(%4.1f%%) %10d"
              % (os.path.basename(p).replace(".pred.jsonl", ""), n, fp, 100.0 * fp / n, sp))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
