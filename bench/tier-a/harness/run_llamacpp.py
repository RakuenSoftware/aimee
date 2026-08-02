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
import time
import urllib.error
import urllib.request

import prompt
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
    sys_prompt = (prompt.system_prompt_no_confidence() if args.no_confidence
                  else prompt.system_prompt())
    rows = [json.loads(l) for l in open(args.gold) if l.strip()]

    with open(args.out, "w") as fh:
        for r in rows:
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
                raw, usage, err, reasoning = "", {}, f"{type(e).__name__}: {e}", ""
            dt = (time.perf_counter() - t0) * 1000

            facts, ok, schema_ok, malformed = extract_json(raw)
            floored = [f for f in facts if f["confidence"] >= CONF_FLOOR]
            fh.write(json.dumps({
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
                "completion_tokens": usage.get("completion_tokens"),
                "prompt_tokens": usage.get("prompt_tokens"),
                "truncated": usage.get("completion_tokens") == args.max_tokens,
                "thinking": bool(args.thinking),
                # Which prompt produced this row. prompt.py's version note is
                # explicit that results taken under different prompt versions are
                # not comparable, and until now the version was recorded nowhere
                # in the output -- so telling a v4 file from a v5 one meant
                # checking the commit date of the directory it sat in.
                "prompt_version": prompt.PROMPT_VERSION,
                "reasoning_chars": len(reasoning),
                # A sample of the reasoning text, not just its length. Without
                # it, "7943 reasoning tokens and no answer" cannot be told apart
                # from "7943 tokens of '?' and no answer" — which is exactly the
                # open question about GLM-4.7-Flash, whose raw /completion output
                # is literal '?' characters. A length is not evidence about what
                # a model produced.
                "reasoning": reasoning[:2000],
            }, ensure_ascii=False) + "\n")
            fh.flush()
            if err:
                print(f"{r['id']}: {err}", flush=True)


if __name__ == "__main__":
    main()
