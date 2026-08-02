"""Structural checks on the gold set.

The gold set turned out to be the largest error source in this benchmark, so it
gets the same treatment as the code: automated checks that fail loudly. These
catch the mistakes that hand-editing 70 JSON records invites — a triple listed
twice, an alternative identical to its parent, a relation left blank.
"""
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import prompt

ROOT = pathlib.Path(__file__).parent.parent
CATEGORIES = {"first_person", "third_person", "multi_fact", "implicit", "negation",
              "transient", "ambiguous", "novel_pred", "infra", "governance"}


def main():
    # Takes an optional path so candidate files can be checked BEFORE they are
    # merged into gold.jsonl. It used to hard-code the gold path and ignore argv,
    # which silently validated the committed set while reporting on a file the
    # caller had passed — a pass that means nothing.
    target = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "data" / "gold.jsonl"
    if not target.exists():
        print(f"no such gold file: {target}", file=sys.stderr)
        raise SystemExit(2)
    print(f"validating {target}")
    rows = [json.loads(l) for l in target.read_text().splitlines() if l.strip()]
    errs = []
    ids = set()
    for r in rows:
        nid = r.get("id")
        if not nid or nid in ids:
            errs.append(f"{nid}: missing or duplicate id")
        ids.add(nid)
        if r.get("category") not in CATEGORIES:
            errs.append(f"{nid}: unknown category {r.get('category')!r}")
        if not (r.get("note") or "").strip():
            errs.append(f"{nid}: empty note")
        seen = set()
        for t in r.get("gold", []):
            key = (t["subject"].casefold(), t["relation"].casefold(), t["object"].casefold())
            if key in seen:
                errs.append(f"{nid}: duplicate gold triple {key}")
            seen.add(key)
            for f in ("subject", "relation", "object"):
                if not (t.get(f) or "").strip():
                    errs.append(f"{nid}: empty {f} in {t}")
            if t["relation"] != t["relation"].strip().lower():
                errs.append(f"{nid}: relation not lowercase: {t['relation']!r}")
            for a in t.get("alt", []):
                # An alternative may differ ONLY in the predicate. Both endpoints
                # must name the same entity; surface variation (Dr. vs Dr, case,
                # underscores) is already absorbed by normalisation. Renaming an
                # endpoint asserts a fact about a different node.
                if (a["subject"].casefold() != t["subject"].casefold()
                        or a["object"].casefold() != t["object"].casefold()):
                    errs.append(f"{nid}: alt {a} changes an endpoint - an "
                                f"alternative may differ only in the predicate")
                ak = (a["subject"].casefold(), a["relation"].casefold(), a["object"].casefold())
                if ak == key:
                    errs.append(f"{nid}: alt identical to its gold triple {ak}")
                if ak in seen:
                    errs.append(f"{nid}: alt duplicates another gold triple {ak}")
        # An alt on a note with no gold triples can never be reached.
        if not r.get("gold") and any(t.get("alt") for t in r.get("gold", [])):
            errs.append(f"{nid}: alt present but no gold triple to attach to")

    # Both directions of an auto-enforced inverse pair must never both appear:
    # the ontology commits one from the other, so listing both makes a model that
    # correctly states one collect a false negative for omitting the twin.
    inv = prompt.inverse_relations()
    for r in rows:
        for i, a in enumerate(r["gold"]):
            for b in r["gold"][i + 1:]:
                if (inv.get(a["relation"]) == b["relation"]
                        and a["subject"].casefold() == b["object"].casefold()
                        and a["object"].casefold() == b["subject"].casefold()):
                    errs.append(f"{r['id']}: gold lists both directions of the "
                                f"auto-enforced inverse {a['relation']}/{b['relation']}")

    # Every subject in a note's gold should be one entity per referent. Flag notes
    # using several distinct subjects, for human review — mf04 labelled the same
    # device as both "forge" and "build host", which violated the ontology's own
    # typing (has_hostname is device->SCALAR, so forge is a value, not an entity).
    for r in rows:
        subs = {t["subject"].casefold() for t in r["gold"]}
        if len(subs) > 2:
            print(f"  note {r['id']} uses {len(subs)} distinct subjects: "
                  f"{sorted(subs)} - check they are genuinely different entities")

    n_gold = sum(len(r["gold"]) for r in rows)
    n_alt = sum(len(t.get("alt", [])) for r in rows for t in r["gold"])
    n_empty = sum(1 for r in rows if not r["gold"])
    print(f"notes={len(rows)} required_triples={n_gold} alternatives={n_alt} empty_gold_notes={n_empty}")
    print(f"seed relations available: {len(prompt.seed_relations())}")
    if errs:
        print("\nERRORS:", file=sys.stderr)
        for e in errs:
            print(" ", e, file=sys.stderr)
        return 1
    print("gold set OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
