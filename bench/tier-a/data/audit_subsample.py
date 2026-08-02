"""Draw an audit subsample and score inter-annotator agreement on it.

Step 7 of HARNESS_DESIGN.md, and the only thing that converts "one annotator, no
inter-rater agreement" from a permanent caveat into a number.

WHY A GENERATED CORPUS STILL NEEDS THIS. Correct-by-construction means the gold
matches the fact the generator intended. It does not mean the NOTE conveys that
fact to a careful reader. A template can phrase something ambiguously and produce
gold that is internally consistent and externally unextractable — exactly what
happened twice already:

  - "The {year} {policy} supersedes the {prev_year} one" carried gold naming
    "{prev_year} {policy}", an entity absent from the sentence.
  - "The {service} box has hostname X" invited the subject "<service> box" while
    gold said "<service>", and the scorer requires an exact subject.

Both were found by hand. This finds the rest by measurement.

WHO MAY ANNOTATE. Not a model under test. gemma-4, granite, Qwen, LFM2, SmolLM2
and GLM are all on the leaderboard, and a model grading gold it will later be
scored against inflates its own number. Use the operator, or a model family that
is not in the ladder.

WHAT AGREEMENT MEANS HERE. High agreement is NOT proof the gold is right: two
annotators sharing a blind spot agree perfectly. What this reliably produces is a
DISAGREEMENT LIST — the notes whose gold is worth a human's attention. On the
hand-authored set that list would surface `mf04`, which LABELING.md already flags
as its weakest note. Treat the number as a floor on quality and the list as the
actual deliverable.
"""
import argparse
import collections
import json
import random


def load(p):
    return [json.loads(l) for l in open(p) if l.strip()]


def triple_key(t):
    return (t["subject"].strip().lower(),
            t["relation"].strip().lower(),
            t["object"].strip().lower())


def agreement(gold_rows, annot_rows):
    """Per-note exact-set agreement, plus triple-level Cohen-style counts.

    Deliberately strict: a note counts as agreed only when the two annotators
    produce the SAME SET of triples. Partial credit would hide the disagreements
    this exists to surface.
    """
    ann = {r["id"]: r for r in annot_rows}
    notes_same = notes_total = 0
    tp = only_gold = only_ann = 0
    disagreements = []
    for g in gold_rows:
        a = ann.get(g["id"])
        if a is None:
            continue
        notes_total += 1
        gs = {triple_key(t) for t in (g.get("gold") or [])}
        as_ = {triple_key(t) for t in (a.get("gold") or [])}
        tp += len(gs & as_)
        only_gold += len(gs - as_)
        only_ann += len(as_ - gs)
        if gs == as_:
            notes_same += 1
        else:
            disagreements.append({
                "id": g["id"], "category": g.get("category"),
                "domain": g.get("domain"), "note": g["note"],
                "gold_only": sorted(gs - as_), "annotator_only": sorted(as_ - gs),
            })
    return {
        "notes_compared": notes_total,
        "notes_in_full_agreement": notes_same,
        "note_agreement_rate": round(notes_same / notes_total, 4) if notes_total else None,
        "triples_agreed": tp,
        "triples_gold_only": only_gold,
        "triples_annotator_only": only_ann,
        "triple_jaccard": round(tp / (tp + only_gold + only_ann), 4)
                          if (tp + only_gold + only_ann) else None,
    }, disagreements


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--n", type=int, default=200)
    ap.add_argument("--seed", type=int, default=20260802)
    ap.add_argument("--out", help="write the subsample here (notes only, gold stripped)")
    ap.add_argument("--annotations", help="a completed annotation file to score")
    ap.add_argument("--report", help="write agreement JSON + disagreement list here")
    args = ap.parse_args()

    rows = load(args.corpus)

    if args.annotations:
        rep, dis = agreement(rows, load(args.annotations))
        print(json.dumps(rep, indent=1))
        print(f"\ndisagreements: {len(dis)}")
        for d in dis[:5]:
            print(f"  [{d['category']}] {d['note'][:70]}")
            print(f"      gold only     : {d['gold_only']}")
            print(f"      annotator only: {d['annotator_only']}")
        if args.report:
            with open(args.report, "w") as fh:
                json.dump({"agreement": rep, "disagreements": dis}, fh, indent=1)
            print(f"\nwrote {args.report}")
        return

    # Stratified draw, so the audit covers every cell rather than whichever
    # category happens to be largest.
    by = collections.defaultdict(list)
    for r in rows:
        by[(r.get("domain"), r.get("category"))].append(r)
    rng = random.Random(args.seed)
    pick = []
    for cell in sorted(by):
        v = by[cell]
        rng.shuffle(v)
        k = max(1, round(args.n * len(v) / len(rows)))
        pick.extend(v[:k])
    rng.shuffle(pick)
    pick = pick[:args.n]

    if args.out:
        with open(args.out, "w") as fh:
            for r in pick:
                # Gold is STRIPPED. An annotator shown the existing answer is not
                # an independent annotator, and the agreement number would be
                # meaningless.
                fh.write(json.dumps({"id": r["id"], "domain": r.get("domain"),
                                     "category": r.get("category"),
                                     "note": r["note"], "gold": []},
                                    ensure_ascii=False) + "\n")
        print(f"wrote {len(pick)} notes to {args.out} (gold stripped)")
    print("cells covered:", len({(r.get('domain'), r.get('category')) for r in pick}))


if __name__ == "__main__":
    main()
