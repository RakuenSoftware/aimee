"""Measure real drain throughput on CPU: concurrent requests, warm prefix.

llama-bench reports single-sequence token rates. A drain runs a queue, so what
matters is notes per hour with the server free to batch, and with the system
prompt already in the KV cache from the previous note.

Both of those are invisible to the earlier measurement, which processed one note
at a time and re-ingested the full prompt every call.
"""
import argparse
import concurrent.futures as cf
import json
import time
import urllib.request
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).parent))
import prompt


def one(base_url, model, sys_prompt, note, max_tokens, timeout):
    body = json.dumps({
        "model": model,
        "messages": [{"role": "system", "content": sys_prompt},
                     {"role": "user", "content": prompt.user_message(note)}],
        "temperature": 0, "max_tokens": max_tokens, "stream": False,
        "chat_template_kwargs": {"enable_thinking": False},
    }).encode()
    req = urllib.request.Request(f"{base_url}/v1/chat/completions", data=body,
                                 headers={"Content-Type": "application/json"})
    t0 = time.perf_counter()
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = json.loads(r.read())
    return time.perf_counter() - t0, (d.get("usage") or {}).get("completion_tokens", 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", default="http://127.0.0.1:8090")
    ap.add_argument("--model", required=True)
    ap.add_argument("--gold", required=True)
    ap.add_argument("--concurrency", type=int, nargs="+", default=[1, 4, 8])
    ap.add_argument("--max-tokens", type=int, default=128)
    ap.add_argument("--timeout", type=float, default=900)
    ap.add_argument("--out")
    args = ap.parse_args()

    sys_prompt = prompt.system_prompt()
    notes = [json.loads(l)["note"] for l in open(args.gold) if l.strip()]

    # One warm-up pass so the shared system prompt is already in the KV cache,
    # which is the steady state a drain actually runs in.
    one(args.base_url, args.model, sys_prompt, notes[0], args.max_tokens, args.timeout)

    results = []
    for c in args.concurrency:
        t0 = time.perf_counter()
        with cf.ThreadPoolExecutor(max_workers=c) as ex:
            futs = [ex.submit(one, args.base_url, args.model, sys_prompt, n,
                              args.max_tokens, args.timeout) for n in notes]
            done = [f.result() for f in futs]
        wall = time.perf_counter() - t0
        per_note = wall / len(notes)
        results.append({
            "concurrency": c,
            "notes": len(notes),
            "wall_s": round(wall, 1),
            "s_per_note": round(per_note, 2),
            "notes_per_hour": round(3600 / per_note),
            "median_request_s": round(sorted(d for d, _ in done)[len(done) // 2], 2),
        })
        print(json.dumps(results[-1]), flush=True)

    if args.out:
        Path(args.out).write_text(json.dumps(
            {"model": args.model, "runs": results}, indent=2) + "\n")


if __name__ == "__main__":
    main()
