"""Score a v6 run without letting the gold's old policy decide the answer.

The gold labels every retraction note EMPTY, because v1-v5 told the model a
retraction had nothing durable to record. v6 emits the retracted fact with
negated=true. Scored naively, every correct retraction is a false positive and
v6 looks worse for doing the thing it was changed to do. That number would be
real and meaningless.

So two questions are asked separately, because they are separate:

  1. Did v6 damage ordinary extraction? Score ASSERTED facts only (negated
     absent/false) against the unchanged gold. This is directly comparable to a
     v5 run, since on non-retraction notes the two prompts want the same output.

  2. Does v6 retract well? Measured on the negation slice against what the
     retraction API can actually consume: a canonical relation, and a non-empty
     object, because db2_fact_retract uses `target` to scope the retraction and
     an empty one would blank every value of (source, relation).

Plus the failure mode that would sink the design: polarity leaking onto ordinary
notes.
"""

import argparse
import json
import pathlib
import subprocess
import sys

import prompt

HERE = pathlib.Path(__file__).parent


def facts_of(row):
    """The parsed facts, keeping `negated`, which extract_json drops."""
    raw = row.get("raw") or ""
    try:
        obj = json.loads(raw[raw.index("{"):raw.rindex("}") + 1])
    except Exception:  # noqa: BLE001
        return []
    fs = obj.get("facts")
    return [f for f in fs if isinstance(f, dict)] if isinstance(fs, list) else []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gold", required=True)
    ap.add_argument("--pred", required=True)
    ap.add_argument("--outdir", required=True)
    args = ap.parse_args()

    gold = {}
    for line in open(args.gold):
        r = json.loads(line)
        gold[r["id"]] = r
    rows = [json.loads(l) for l in open(args.pred)]
    outdir = pathlib.Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    seed = set(prompt.seed_relations())
    canon = prompt.canonicalize_relation

    # --- 1. ordinary extraction, retractions removed from the predictions ---
    asserted_path = outdir / "asserted_only.pred.jsonl"
    with open(asserted_path, "w") as fh:
        for r in rows:
            keep = [f for f in facts_of(r) if f.get("negated") is not True]
            r2 = dict(r)
            r2["pred_nofloor"] = [
                {"subject": f.get("subject", ""), "relation": f.get("relation", ""),
                 "object": f.get("object", ""),
                 "confidence": f.get("confidence", 0.0) or 0.0}
                for f in keep]
            r2["pred"] = r2["pred_nofloor"]
            fh.write(json.dumps(r2, ensure_ascii=False) + "\n")
    print("=== 1. ordinary extraction (negated facts removed, gold unchanged)")
    subprocess.run([sys.executable, str(HERE / "score.py"), "--gold", args.gold,
                    "--pred", str(asserted_path), "--json-out",
                    str(outdir / "asserted_only.score.json")],
                   check=False, stdout=subprocess.DEVNULL)
    s = json.load(open(outdir / "asserted_only.score.json"))
    for view in ("strict", "relation_agnostic"):
        v = s[view]
        print(f"    {view:18s} F1 {v['f1']:.4f}  P {v['precision']:.4f}  R {v['recall']:.4f}")

    # --- 2. retraction quality on the negation slice ---
    neg = [r for r in rows if gold[r["id"]]["category"] == "negation"]
    flagged = usable = bad_rel = empty_obj = 0
    examples = []
    for r in neg:
        fs = [f for f in facts_of(r) if f.get("negated") is True]
        if not fs:
            continue
        flagged += 1
        for f in fs:
            rel_ok = canon(f.get("relation", "")) in seed
            obj_ok = bool(str(f.get("object", "")).strip())
            if rel_ok and obj_ok:
                usable += 1
            else:
                if not rel_ok:
                    bad_rel += 1
                if not obj_ok:
                    empty_obj += 1
                if len(examples) < 6:
                    examples.append((gold[r["id"]]["note"], f))
    print(f"\n=== 2. retraction quality ({len(neg)} negation notes)")
    print(f"    flagged with negated=true      {flagged:4d}/{len(neg)}")
    print(f"    usable by db2_fact_retract     {usable:4d}   (canonical relation AND non-empty object)")
    print(f"    invented/non-canonical relation{bad_rel:4d}")
    print(f"    EMPTY object                   {empty_obj:4d}   <- would blank all values of (source,relation)")
    for n, f in examples:
        print(f"      \"{n[:62]}\"\n         -> {json.dumps(f, ensure_ascii=False)[:120]}")

    # --- 3. the failure mode that sinks the design ---
    others = [r for r in rows if gold[r["id"]]["category"] != "negation"]
    leaked = [r for r in others if any(f.get("negated") is True for f in facts_of(r))]
    print(f"\n=== 3. polarity leak on non-retraction notes")
    print(f"    {len(leaked)}/{len(others)} notes carry a negated fact")
    for r in leaked[:6]:
        f = next(f for f in facts_of(r) if f.get("negated") is True)
        print(f"      [{gold[r['id']]['category']}] \"{gold[r['id']]['note'][:60]}\"")
        print(f"         -> {json.dumps(f, ensure_ascii=False)[:120]}")


if __name__ == "__main__":
    main()
