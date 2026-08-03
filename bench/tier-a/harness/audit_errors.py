"""Classify every false positive and false negative, across every model.

Written after the scorer was found wrong twice by hand-inspecting a single
model's output. Spot-checking one model finds the bugs that model happens to
trip; it says nothing about the rest. This walks all predictions and buckets each
disagreement, so systematic metric artefacts show up as counts rather than
anecdotes.

Buckets are ordered most-benign to most-real. A high `spurious` share means the
model is inventing facts; a high `inverse`/`predicate_variant` share means the
metric is penalising a naming choice the KB's reconciliation layer already
handles.
"""

import argparse
import json
import pathlib
from collections import Counter, defaultdict

import prompt
import score as S

HERE = pathlib.Path(__file__).parent
ROOT = HERE.parent


def inverses():
    import re
    src = (prompt.REPO / "src" / "rel_types.c").read_text()
    body = src[src.index("SEED_ONTOLOGY[] = {"):]
    out = {}
    for m in re.finditer(
        r'\{"([a-z_]+)",\s*\{[^}]*\},\s*\d+,\s*\{[^}]*\},\s*\d+,\s*\d+,\s*(NULL|"([a-z_]+)")',
        body):
        if m.group(3):
            out[m.group(1)] = m.group(3)
    return out


def tok_overlap(a, b):
    ta, tb = set(a.split()), set(b.split())
    return bool(ta & tb)


def classify(fp, golds, inv, sym):
    """Bucket one false positive against the note's gold triples."""
    s, r, o = fp["subject"], fp["relation"], fp["object"]
    for g in golds:
        gs, gr, go = g["subject"], g["relation"], g["object"]
        # Same edge asserted in the ontology's inverse direction. rel_types
        # auto-enforces these, so both forms land the same fact.
        if inv.get(gr) == r and s == go and o == gs:
            return "inverse_of_gold"
        if gr in sym and s == go and o == gs:
            return "symmetric_swap"
        # Right pair, different predicate — what the rel_types gate reconciles.
        if s == gs and o == go and r != gr:
            return "predicate_variant"
        # Right fact, different name for one endpoint.
        if r == gr and ((s == gs and tok_overlap(o, go)) or (o == go and tok_overlap(s, gs))):
            return "entity_variant"
    if any(tok_overlap(s, g["subject"]) or tok_overlap(o, g["object"]) for g in golds):
        return "partial_overlap"
    return "spurious"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gold", default=str(ROOT / "data" / "gold.jsonl"))
    ap.add_argument("--dir", default="gpu")
    args = ap.parse_args()

    inv, sym = inverses(), prompt.symmetric_relations()
    S.SYMMETRIC = sym
    gold_rows = [json.loads(l) for l in open(args.gold) if l.strip()]
    gold = S.load_triples(gold_rows, "gold")

    d = ROOT / "results" / args.dir
    totals = Counter()
    print(f"{'model':30s} {'FP':>4} {'inv':>4} {'sym':>4} {'pred':>5} {'ent':>4} "
          f"{'part':>5} {'spur':>5}  {'FN':>4}")
    for f in sorted(d.glob("*.pred.jsonl")):
        rows = [json.loads(l) for l in open(f) if l.strip()]
        pred = S.load_triples(rows, "pred")
        mid = rows[0].get("model", f.stem)
        buckets, fn_total, fp_total = Counter(), 0, 0
        for nid, g in gold.items():
            p = pred.get(nid, [])
            tp, used = S.match_note(p, g, True)
            fn_total += len(g) - tp
            for i, fp in enumerate(p):
                if i in used:
                    continue
                fp_total += 1
                buckets[classify(fp, g, inv, sym)] += 1
        totals.update(buckets)
        print(f"{mid[:30]:30s} {fp_total:4d} {buckets['inverse_of_gold']:4d} "
              f"{buckets['symmetric_swap']:4d} {buckets['predicate_variant']:5d} "
              f"{buckets['entity_variant']:4d} {buckets['partial_overlap']:5d} "
              f"{buckets['spurious']:5d}  {fn_total:4d}")

    print(f"\ntotals across models: {dict(totals)}")
    benign = sum(totals[k] for k in ("inverse_of_gold", "symmetric_swap"))
    reconcilable = totals["predicate_variant"] + totals["entity_variant"]
    allfp = sum(totals.values())
    if allfp:
        print(f"scored as errors but semantically identical: {benign} ({benign/allfp:.1%})")
        print(f"reconcilable by the rel_types gate / entity graph: "
              f"{reconcilable} ({reconcilable/allfp:.1%})")
        print(f"genuinely spurious: {totals['spurious']} ({totals['spurious']/allfp:.1%})")


if __name__ == "__main__":
    main()
