"""Does serving N sequences at once change what the model answers?

Parallel slots are the obvious throughput win: at batch=1 the weights are read
once per TOKEN, so the GPU is bandwidth-bound and idle on compute; batching
reads them once per BATCH. Even 2x turns a 40-minute arm into 20.

The reason it is not simply switched on is that batch composition changes the
order the matmuls are reduced in, float addition is not associative, and greedy
decoding turns a near-tie into a different token. An earlier 20-note check
measured 1.83x with outputs changed on 3 notes, and that killed it for a
benchmark trying to resolve ~0.01 F1.

That check was 20 notes on a different lane and llama.cpp build, and it was never
re-run per model or quant. This re-runs it properly: the SAME notes, same server,
sequential first and then parallel, compared byte-for-byte on the raw completion.

Verdict is deliberately strict. Identical means identical: if a single note's
text differs, parallel decoding is not a free speedup on this model and any arm
run with it is measuring something slightly different from the arms that were
not. Speedup is reported either way, so the tradeoff is visible rather than
assumed.
"""

import argparse
import json
import statistics
import time
from concurrent.futures import ThreadPoolExecutor

import prompt
import prompt_versions
from run_llamacpp import complete


def one(base_url, model, sys_prompt, note, max_tokens, timeout):
    t0 = time.perf_counter()
    resp = complete(base_url, model, sys_prompt, note, max_tokens, timeout, thinking=True)
    msg = resp["choices"][0]["message"]
    return {
        "raw": msg.get("content") or "",
        "reasoning_chars": len(msg.get("reasoning_content") or ""),
        "completion_tokens": (resp.get("usage") or {}).get("completion_tokens"),
        "ms": (time.perf_counter() - t0) * 1000,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--gold", required=True)
    ap.add_argument("--n", type=int, default=48)
    ap.add_argument("--concurrency", type=int, default=32)
    ap.add_argument("--max-tokens", type=int, default=8192)
    ap.add_argument("--timeout", type=float, default=900)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    prompt.verify_against_source()
    sys_prompt = prompt_versions.render("live")
    rows = [json.loads(l) for l in open(args.gold) if l.strip()][: args.n]
    print(f"{len(rows)} notes | model={args.model} | concurrency under test={args.concurrency}\n")

    t0 = time.perf_counter()
    seq = [one(args.base_url, args.model, sys_prompt, r["note"], args.max_tokens, args.timeout)
           for r in rows]
    seq_s = time.perf_counter() - t0
    print(f"sequential : {seq_s:7.1f}s  ({len(rows)/seq_s*60:5.1f} notes/min)")

    t0 = time.perf_counter()
    with ThreadPoolExecutor(max_workers=args.concurrency) as ex:
        futs = [ex.submit(one, args.base_url, args.model, sys_prompt, r["note"],
                          args.max_tokens, args.timeout) for r in rows]
        par = [f.result() for f in futs]
    par_s = time.perf_counter() - t0
    print(f"parallel   : {par_s:7.1f}s  ({len(rows)/par_s*60:5.1f} notes/min)"
          f"   speedup {seq_s/par_s:.2f}x\n")

    same = sum(1 for a, b in zip(seq, par) if a["raw"] == b["raw"])
    difftok = sum(1 for a, b in zip(seq, par)
                  if a["completion_tokens"] != b["completion_tokens"])
    print(f"identical raw completions : {same}/{len(rows)}")
    print(f"differing token counts    : {difftok}/{len(rows)}")
    for a, b, r in zip(seq, par, rows):
        if a["raw"] != b["raw"]:
            print(f"\n  DIFF on {r['id']}: {r['note'][:70]}")
            print(f"    sequential: {a['raw'][:160]}")
            print(f"    parallel  : {b['raw'][:160]}")
            break

    if args.out:
        json.dump({"n": len(rows), "concurrency": args.concurrency,
                   "sequential_s": round(seq_s, 1), "parallel_s": round(par_s, 1),
                   "speedup": round(seq_s / par_s, 3), "identical": same,
                   "differing_token_counts": difftok,
                   "median_ms_sequential": round(statistics.median(a["ms"] for a in seq), 1),
                   "median_ms_parallel": round(statistics.median(b["ms"] for b in par), 1)},
                  open(args.out, "w"), indent=2)

    print()
    if same == len(rows):
        print(f"VERDICT: identical on every note at concurrency {args.concurrency}. "
              f"Parallel decoding is a free {seq_s/par_s:.2f}x on this model+quant.")
    else:
        print(f"VERDICT: {len(rows)-same} note(s) changed. A run at this concurrency is NOT "
              f"comparable to a sequential one; the {seq_s/par_s:.2f}x costs output fidelity.")


if __name__ == "__main__":
    main()
