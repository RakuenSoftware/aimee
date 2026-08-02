"""Find a system prompt E4B will both obey and think under.

The v4 prompt suppresses gemma-4-E4B's reasoning entirely: 0 reasoning chars on
25/25 notes, against 25/25 that think when `No prose, no markdown.` is removed.
The model extends an instruction about its ANSWER to its own thought channel.

Removing the clause is not automatically the fix. It is there to stop models
fencing the JSON, and a variant that thinks but returns ```json is a regression
somewhere else. So both properties are measured together, on real corpus notes:

  thinks   -- reasoning_chars > 0
  obeys    -- content parses as the {"facts":[...]} wrapper, unfenced

A variant only wins if it does both on every note. Greedy decoding, same call
shape as run_llamacpp.complete(), so this measures the prompt and nothing else.
"""

import argparse
import json
import random
import statistics
import sys

import prompt
from run_hf import extract_json
from run_llamacpp import complete

# The clause under test is whatever currently follows "never a bare []." — named
# by its anchor rather than its text so this probe keeps working across prompt
# versions. Hard-coding the v4 wording made it refuse to run the moment v5 landed,
# which is exactly when it is most needed.
ANCHOR = "never a bare []."

# Each variant is the production template with the final clause replaced. Keeping
# them as substitutions of one clause is deliberate: everything else stays
# byte-identical to what production sends, so a difference in outcome has one
# cause.
VARIANTS = {
    # Control. Expected to score thinks=0 -- if it does not, the bug is not
    # reproducing today and nothing below is interpretable.
    "v4-control": " No prose, no markdown.",
    # The measured-good case from the bisect, kept as the upper bound.
    "drop": "",
    # Rescopings: same intent, but the constraint is bound to the answer rather
    # than left open enough for the model to apply it to its reasoning.
    "answer-only": " The answer itself must be a JSON object only, with no prose"
                   " or markdown around it.",
    "final-answer": " Your final answer must contain the JSON object and nothing"
                    " else.",
    # Explicit permission. Tests whether E4B needs to be told the thought channel
    # is exempt, or merely needs the instruction not to reach it.
    "permit": " Reason first if it helps; the answer that follows must be the"
              " JSON object only, no prose, no markdown.",
}


def sample_notes(gold_path, n, seed):
    rows = [json.loads(l) for l in open(gold_path) if l.strip()]
    # Stratify on whether the note carries facts. An all-empty sample would let a
    # variant look obedient by emitting {"facts":[]} to everything.
    bearing = [r for r in rows if r.get("gold")]
    empty = [r for r in rows if not r.get("gold")]
    rng = random.Random(seed)
    take_b = min(len(bearing), n - n // 4)
    take_e = min(len(empty), n - take_b)
    return rng.sample(bearing, take_b) + rng.sample(empty, take_e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", default="http://192.168.0.5:8113")
    ap.add_argument("--model", default="E4B.UD-Q4_K_XL")
    ap.add_argument("--gold", default="../data/corpora/v4/gold_mid.jsonl")
    ap.add_argument("--n", type=int, default=20)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--max-tokens", type=int, default=8192)
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--out", default="../results/thinking-prompt/probe.jsonl")
    args = ap.parse_args()

    prompt.verify_against_source()
    base = prompt.system_prompt()
    if ANCHOR not in base:
        sys.exit(f"anchor {ANCHOR!r} is gone from the template; re-point the probe")
    stem = base[: base.index(ANCHOR) + len(ANCHOR)]
    # The shipped tail is a variant too, so the current prompt is always measured
    # alongside the alternatives rather than assumed still good.
    VARIANTS[f"shipped-{prompt.PROMPT_VERSION}"] = base[len(stem):]

    notes = sample_notes(args.gold, args.n, args.seed)
    print(f"{len(notes)} notes ({sum(1 for r in notes if r.get('gold'))} fact-bearing)\n")

    out = open(args.out, "w")
    summary = []
    for name, tail in VARIANTS.items():
        sys_prompt = stem + tail
        thinks = obeys = fenced = 0
        rchars, nfacts = [], []
        for r in notes:
            resp = complete(args.base_url, args.model, sys_prompt, r["note"],
                            args.max_tokens, args.timeout, thinking=True)
            msg = resp["choices"][0]["message"]
            raw = msg.get("content") or ""
            reasoning = msg.get("reasoning_content") or ""
            facts, ok, schema_ok, _ = extract_json(raw)
            thinks += len(reasoning) > 0
            obeys += bool(ok and schema_ok)
            fenced += "```" in raw
            rchars.append(len(reasoning))
            nfacts.append(len(facts))
            out.write(json.dumps({
                "variant": name, "id": r["id"], "note": r["note"],
                "reasoning_chars": len(reasoning), "reasoning": reasoning[:1500],
                "raw": raw[:2000], "parse_ok": ok, "schema_ok": schema_ok,
                "n_facts": len(facts), "n_gold": len(r.get("gold") or []),
            }, ensure_ascii=False) + "\n")
            out.flush()
        med = statistics.median(rchars) if rchars else 0
        summary.append((name, thinks, obeys, fenced, med, sum(nfacts)))
        print(f"{name:14s} thinks {thinks:2d}/{len(notes)}  obeys {obeys:2d}/{len(notes)}"
              f"  fenced {fenced:2d}  median_reasoning {med:5.0f}  facts {sum(nfacts):3d}")
    out.close()

    print()
    clean = [s for s in summary
             if s[0] != "v4-control" and s[1] == len(notes) and s[2] == len(notes)]
    if summary[0][1] != 0:
        print("WARNING: the control thought on some notes -- the bug is not "
              "reproducing cleanly, treat the rest as uninterpretable.")
    if clean:
        print("thinks AND obeys on every note: " + ", ".join(s[0] for s in clean))
    else:
        print("no variant is clean on both axes; see the per-note rows.")


if __name__ == "__main__":
    main()
