"""Score Tier-A triple extraction against the gold set.

Reads a predictions JSONL (one row per note, produced by a runner) and the gold
set, and emits the metrics the proposal's §4.2 asks for: triple
precision/recall/F1, plus the over-extraction rate that matters most for a drain
that commits into memory_facts.

Matching is greedy 1-1 within a note. Subject and relation must match exactly
after normalization under both modes; only object comparison loosens in lenient
mode. See data/LABELING.md.
"""

import argparse
import json
import re
from collections import defaultdict

import prompt

ARTICLES = {"the", "a", "an"}

# Models emit ages and counts as words as readily as digits ("Nina is seven").
# The ontology stores a scalar either way, so treating them as different answers
# measures spelling, not extraction.
NUMBER_WORDS = {
    "zero": "0", "one": "1", "two": "2", "three": "3", "four": "4", "five": "5",
    "six": "6", "seven": "7", "eight": "8", "nine": "9", "ten": "10",
    "eleven": "11", "twelve": "12",
}


def norm(s):
    if s is None:
        return ""
    s = str(s).casefold().strip()
    s = re.sub(r"\s+", " ", s)
    if s in NUMBER_WORDS:
        return NUMBER_WORDS[s]
    # Strip edge punctuation but preserve internal dots/colons so IPs and
    # hostnames survive intact.
    s = re.sub(r"^[^\w]+|[^\w]+$", "", s)
    toks = s.split()
    while toks and toks[0] in ARTICLES:
        toks.pop(0)
    return " ".join(toks)


def tok_f1(a, b):
    ta, tb = set(a.split()), set(b.split())
    if not ta or not tb:
        return 1.0 if ta == tb else 0.0
    inter = len(ta & tb)
    if not inter:
        return 0.0
    p, r = inter / len(ta), inter / len(tb)
    return 2 * p * r / (p + r)


def obj_match(pred, gold, lenient):
    if pred == gold:
        return True
    return lenient and tok_f1(pred, gold) >= 0.6


SYMMETRIC = None  # populated from the ontology in main()
INVERSES = None


def triple_eq(p, g, lenient):
    if p["relation"] == g["relation"]:
        if p["subject"] == g["subject"] and obj_match(p["object"], g["object"], lenient):
            return True
        # The ontology declares some relations symmetric ("one assertion implies
        # both directions"), so argument order carries no information for them.
        if g["relation"] in (SYMMETRIC or ()):
            return p["subject"] == g["object"] and obj_match(p["object"], g["subject"], lenient)
        return False
    # inverse_rel_type is documented as "auto-enforced": asserting (a parent_of b)
    # commits (b child_of a) too, so the two forms are one fact and scoring them
    # as different answers measures direction of phrasing, not correctness.
    if (INVERSES or {}).get(g["relation"]) == p["relation"]:
        return p["subject"] == g["object"] and obj_match(p["object"], g["subject"], lenient)
    return False


def match_note(preds, golds, lenient, ignore_relation=False):
    """Greedy 1-1 match. Returns (tp, matched_pred_idx).

    ignore_relation credits a pair on subject and object alone. That is not a
    quality metric — it is a diagnostic that separates "did not find the fact"
    from "found it and labelled the edge differently". The two failures have
    completely different fixes: the first needs a better model, the second is
    what the rel_types reconciliation gate already exists to absorb.
    """
    used, tp = set(), 0
    for g in golds:
        for i, p in enumerate(preds):
            if i in used:
                continue
            if ignore_relation:
                ok = (p["subject"] == g["subject"]
                      and obj_match(p["object"], g["object"], lenient)) or \
                     (p["subject"] == g["object"]
                      and obj_match(p["object"], g["subject"], lenient))
            else:
                ok = triple_eq(p, g, lenient)
            if ok:
                used.add(i)
                tp += 1
                break
    return tp, used


FIRST_PERSON = {"user", "i", "me", "my", "myself", "we", "us"}


