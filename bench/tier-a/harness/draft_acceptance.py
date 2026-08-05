#!/usr/bin/env python3
"""Draft acceptance rate per MTP arm: the mechanism behind the speedup.

Speculative decoding is fast because the target accepts drafted tokens instead of
generating them. The acceptance rate is what determines the gain, and llama.cpp
reports it per request (`timings.draft_n`, `timings.draft_n_accepted`). Every MTP
figure in this project so far is wall-clock, which confounds the mechanism with
the host, the model and the backend.

Reads whatever banked arms carry the counters. Arms run before the field was
added will show as unavailable rather than as zero, because a missing counter and
a rejected draft are different things and reporting them the same way is how
defect 38 happened.
"""
import glob, json, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    pats = sys.argv[1:] or [os.path.join(ROOT, "results/vast/*.pred.jsonl")]
    print("%-46s %7s %9s %9s %8s" % ("arm", "rows", "drafted", "accepted", "rate"))
    for pat in pats:
        for f in sorted(glob.glob(pat)):
            rows = [json.loads(l) for l in open(f) if l.strip()]
            if not rows:
                continue
            have = [r for r in rows if r.get("draft_n") is not None]
            name = os.path.basename(f).replace(".pred.jsonl", "")
            if not have:
                print("%-46s %7d %9s %9s %8s" % (name, len(rows), "-", "-", "no counter"))
                continue
            d = sum(r["draft_n"] for r in have)
            a = sum(r["draft_n_accepted"] for r in have)
            print("%-46s %7d %9d %9d %7.1f%%" % (name, len(have), d, a, 100.0 * a / d if d else 0))


if __name__ == "__main__":
    main()
