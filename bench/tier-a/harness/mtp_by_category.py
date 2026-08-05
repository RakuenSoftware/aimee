#!/usr/bin/env python3
"""Split the paired MTP arms by note category.

Article 6 open item 3. The aggregate MTP-vs-no-MTP delta is a precise null
(E4B Q4: +0.0005, 95% CI [-0.0028,+0.0036] at n=10000). On a *different*
question in this project -- reasoning on versus off -- an aggregate null over
this same corpus turned out to be +0.24 F1 on one subset and -0.02 on another,
cancelling. Nothing has checked whether the MTP null has the same shape.

This does not re-run anything. It scores banked predictions per category.

Reported per category per pair: MTP F1, no-MTP F1, delta, and n. A category
whose delta exceeds the aggregate interval is flagged; whether it is real then
needs its own bootstrap, per defect 39 -- a per-category delta at n~1000 has an
interval near +/-0.024, so most of them will not survive and the flag is a
shortlist, not a finding.
"""
import json, subprocess, sys, tempfile, os
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GOLD = os.path.join(ROOT, "data/corpora/v5/gold_large.jsonl")
MTP_DIR = os.path.join(ROOT, "results/10k-sharded")
NOM_DIR = os.path.join(ROOT, "results/10k-nomtp")

PAIRS = ["E2B.UD-Q4_K_XL.10k", "E2B.UD-Q6_K_XL.10k", "E2B.UD-Q8_K_XL.10k",
         "E4B.UD-Q4_K_XL.10k", "E4B.UD-Q6_K_XL.10k", "E4B.UD-Q8_K_XL.10k"]


def score(gold_rows, pred_rows):
    """Run the unmodified scorer over a subset. score.py stays unchanged."""
    g = tempfile.mktemp(suffix=".jsonl")
    p = tempfile.mktemp(suffix=".jsonl")
    o = tempfile.mktemp(suffix=".json")
    with open(g, "w") as fh:
        fh.write("".join(json.dumps(r) + "\n" for r in gold_rows))
    with open(p, "w") as fh:
        fh.write("".join(json.dumps(r) + "\n" for r in pred_rows))
    cmd = [sys.executable, os.path.join(ROOT, "harness/score.py"),
           "--gold", g, "--pred", p, "--json-out", o]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        cmd.append("--allow-thinking-off")
        r = subprocess.run(cmd, capture_output=True)
        if r.returncode != 0:
            return None
    with open(o) as fh:
        return json.load(fh)["strict"]["f1"]


def gold_facts(row):
    g = row["gold"]
    return json.loads(g) if isinstance(g, str) else g


def load(path):
    if not os.path.exists(path):
        return None
    return {json.loads(l)["id"]: json.loads(l) for l in open(path)}


def main():
    gold = {json.loads(l)["id"]: json.loads(l) for l in open(GOLD)}
    by_cat = defaultdict(list)
    for i, r in gold.items():
        by_cat[r["category"]].append(i)

    print("MTP minus no-MTP, strict F1, split by note category")
    print("gold_large, 10000 notes, 3 processes, XTX\n")

    for pair in PAIRS:
        m = load(os.path.join(MTP_DIR, pair + ".pred.jsonl"))
        n = load(os.path.join(NOM_DIR, pair + ".pred.jsonl"))
        if not m or not n:
            print("%-22s -- no-MTP side not banked yet, skipped" % pair)
            continue
        shared = set(m) & set(n) & set(gold)
        agg_m = score([gold[i] for i in sorted(shared)],
                      [m[i] for i in sorted(shared)])
        agg_n = score([gold[i] for i in sorted(shared)],
                      [n[i] for i in sorted(shared)])
        print("=== %s   aggregate MTP %.4f  no-MTP %.4f  delta %+.4f  n=%d"
              % (pair, agg_m, agg_n, agg_m - agg_n, len(shared)))
        print("    %-16s %6s %8s %8s %8s" % ("category", "n", "MTP", "noMTP", "delta"))
        rows = []
        for cat, ids in sorted(by_cat.items()):
            ids = [i for i in ids if i in shared]
            if len(ids) < 50:
                continue
            # A category whose notes carry no gold triples has UNDEFINED F1, not
            # zero. score.py emits null for exactly these and says why: fp=0 on a
            # factless note is perfect restraint, so printing 0.0 inverts the
            # meaning and ranks the best behaviour as the worst. This script
            # printed 0.0000 for negation/transient/ambiguous in its first
            # version, reintroducing the bug the scorer was written to avoid.
            # Read abstention_rate_on_schema in the score.json for these.
            if not any(gold_facts(gold[i]) for i in ids):
                print("    %-16s %6d %8s %8s %8s   (no gold triples; see "
                      "abstention_rate_on_schema)" % (cat, len(ids), "n/a", "n/a", "n/a"))
                continue
            fm = score([gold[i] for i in ids], [m[i] for i in ids])
            fn_ = score([gold[i] for i in ids], [n[i] for i in ids])
            if fm is None or fn_ is None:
                continue
            rows.append((cat, len(ids), fm, fn_, fm - fn_))
        for cat, k, fm, fn_, d in sorted(rows, key=lambda r: -abs(r[4])):
            flag = "  <-- exceeds aggregate interval" if abs(d) > 0.024 else ""
            print("    %-16s %6d %8.4f %8.4f %+8.4f%s" % (cat, k, fm, fn_, d, flag))
        pos = sum(1 for r in rows if r[4] > 0)
        print("    sign: %d categories positive, %d negative, spread %.4f\n"
              % (pos, len(rows) - pos,
                 max(r[4] for r in rows) - min(r[4] for r in rows)))


if __name__ == "__main__":
    main()