def ground_text(s):
    """Normalise for grounding comparisons.

    Models write kb_server for "KB server" and 7 for "seven"; both are the same
    entity written differently, and counting them as fabrication would measure
    spelling. Underscores and hyphens become spaces, and number words are mapped
    to digits on BOTH sides so the two forms meet.
    """
    s = re.sub(r"[_\-/]+", " ", str(s).casefold())
    s = re.sub(r"[^\w\s.:]+", " ", s)
    s = re.sub(r"\s+", " ", s).strip()
    # Dots are kept so IPs and hostnames survive, but a sentence-final "seven."
    # must still meet the digit form, so strip trailing dots per token.
    return " ".join(NUMBER_WORDS.get(t.rstrip("."), t.rstrip("."))
                    for t in s.split())


def grounded(value, note_norm):
    """Can this argument be traced to the source note?

    Deliberately independent of the gold labels: it asks whether the model
    invented an entity, not whether it picked the entity I happened to label.
    A fabricated endpoint is the failure that matters most for a drain writing
    into memory_facts, because the write gate cannot catch it — a well-formed
    triple about a person who was never mentioned looks exactly like a good one.

    "user" is grounded by convention: the prompt instructs the model to use it as
    the subject for first-person notes.
    """
    v = ground_text(value)
    if not v or v in FIRST_PERSON:
        return True
    if v in note_norm:
        return True
    note_toks = set(note_norm.split())
    toks = [t for t in v.split() if t not in ARTICLES and len(t) > 2]
    if not toks:
        return v in note_toks
    hit = sum(1 for t in toks if t in note_toks or t in note_norm)
    # Majority of content tokens present: tolerates "Rakuen Software Ltd" for
    # "Rakuen Software" without tolerating an invented name.
    return hit * 2 >= len(toks)


def prf(tp, fp, fn):
    p = tp / (tp + fp) if tp + fp else 0.0
    r = tp / (tp + fn) if tp + fn else 0.0
    f = 2 * p * r / (p + r) if p + r else 0.0
    return p, r, f


def derive_schema_ok(row):
    """Re-derive schema validity from the raw output, correcting the runners.

    The runners recorded schema_ok=False for anything without a "facts" array,
    which swept up {} and [] — and every single one of those, across every model,
    turned out to be an empty answer on a note that asserts no durable fact. That
    is a correct abstention in a terser shape, not a malformed response, and
    production agrees: mf_commit_facts() commits nothing either way.

    Counting it as a schema failure produced a false headline — schema validity
    appearing to degrade monotonically with model size (1.00 -> 0.96 -> 0.84 ->
    0.77) when what actually varies is how tersely a model says "nothing here".
    It also excluded those notes from the abstention denominator, understating
    abstention for exactly the models that abstained most.

    schema_ok is now False only for output that carries content in the wrong
    shape (a bare fact object, prose, unparseable text) — a real failure that
    silently commits nothing.
    """
    raw = (row.get("raw") or "").strip()
    if raw in ("{}", "[]", "{ }", "[ ]", ""):
        return True
    start, end = raw.find("{"), raw.rfind("}")
    if start == -1 or end < start:
        return False
    try:
        obj = json.loads(raw[start:end + 1])
    except json.JSONDecodeError:
        return False
    if isinstance(obj, dict) and isinstance(obj.get("facts"), list):
        return True
    # Valid JSON, no facts array, and not empty: wrong shape carrying content.
    return isinstance(obj, dict) and not obj


