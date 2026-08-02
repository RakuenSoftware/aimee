"""How much Class-C drift is avoidable naming, and how much is genuinely new?

Motivated by an observation that has_ip and device_has_ip mean the same thing, so
demoting one to a provisional Class-C edge is an ontology-exposure problem rather
than a model error.

The seed ontology carries type signatures (device_has_ip is NODE_DEVICE ->
NODE_IP, category "network") but the extraction prompt sends only a bare list of
17 predicate names. A model cannot infer that "device_has_ip" is the canonical
spelling of "has an IP" from the name alone. When it guesses has_ip, the gate
returns NOVEL and stages a provisional rel_type plus a Class-C edge.

Note the asymmetry this exposes: entities have alias resolution
(db2_entity_alias_bind binds "Billie" to a canonical node), relations have none.

SYNONYMS below is hand-built from what the models actually emitted. It is a
measurement aid, not a proposed production mapping — a real one should be derived
from the ontology and, given the KB already runs an embedder, could match novel
predicates against seed predicates semantically rather than by string table.
"""

import collections
import glob
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import prompt

ROOT = pathlib.Path(__file__).parent.parent

# Novel predicate -> the seed predicate it duplicates.
SYNONYMS = {
    "has_ip": "device_has_ip", "ip": "device_has_ip", "ip_address": "device_has_ip",
    "hostname": "has_hostname", "has_host": "has_hostname",
    "governed_by": "linked_policy", "policy": "linked_policy",
    "board member": "member_of", "joined": "member_of", "member": "member_of",
    "daughter": "child_of", "son": "child_of",
    "mother": "parent_of", "father": "parent_of",
    "started_at": "works_for", "employed_by": "works_for", "employee": "works_for",
    "works_at": "works_for", "role": "has_role", "aka": "also_known_as",
    "located": "located_in", "lives": "lives_in", "born": "born_in",
    "decided": "decided_by",
}

# Not predicates at all — parse debris, negation leakage, sentence fragments.
def is_malformed(r):
    return (r in {"is", "to", "ran", "met", "no longer on", "unknown", "other"}
            or len(r.split()) > 2
            or r.startswith("does_not") or r.startswith("no_longer"))


def main():
    seed = set(prompt.seed_relations())
    counts = collections.Counter()
    per_model = collections.defaultdict(collections.Counter)

    for f in sorted(glob.glob(str(ROOT / "results" / "gpu" / "*.pred.jsonl"))):
        rows = [json.loads(l) for l in open(f) if l.strip()]
        mid = rows[0].get("model", f)
        for r in rows:
            for t in r["pred"]:
                rel = (t.get("relation") or "").strip().lower()
                if rel and rel not in seed:
                    counts[rel] += 1
                    per_model[mid][rel] += 1

    total = sum(counts.values())
    syn = sum(n for r, n in counts.items() if r in SYNONYMS)
    mal = sum(n for r, n in counts.items() if is_malformed(r) and r not in SYNONYMS)
    new = total - syn - mal

    print(f"novel predicate emissions: {total} across {len(counts)} distinct\n")
    print(f"  duplicates an existing seed predicate : {syn:4d}  ({syn/total:.0%})"
          "   <- avoidable Class-C drift")
    print(f"  malformed / not a predicate           : {mal:4d}  ({mal/total:.0%})")
    print(f"  genuinely new relation                : {new:4d}  ({new/total:.0%})"
          "   <- Class C is the right outcome\n")

    print("most common novel predicates:")
    for r, n in counts.most_common(20):
        tag = (f"-> {SYNONYMS[r]}" if r in SYNONYMS
               else "malformed" if is_malformed(r) else "genuinely new")
        print(f"  {n:3d}  {r:22s} {tag}")

    print("\nper-model share that is avoidable drift:")
    for mid, c in sorted(per_model.items()):
        t = sum(c.values())
        if not t:
            continue
        s = sum(n for r, n in c.items() if r in SYNONYMS)
        print(f"  {mid[:34]:34s} {s:3d}/{t:3d}  {s/t:5.0%}")


if __name__ == "__main__":
    main()
