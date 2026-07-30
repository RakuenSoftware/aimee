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


def norm(s):
    if s is None:
        return ""
    s = str(s).casefold().strip()
    s = re.sub(r"\s+", " ", s)
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


def match_note(preds, golds, lenient):
    """Greedy 1-1 match. Returns (tp, matched_pred_idx)."""
    used, tp = set(), 0
    for g in golds:
        for i, p in enumerate(preds):
            if i in used:
                continue
            if p["subject"] == g["subject"] and p["relation"] == g["relation"] \
               and obj_match(p["object"], g["object"], lenient):
                used.add(i)
                tp += 1
                break
    return tp, used


def prf(tp, fp, fn):
    p = tp / (tp + fp) if tp + fp else 0.0
    r = tp / (tp + fn) if tp + fn else 0.0
    f = 2 * p * r / (p + r) if p + r else 0.0
    return p, r, f


def load_triples(rows, key):
    out = {}
    for r in rows:
        ts = []
        for t in r.get(key) or []:
            ts.append({
                "subject": norm(t.get("subject")),
                "relation": norm(t.get("relation")),
                "object": norm(t.get("object")),
            })
        out[r["id"]] = ts
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gold", required=True)
    ap.add_argument("--pred", required=True)
    ap.add_argument("--json-out")
    args = ap.parse_args()

    gold_rows = [json.loads(l) for l in open(args.gold) if l.strip()]
    pred_rows = [json.loads(l) for l in open(args.pred) if l.strip()]
    gold = load_triples(gold_rows, "gold")
    pred = load_triples(pred_rows, "pred")
    cat = {r["id"]: r["category"] for r in gold_rows}
    pmeta = {r["id"]: r for r in pred_rows}

    seed = set(prompt.seed_relations())

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

    # Over-extraction: notes whose gold is the empty list. Any triple here is a
    # false positive that the write gate would then have to catch.
    empty_ids = [i for i, g in gold.items() if not g]
    spurious = sum(len(pred.get(i, [])) for i in empty_ids)
    clean = sum(1 for i in empty_ids if not pred.get(i))
    report["over_extraction"] = {
        "empty_gold_notes": len(empty_ids),
        "notes_correctly_empty": clean,
        "abstention_rate": round(clean / len(empty_ids), 4) if empty_ids else None,
        "spurious_triples": spurious,
    }

    # Operational health: did we get parseable JSON, and did it respect the ontology?
    ok = sum(1 for r in pred_rows if r.get("parse_ok"))
    rels = [t["relation"] for ts in pred.values() for t in ts]
    report["output_health"] = {
        "notes": len(pred_rows),
        "json_parse_ok": ok,
        "json_parse_rate": round(ok / len(pred_rows), 4) if pred_rows else 0.0,
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

    out = json.dumps(report, indent=2)
    print(out)
    if args.json_out:
        open(args.json_out, "w").write(out + "\n")


if __name__ == "__main__":
    main()
