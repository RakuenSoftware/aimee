"""Run the Tier-B synthesize task against a llama.cpp server.

Same call path as the Tier-A llama.cpp runner and the same reason: the KB talks
to an OpenAI-compatible endpoint, so this exercises the request shape
kb_curator_llm_run actually sends.

Tier-B does NOT set disable_thinking — kb_curator_provider_for_stage sets it only
for Tier-A — so thinking is left enabled here, matching production.
"""

import argparse
import json
import time
import urllib.error
import urllib.request
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).parent))
import prompt_b


def complete(base_url, model, sys_prompt, user_msg, max_tokens, timeout):
    body = json.dumps({
        "model": model,
        "messages": [{"role": "system", "content": sys_prompt},
                     {"role": "user", "content": user_msg}],
        "temperature": 0,
        "top_p": 1,
        "max_tokens": max_tokens,
        "stream": False,
    }).encode()
    req = urllib.request.Request(f"{base_url.rstrip('/')}/v1/chat/completions",
                                 data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", default="http://127.0.0.1:8080")
    ap.add_argument("--model", required=True)
    ap.add_argument("--topics", required=True)
    ap.add_argument("--out", required=True)
    # CURATOR_SYNTH_OUTBUF is 16384; a synthesis paragraph needs far less, and a
    # bound keeps a runaway model from stalling a CPU run for an hour.
    ap.add_argument("--max-tokens", type=int, default=1024)
    ap.add_argument("--timeout", type=float, default=1800)
    args = ap.parse_args()

    prompt_b.verify_against_source()
    rows = [json.loads(l) for l in open(args.topics) if l.strip()]

    with open(args.out, "w") as fh:
        for r in rows:
            user = prompt_b.user_message(r["topic"]["id"], r["topic"]["name"], r["sources"])
            t0 = time.perf_counter()
            try:
                resp = complete(args.base_url, args.model, prompt_b.SYSTEM_PROMPT,
                                user, args.max_tokens, args.timeout)
                msg = resp["choices"][0]["message"]
                raw = msg.get("content") or ""
                usage = resp.get("usage") or {}
                err = None
            except (urllib.error.URLError, KeyError, TimeoutError, OSError) as e:
                raw, usage, err = "", {}, f"{type(e).__name__}: {e}"
            dt = (time.perf_counter() - t0) * 1000

            fh.write(json.dumps({
                "id": r["id"],
                "model": args.model,
                "raw": raw[:8000],
                "error": err,
                "latency_ms": round(dt, 1),
                "prompt_tokens": usage.get("prompt_tokens"),
                "completion_tokens": usage.get("completion_tokens"),
                "truncated": usage.get("completion_tokens") == args.max_tokens,
            }, ensure_ascii=False) + "\n")
            fh.flush()
            print(f"{r['id']} {dt/1000:.1f}s "
                  f"{usage.get('prompt_tokens')}p/{usage.get('completion_tokens')}c"
                  f"{' ERR ' + err if err else ''}", flush=True)


if __name__ == "__main__":
    main()
