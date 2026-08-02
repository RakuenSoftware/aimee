"""A/B one prompt version against another on the same notes, same server.

v5 was adopted to stop gemma-4-E4B suppressing its own reasoning. E2B never had
that problem, so for E2B v5 is a change with an upside that does not apply and a
downside that might: it lengthens the prompt and it invites reasoning from a
model that was previously answering directly. "It should be fine" is not a
measurement, and v5 is what production now sends to every model.

Emits run_llamacpp-compatible rows so score.py scores both arms with no special
casing -- the point is to compare F1 under the real scorer, not a proxy.

Both arms run against the SAME server process and the SAME sampled notes, so the
prompt is the only thing that differs between them.
"""

import argparse
import json
import pathlib
import random
import time

import prompt
from run_hf import CONF_FLOOR, extract_json
from run_llamacpp import complete

ANCHOR = "never a bare []."

# The historical tails, by version. v5 is read from the live template rather than
# duplicated, so this cannot drift from what production sends.
TAILS = {
    "v4": " No prose, no markdown.",
}


def sample(gold_path, n, seed):
    rows = [json.loads(l) for l in open(gold_path) if l.strip()]
    if n >= len(rows):
        return rows
    # Proportional by category, so the negation and third_person slices that the
    # prompt changes actually target are present rather than left to chance.
    by_cat = {}
    for r in rows:
        by_cat.setdefault(r.get("category", "?"), []).append(r)
    rng = random.Random(seed)
    out = []
    for cat, rs in sorted(by_cat.items()):
        take = max(1, round(n * len(rs) / len(rows)))
        out.extend(rng.sample(rs, min(take, len(rs))))
    return out


def run_arm(notes, sys_prompt, label, base_url, model, args, out_path):
    with open(out_path, "w") as fh:
        for i, r in enumerate(notes, 1):
            t0 = time.perf_counter()
            try:
                resp = complete(base_url, model, sys_prompt, r["note"],
                                args.max_tokens, args.timeout, thinking=True)
                msg = resp["choices"][0]["message"]
                raw = msg.get("content") or ""
                reasoning = msg.get("reasoning_content") or ""
                usage = resp.get("usage") or {}
                err = None
            except Exception as e:  # noqa: BLE001 - recorded, not swallowed
                raw, usage, err, reasoning = "", {}, f"{type(e).__name__}: {e}", ""
            dt = (time.perf_counter() - t0) * 1000
            facts, ok, schema_ok, malformed = extract_json(raw)
            floored = [f for f in facts if f["confidence"] >= CONF_FLOOR]
            fh.write(json.dumps({
                "id": r["id"], "model": model, "runtime": "llama.cpp",
                "pred": floored, "pred_nofloor": facts,
                "parse_ok": ok, "schema_ok": schema_ok,
                "malformed_facts": malformed,
                "dropped_by_conf_floor": len(facts) - len(floored),
                "raw": raw[:4000], "error": err, "latency_ms": round(dt, 1),
                "completion_tokens": usage.get("completion_tokens"),
                "prompt_tokens": usage.get("prompt_tokens"),
                "truncated": usage.get("completion_tokens") == args.max_tokens,
                "thinking": True, "prompt_version": label,
                "reasoning_chars": len(reasoning), "reasoning": reasoning[:2000],
                "fenced": "```" in raw,
            }, ensure_ascii=False) + "\n")
            fh.flush()
            if err:
                print(f"  {r['id']}: {err}", flush=True)
            if i % 50 == 0:
                print(f"  {label}: {i}/{len(notes)}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--gold", default="../data/corpora/v4/gold_mid.jsonl")
    ap.add_argument("--n", type=int, default=400)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--against", default="v4", choices=sorted(TAILS))
    ap.add_argument("--max-tokens", type=int, default=8192)
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--outdir", required=True)
    args = ap.parse_args()

    prompt.verify_against_source()
    live = prompt.system_prompt()
    if ANCHOR not in live:
        raise SystemExit(f"anchor {ANCHOR!r} gone from the template")
    stem = live[: live.index(ANCHOR) + len(ANCHOR)]

    outdir = pathlib.Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    notes = sample(args.gold, args.n, args.seed)

    # The sampled gold is written out so score.py scores exactly these notes.
    gold_out = outdir / "gold_sample.jsonl"
    with open(gold_out, "w") as fh:
        for r in notes:
            fh.write(json.dumps(r, ensure_ascii=False) + "\n")

    arms = {args.against: stem + TAILS[args.against],
            prompt.PROMPT_VERSION: live}
    print(f"{len(notes)} notes, model={args.model}, arms={list(arms)}")
    for label, sys_prompt in arms.items():
        print(f"[{label}] {len(sys_prompt)} chars")
        run_arm(notes, sys_prompt, label, args.base_url, args.model, args,
                outdir / f"{args.model}.{label}.pred.jsonl")
    print(f"\ngold sample: {gold_out}")
    print("score each arm with score.py --gold "
          f"{gold_out} --pred {outdir}/{args.model}.<version>.pred.jsonl")


if __name__ == "__main__":
    main()
