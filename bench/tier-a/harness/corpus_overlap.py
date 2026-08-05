#!/usr/bin/env python3
"""How much surface form does the new corpus share with the old one?

Independence is a claim, and this is the cheapest way to check it rather than
assert it. Any shared entity string is a place where the scorer's name-folding,
which was tuned on v5, could flatter v6 for reasons unrelated to the models.
"""
import json, os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def ents(p):
    s = set()
    for l in open(p):
        r = json.loads(l)
        g = r["gold"]
        g = json.loads(g) if isinstance(g, str) else g
        for t in g:
            s.add(t["subject"]); s.add(t["object"])
    return s


def main():
    old = os.path.join(ROOT, "data/corpora/v5/gold_small.jsonl")
    new = os.path.join(ROOT, "data/corpora/v6/gold_small.jsonl")
    if not os.path.exists(new):
        print("v6 not generated yet"); return 1
    a, b = ents(old), ents(new)
    sh = a & b
    print("v5 entities %d | v6 entities %d | shared %d (%.1f%% of v6)"
          % (len(a), len(b), len(sh), 100.0 * len(sh) / max(len(b), 1)))
    if sh:
        print("shared:", sorted(sh)[:15])
    else:
        print("no shared entity surface forms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