def load_triples(rows, key, canonicalize=False):
    """canonicalize applies rel_type_canonicalize()'s alias folding, which is what
    the commit path now does before a triple reaches the gate. Applied to
    predictions only — gold labels are authored canonical."""
    out = {}
    for r in rows:
        ts = []
        for t in r.get(key) or []:
            rel = t.get("relation")
            if canonicalize:
                rel = prompt.canonicalize_relation(rel)
            ts.append({
                "subject": norm(t.get("subject")),
                "relation": norm(rel),
                "object": norm(t.get("object")),
            })
        out[r["id"]] = ts
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gold", required=True)
    ap.add_argument("--pred", required=True)
    ap.add_argument("--pred-key", default="pred",
                    help="'pred' scores what production would commit (confidence "
                         "floor applied); 'pred_nofloor' scores the same extraction "
                         "with MF_CONF_FLOOR lifted.")
    ap.add_argument("--no-alias", action="store_true",
                    help="skip rel_type_canonicalize() alias folding. Production "
                         "folds, so this only exists to measure what aliasing buys.")
    ap.add_argument("--json-out")
    args = ap.parse_args()

    gold_rows = [json.loads(l) for l in open(args.gold) if l.strip()]
    pred_rows = [json.loads(l) for l in open(args.pred) if l.strip()]
    gold = load_triples(gold_rows, "gold")
    pred = load_triples(pred_rows, args.pred_key, canonicalize=not args.no_alias)
    cat = {r["id"]: r["category"] for r in gold_rows}
    pmeta = {r["id"]: r for r in pred_rows}
    for r in pred_rows:
        r["schema_ok"] = derive_schema_ok(r)

    seed = set(prompt.seed_relations())
    global SYMMETRIC, INVERSES
    SYMMETRIC = prompt.symmetric_relations()
    INVERSES = prompt.inverse_relations()

    # Diagnostic: how much of the error is edge-labelling rather than a missed
    # fact? Scored on the lenient object rule, relation ignored.
    TPr = FPr = FNr = 0
    for nid, g in gold.items():
        p = pred.get(nid, [])
        tp, used = match_note(p, g, True, ignore_relation=True)
        TPr, FPr, FNr = TPr + tp, FPr + (len(p) - len(used)), FNr + (len(g) - tp)
    Pr, Rr, Fr = prf(TPr, FPr, FNr)

    report = {}
    for mode in ("strict", "lenient"):
        lenient = mode == "lenient"
        TP = FP = FN = 0
        by_cat = defaultdict(lambda: [0, 0, 0])
        for nid, g in gold.items():
            p = pred.get(nid, [])
            tp, used = match_note(p, g, lenient)
            fp, fn = len(p) - len(used), len(g) - tp
            TP, FP, FN = TP + tp, FP + fp, FN + fn
            c = by_cat[cat[nid]]
            c[0] += tp; c[1] += fp; c[2] += fn
        P, R, F = prf(TP, FP, FN)
        report[mode] = {
            "precision": round(P, 4), "recall": round(R, 4), "f1": round(F, 4),
            "tp": TP, "fp": FP, "fn": FN,
            "by_category": {k: dict(zip(("precision", "recall", "f1"),
                                        [round(x, 4) for x in prf(*v)]),
                                    tp=v[0], fp=v[1], fn=v[2])
                            for k, v in sorted(by_cat.items())},
        }

    # Fabrication: triples with an endpoint that cannot be traced to the note.
    # Reported separately from precision because the two failures differ in kind
    # — a mislabelled edge is recoverable downstream, an invented entity is not.
    notes = {r["id"]: ground_text(r["note"]) for r in gold_rows}
    ungrounded, ungrounded_examples, total_pred = 0, [], 0
    for nid, ts in pred.items():
        nn = notes.get(nid, "")
        for t in ts:
            total_pred += 1
            bad = [k for k in ("subject", "object") if not grounded(t[k], nn)]
            if bad:
                ungrounded += 1
                if len(ungrounded_examples) < 12:
                    ungrounded_examples.append(
                        {"id": nid, "triple": [t["subject"], t["relation"], t["object"]],
                         "ungrounded_args": bad})
    report["fabrication"] = {
        "_note": "a triple is counted here when a subject or object cannot be traced "
                 "to the source note. Gold-independent: it measures invented entities, "
                 "not disagreement with my labels. The write gate cannot catch these — "
                 "a well-formed triple about someone never mentioned looks valid.",
        "predicted_triples": total_pred,
        "ungrounded_triples": ungrounded,
        "fabrication_rate": round(ungrounded / total_pred, 4) if total_pred else None,
        "examples": ungrounded_examples,
    }

    report["relation_agnostic"] = {
        "_note": "diagnostic, not a quality score: subject+object matched, relation "
                 "ignored. The gap to lenient F1 is the share of error that is edge "
                 "labelling rather than a missed fact.",
        "precision": round(Pr, 4), "recall": round(Rr, 4), "f1": round(Fr, 4),
        "tp": TPr, "fp": FPr, "fn": FNr,
    }

    # Over-extraction: notes whose gold is the empty list. Any triple here is a
    # false positive that the write gate would then have to catch.
    #
    # Abstention is counted only over notes where the model actually emitted the
    # {"facts":[...]} shape. A model that returns valid JSON of the wrong shape
    # commits nothing in production, but it has not decided the note is factless —
    # crediting that as abstention would make a broken model look maximally
    # precise. Both denominators are reported so the distinction stays visible.
    empty_ids = [i for i, g in gold.items() if not g]
    spurious = sum(len(pred.get(i, [])) for i in empty_ids)
    on_schema = [i for i in empty_ids if pmeta.get(i, {}).get("schema_ok")]
    # Note: an explicit {} on a factless note now counts as a schema-valid
    # abstention, so these denominators include it.
    clean = sum(1 for i in on_schema if not pred.get(i))
    report["over_extraction"] = {
        "empty_gold_notes": len(empty_ids),
        "on_schema_empty_gold_notes": len(on_schema),
        "notes_correctly_empty": clean,
        "abstention_rate_on_schema": round(clean / len(on_schema), 4) if on_schema else None,
        "spurious_triples": spurious,
    }

    # Operational health: did we get parseable JSON of the right shape, and did it
    # respect the ontology?
    ok = sum(1 for r in pred_rows if r.get("parse_ok"))
    schema = sum(1 for r in pred_rows if r["schema_ok"])
    rels = [t["relation"] for ts in pred.values() for t in ts]
    report["output_health"] = {
        "notes": len(pred_rows),
        "json_parse_ok": ok,
        "json_parse_rate": round(ok / len(pred_rows), 4) if pred_rows else 0.0,
        "schema_ok": schema,
        "schema_rate": round(schema / len(pred_rows), 4) if pred_rows else 0.0,
        "malformed_facts": sum(r.get("malformed_facts", 0) for r in pred_rows),
        "dropped_by_conf_floor": sum(r.get("dropped_by_conf_floor", 0) for r in pred_rows),
        "predicted_triples": len(rels),
        "in_seed_ontology": round(sum(1 for r in rels if r in seed) / len(rels), 4) if rels else None,
        "catch_all_relations": sum(1 for r in rels if r in {"other", "unknown", "misc"}),
    }
    lat = sorted(r["latency_ms"] for r in pred_rows if r.get("latency_ms") is not None)
    if lat:
        report["latency_ms"] = {
            "median": lat[len(lat) // 2],
            "p90": lat[int(len(lat) * 0.9)],
            "mean": round(sum(lat) / len(lat), 1),
        }
    toks = [r["completion_tokens"] for r in pred_rows if r.get("completion_tokens") is not None]
    if toks:
        report["completion_tokens"] = {
            "median": sorted(toks)[len(toks) // 2],
            "mean": round(sum(toks) / len(toks), 1),
            "max": max(toks),
        }
    report["model"] = pmeta[pred_rows[0]["id"]].get("model") if pred_rows else None
    report["scored_key"] = args.pred_key
    report["alias_folding"] = not args.no_alias

    out = json.dumps(report, indent=2)
    print(out)
    if args.json_out:
        open(args.json_out, "w").write(out + "\n")


if __name__ == "__main__":
    main()
