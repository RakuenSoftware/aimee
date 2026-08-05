"""Run the Tier-A extraction task against a llama.cpp server.

Two reasons this exists alongside run_hf.py:

1. MoE offload. A 26B/3.8B-active or 35B/3B-active model does not fit 15.5GB of
   VRAM, and transformers' offload handles that badly — three quantised attempts
   failed outright and bf16 offload ran at 74s a note. llama.cpp can pin
   attention and shared weights to the GPU and route only the expert FFN tensors
   to CPU, which is the split MoE was designed for.

2. It is closer to production. The KB calls an OpenAI-compatible endpoint, so
   this path exercises the same request shape kb_curator_llm_run does, rather
   than an in-process generate().

Changing runtime is a confound, so the sweep runs E4B through here as a control
against its transformers result — the same discipline the NF4 control used.
"""

import argparse
import json
import threading
import time
from concurrent.futures import ThreadPoolExecutor
import urllib.error
import urllib.request

import prompt
import prompt_versions
from run_hf import CONF_FLOOR, extract_json


def complete(base_url, model, sys_prompt, note, max_tokens, timeout, thinking=False):
    """One chat completion. Greedy, matching the transformers runner."""
    body = json.dumps({
        "model": model,
        "messages": [
            {"role": "system", "content": sys_prompt},
            {"role": "user", "content": prompt.user_message(note)},
        ],
        "temperature": 0,
        "top_p": 1,
        "max_tokens": max_tokens,
        "stream": False,
        # Tier-A sets disable_thinking in production; the flag lets us test
        # whether suppressing reasoning is costing it.
        "chat_template_kwargs": {"enable_thinking": bool(thinking)},
    }).encode()
    req = urllib.request.Request(
        f"{base_url.rstrip('/')}/v1/chat/completions", data=body,
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", default="http://127.0.0.1:8080")
    ap.add_argument("--model", required=True, help="label recorded in the results")
    ap.add_argument("--gold", required=True)
    ap.add_argument("--out", required=True)
    # Defaults are PRODUCTION's, so a caller that forgets a flag measures the
    # shipped system rather than a quieter one. This used to default to 512, a
    # sixteenth of MF_LLM_OUT_CAP, and two separate sweeps silently inherited it:
    # sweep_challenger_254.sh truncated Olmo-3.1-32B-Think on 59 of 70 notes and
    # nothing said so until the scorer learned to refuse truncated rows.
    ap.add_argument("--max-tokens", type=int, default=8192,
                    help="matches MF_LLM_OUT_CAP in src/kb/kb_memory_facts.c")
    # 600s was too low for any model that does not fit the card. gemma-4-31B
    # timed out on 12 of 70 notes and Qwen3.6-27B on 60 of 70, both dense at Q8_0
    # against a 16GB card, so llama.cpp served them from CPU at ~2 tok/s. Their
    # median latencies were 276s and 600s: the bound was firing on the models it
    # most needed not to fire on, and score.py correctly refused both runs after
    # they had each consumed hours.
    #
    # 3600s is not a real limit, it is a hang detector. A note that takes an hour
    # is a broken configuration, and the run should still be recorded rather than
    # hanging the sweep forever.
    ap.add_argument("--timeout", type=float, default=3600)
    ap.add_argument("--no-confidence", action="store_true",
                    help="ABLATION: drop the confidence field from the schema.")
    # Older prompts are DERIVED from the live template (see prompt_versions.py)
    # rather than kept as second copies, so a comparison across versions cannot
    # quietly diverge from what production sends.
    # Parallel slots. llama-server serves several sequences at once and the
    # weights are read ONCE per batch instead of once per sequence, so at batch=1
    # the GPU is memory-bandwidth-bound and mostly idle on compute. Concurrency
    # trades that idle compute for throughput.
    #
    # It is NOT free for a benchmark: batch composition changes the order of the
    # matmuls, float addition is not associative, and greedy decoding turns a
    # near-tie into a different token. An earlier 20-note check measured 1.83x
    # with outputs changed on 3 notes. Whether that still holds is testable per
    # model and quant -- see check_parallel_determinism.py -- so this defaults to
    # 1 and must be asked for.
    ap.add_argument("--concurrency", type=int, default=1,
                    help="parallel in-flight requests (1 = strictly sequential, "
                         "the only setting proven not to perturb outputs)")
    ap.add_argument("--prompt-version", default="live",
                    help="'live' (default, the shipped prompt) or an older version "
                         "reconstructed by prompt_versions.py, e.g. v5. Recorded on "
                         "every row.")
    # Thinking has no default at all: it must be stated. It is worth +0.09 F1 to
    # gemma-4-E4B and it is the single largest effect measured on this benchmark,
    # so a run that does not record which side of it was taken is not
    # interpretable. It was previously a bare store_true, which meant "off"
    # looked identical to "not considered".
    think = ap.add_mutually_exclusive_group(required=True)
    think.add_argument("--thinking", dest="thinking", action="store_true",
                       help="enable_thinking=true, which is what production does "
                            "now: kb_curator_provider.c stopped setting "
                            "disable_thinking after it measured 0.738 -> 0.828 on "
                            "gemma-4-E4B.")
    think.add_argument("--no-thinking", dest="thinking", action="store_false",
                       help="ABLATION: the retired disable_thinking behaviour.")
    args = ap.parse_args()

    prompt.verify_against_source()
    if args.no_confidence:
        if args.prompt_version != "live":
            raise SystemExit("--no-confidence is an ablation on the live prompt only")
        sys_prompt = prompt.system_prompt_no_confidence()
    else:
        sys_prompt = prompt_versions.render(args.prompt_version)
    version_label = (prompt.PROMPT_VERSION if args.prompt_version == "live"
                     else args.prompt_version)
    rows = [json.loads(l) for l in open(args.gold) if l.strip()]

    def run_one(r):
        """One note -> one output row. Pure apart from the HTTP call, so several
        can be in flight at once without sharing state."""
        t0 = time.perf_counter()
        try:
            resp = complete(args.base_url, args.model, sys_prompt, r["note"],
                            args.max_tokens, args.timeout, args.thinking)
            msg = resp["choices"][0]["message"]
            raw = msg.get("content") or ""
            reasoning = msg.get("reasoning_content") or ""
            usage = resp.get("usage") or {}
            err = None
        except (urllib.error.URLError, KeyError, TimeoutError, OSError) as e:
            # Retry TRANSPORT failures. A dropped SSH tunnel is not a property of
            # the model, but with no retry every request during the outage became
            # a permanent errored row: the E2B Q4 10k arm lost 1,644 rows (16%,
            # all on shard 0) to one tunnel that died and stayed dead, while the
            # server on the far side kept serving normally throughout.
            #
            # Only transport errors are retried. A KeyError means the response
            # came back and was the wrong shape, which retrying cannot fix.
            raw, usage, err, reasoning = "", {}, f"{type(e).__name__}: {e}", ""
            if isinstance(e, (urllib.error.URLError, TimeoutError, OSError)):
                for attempt in range(6):
                    time.sleep(min(2 ** attempt, 30))
                    try:
                        resp = complete(args.base_url, args.model, sys_prompt,
                                        r["note"], args.max_tokens, args.timeout,
                                        args.thinking)
                        msg = resp["choices"][0]["message"]
                        raw = msg.get("content") or ""
                        reasoning = msg.get("reasoning_content") or ""
                        usage = resp.get("usage") or {}
                        err = None
                        break
                    except (urllib.error.URLError, KeyError, TimeoutError, OSError) as e2:
                        err = f"{type(e2).__name__}: {e2} (after {attempt + 1} retries)"
        dt = (time.perf_counter() - t0) * 1000

        facts, ok, schema_ok, malformed = extract_json(raw)
        floored = [f for f in facts if f["confidence"] >= CONF_FLOOR]
        return {
            "id": r["id"],
            "model": args.model,
            "runtime": "llama.cpp",
            "pred": floored,
            "pred_nofloor": facts,
            "parse_ok": ok,
            "schema_ok": schema_ok,
            "malformed_facts": malformed,
            "dropped_by_conf_floor": len(facts) - len(floored),
            "raw": raw[:4000],
            "error": err,
            "latency_ms": round(dt, 1),
            # Speculative decoding's speedup IS the draft acceptance rate: the
            # fraction of drafted tokens the target accepts as its own argmax.
            # llama.cpp reports it per request in `timings` as draft_n and
            # draft_n_accepted, and nothing was reading it, so every MTP claim in
            # this project so far is measured through wall clock -- which mixes
            # the mechanism with the host, the model and the backend.
            #
            # Recorded additively. No score changes; absent on non-MTP servers.
            "draft_n": (resp.get("timings") or {}).get("draft_n"),
            "draft_n_accepted": (resp.get("timings") or {}).get("draft_n_accepted"),
            "predicted_per_second": (resp.get("timings") or {}).get("predicted_per_second"),
            "completion_tokens": usage.get("completion_tokens"),
            "prompt_tokens": usage.get("prompt_tokens"),
            "truncated": usage.get("completion_tokens") == args.max_tokens,
            "thinking": bool(args.thinking),
            # Which prompt produced this row. prompt.py's version note is explicit
            # that results under different prompt versions are not comparable, and
            # the version used to be recorded nowhere in the output.
            "prompt_version": version_label,
            # Recorded so a run's parallelism is visible in its own data rather
            # than in whoever remembers how it was launched.
            "concurrency": args.concurrency,
            "reasoning_chars": len(reasoning),
            # A sample of the reasoning text, not just its length: "7943 reasoning
            # tokens and no answer" cannot be told from "7943 tokens of '?'".
            "reasoning": reasoning[:2000],
        }

    # Rows are written IN GOLD ORDER regardless of completion order, and flushed
    # as soon as their turn comes up. Order matters because paired scoring zips
    # two files together, and incremental flushing matters because the driver
    # decides an arm is complete by counting lines.
    with open(args.out, "w") as fh:
        pending, nxt = {}, 0
        def drain():
            nonlocal nxt
            while nxt in pending:
                row = pending.pop(nxt)
                fh.write(json.dumps(row, ensure_ascii=False) + "\n")
                if row["error"]:
                    print(f"{row['id']}: {row['error']}", flush=True)
                nxt += 1
            fh.flush()

        if args.concurrency <= 1:
            for i, r in enumerate(rows):
                pending[i] = run_one(r)
                drain()
        else:
            with ThreadPoolExecutor(max_workers=args.concurrency) as ex:
                # All notes are submitted up front, so max_workers of them are in
                # flight at once; results are then collected in submission order.
                futs = {ex.submit(run_one, r): i for i, r in enumerate(rows)}
                for f, i in list(futs.items()):
                    pending[i] = f.result()
                    drain()


if __name__ == "__main__":
    main()
