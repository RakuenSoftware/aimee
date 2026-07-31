"""Compare models on the axes that actually distinguish them, not on F1 alone.

A single F1 threshold is the wrong gate. Two models at the same F1 can be doing
very different things, and the differences below are the ones that would change
a deployment decision:

  recall vs precision
      Not symmetric in this system. A missed fact is gone: the memory_facts
      drain sees each note once, so recall is the only chance to acquire it.
      A false positive still has to clear fact_grounded() and the relation kind
      gate before it commits. So a model trading precision for recall is more
      interesting than the same F1 the other way round.

  fabrication rate
      Gold-independent, and the failure the write gate cannot catch: a
      well-formed triple about an entity never mentioned looks exactly like a
      good one. gemma-4-26B-A4B sits at 0.000. A challenger that is not at or
      near 0.000 is disqualified regardless of F1.

  schema rate
      Whether the output contract holds at all. gemma-4-E4B holds 1.00 where
      several larger models do not, which is most of why it is usable.

  per-category F1
      The gold set separates negation, implicit inference, first/third person,
      infra, multi-fact and governance. Aggregate F1 hides these. A model level
      overall but clearly better on negation or implicit is doing something the
      aggregate cannot show, and those are the categories that have separated
      models so far.

  abstention on empty-gold
      23 of the 69 notes carry no facts. Restraint on those is a distinct
      skill from extraction, and the cross-configuration study showed it moves
      independently of everything else.

Usage: compare.py <label>=<score.json> [<label>=<score.json> ...]
"""

import json
import sys


AXES = [
    ("strict F1", lambda d: d["strict"]["f1"]),
    ("precision", lambda d: d["strict"]["precision"]),
    ("recall", lambda d: d["strict"]["recall"]),
    ("schema rate", lambda d: d["output_health"]["schema_rate"]),
    ("fabrication", lambda d: d["fabrication"]["fabrication_rate"]),
    ("abstain (empty-gold)", lambda d: d["over_extraction"]["abstention_rate_on_schema"]),
    ("triples emitted", lambda d: d["output_health"]["predicted_triples"]),
    ("in-seed relations", lambda d: d["output_health"]["in_seed_ontology"]),
    ("median latency ms", lambda d: d["latency_ms"]["median"]),
]


def fmt(v):
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.4f}" if v < 100 else f"{v:.0f}"
    return str(v)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    cols = []
    for arg in sys.argv[1:]:
        label, _, path = arg.partition("=")
        cols.append((label, json.load(open(path))))

    w = max(max(len(c[0]) for c in cols) + 2, 10)  # min width: short labels ran values together
    print(f"{'':<22}" + "".join(f"{c[0]:>{w}}" for c in cols))
    for name, get in AXES:
        vals = []
        for _, d in cols:
            try:
                vals.append(get(d))
            except (KeyError, TypeError):
                vals.append(None)
        print(f"{name:<22}" + "".join(f"{fmt(v):>{w}}" for v in vals))

    cats = set()
    for _, d in cols:
        cats |= set(d["strict"].get("by_category", {}))
    if not cats:
        return
    print()
    print("per-category strict F1 — where aggregate F1 hides the difference")
    print(f"{'':<22}" + "".join(f"{c[0]:>{w}}" for c in cols))
    for cat in sorted(cats):
        vals = [d["strict"].get("by_category", {}).get(cat, {}).get("f1") for _, d in cols]
        if all(v is None for v in vals):
            continue
        print(f"{cat:<22}" + "".join(f"{fmt(v):>{w}}" for v in vals))


if __name__ == "__main__":
    main()
