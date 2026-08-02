"""Can a small model express a retraction as polarity on the ORIGINAL fact?

Today a retraction is discarded. The prompt says a retraction "asserts a fact is
FALSE, so there is nothing durable to record", and the gold for every negation
note is empty. That is a policy, not a truth, and it is lossy: `member_of` is
multi-valued (rel_types.c), so there is no supersede path, and the edge
"Kestrel Freight member_of customer" stays ACTIVE forever no matter how many
notes say it is over.

The models already find the right fact. Measured on the 53-note negation slice,
what they emit is either the correct triple with the polarity silently dropped
(`Kestrel Freight member_of customer` for "isn't a customer any more") or an
invented predicate that means "not X" (`removed_from`, `deleted_from`,
`is_absent_from`). Both are thrown away. The information was extracted and the
schema had nowhere to put it.

This probe tests the alternative: keep the canonical relation, add a boolean.
A retraction becomes the ORIGINAL fact carrying negated=true, which is the form
a write gate can act on -- it names exactly the edge to deactivate, where
`not_located_in` names nothing the graph knows about.

The open question is not whether the design is cleaner. It is whether a 2B model
can USE it: emit negated=true on retractions, and NOT scatter it over ordinary
notes. A flag that fires everywhere is worse than no flag. So both slices are
measured, and the control slice is the one that decides it.
"""

import argparse
import json
import random
import sys

import prompt
from run_hf import extract_json
from run_llamacpp import complete

ANCHOR = "never a bare []."

# v6 rewrites the retraction sentence and the schema line. Everything else is the
# live prompt, so the comparison isolates the polarity change.
RETRACTION_V5 = ('If the note RETRACTS or DENIES something ("no longer", "did '
                 'not", "never", "is not", "has left", "was removed"), do NOT '
                 'emit the negated fact - a retraction asserts a fact is FALSE, '
                 'so there is nothing durable to record. ')
RETRACTION_V6 = ('If the note RETRACTS or DENIES something ("no longer", "did '
                 'not", "never", "is not", "has left", "was removed"), emit the '
                 'ORIGINAL fact it retracts with "negated":true - use the same '
                 'canonical relation the positive fact would use, NEVER a '
                 'negative predicate of your own such as not_member_of or '
                 'removed_from. "Kestrel Freight is no longer a customer" is '
                 '{"subject":"Kestrel Freight","relation":"member_of",'
                 '"object":"customer","negated":true}. For an ordinary fact '
                 'that is simply true, omit "negated" or set it false. ')
SCHEMA_V5 = '{"facts":[{"subject":"","relation":"","object":"","confidence":0.0}]}'
SCHEMA_V6 = ('{"facts":[{"subject":"","relation":"","object":"",'
             '"confidence":0.0,"negated":false}]}')


def build_v6(live):
    out = live.replace(SCHEMA_V5, SCHEMA_V6).replace(RETRACTION_V5, RETRACTION_V6)
    if out == live:
        raise SystemExit("v6 rewrite was a no-op; the v5 wording moved")
    return out


def sample(gold_path, n, seed):
    rows = [json.loads(l) for l in open(gold_path) if l.strip()]
    neg = [r for r in rows if r.get("category") == "negation"]
    # The control slice is every OTHER category, because the failure mode that
    # kills this design is a flag that leaks onto ordinary facts.
    ctl = [r for r in rows if r.get("category") != "negation" and r.get("gold")]
    rng = random.Random(seed)
    return neg, rng.sample(ctl, min(n, len(ctl)))


def facts_of(raw):
    """Parse, keeping the negated flag that extract_json drops."""
    try:
        start, end = raw.index("{"), raw.rindex("}")
        obj = json.loads(raw[start:end + 1])
    except Exception:  # noqa: BLE001
        return None
    fs = obj.get("facts")
    return fs if isinstance(fs, list) else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--gold", default="../results/v5-sanity/e2b/gold_sample.jsonl")
    ap.add_argument("--control-n", type=int, default=60)
    ap.add_argument("--seed", type=int, default=3)
    ap.add_argument("--max-tokens", type=int, default=8192)
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--out", default="../results/polarity/probe.jsonl")
    args = ap.parse_args()

    prompt.verify_against_source()
    live = prompt.system_prompt()
    if ANCHOR not in live:
        sys.exit("anchor gone from template")
    arms = {f"{prompt.PROMPT_VERSION}-live": live, "v6-polarity": build_v6(live)}

    neg, ctl = sample(args.gold, args.control_n, args.seed)
    print(f"{len(neg)} retraction notes, {len(ctl)} ordinary fact-bearing notes\n")
    seed_rels = set(prompt.seed_relations())
    canon = prompt.canonicalize_relation

    import pathlib
    pathlib.Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    fh = open(args.out, "w")
    for label, sp in arms.items():
        got = {"neg_flagged": 0, "neg_canon": 0, "neg_any": 0, "neg_invented": 0,
               "ctl_flagged": 0, "ctl_any": 0, "parse_fail": 0}
        for slice_name, rows in (("neg", neg), ("ctl", ctl)):
            for r in rows:
                resp = complete(args.base_url, args.model, sp, r["note"],
                                args.max_tokens, args.timeout, thinking=True)
                raw = resp["choices"][0]["message"].get("content") or ""
                fs = facts_of(raw)
                if fs is None:
                    got["parse_fail"] += 1
                    continue
                flagged = [f for f in fs if isinstance(f, dict) and f.get("negated") is True]
                if fs:
                    got[f"{slice_name}_any"] += 1
                if flagged:
                    got[f"{slice_name}_flagged"] += 1
                if slice_name == "neg":
                    for f in flagged:
                        if canon(f.get("relation", "")) in seed_rels:
                            got["neg_canon"] += 1
                        else:
                            got["neg_invented"] += 1
                fh.write(json.dumps({
                    "arm": label, "slice": slice_name, "id": r["id"],
                    "note": r["note"], "raw": raw[:1200],
                    "facts": fs, "n_flagged": len(flagged),
                }, ensure_ascii=False) + "\n")
                fh.flush()
        print(f"[{label}]")
        print(f"  retraction notes ({len(neg)}): emitted anything {got['neg_any']:2d}"
              f" | used negated=true {got['neg_flagged']:2d}"
              f" | of those, canonical relation {got['neg_canon']:2d},"
              f" invented {got['neg_invented']:2d}")
        print(f"  ordinary notes  ({len(ctl)}): emitted anything {got['ctl_any']:2d}"
              f" | LEAKED negated=true {got['ctl_flagged']:2d}   <-- must stay ~0")
        print(f"  unparseable: {got['parse_fail']}\n")
    fh.close()


if __name__ == "__main__":
    main()
