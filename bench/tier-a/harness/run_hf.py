"""Run the Tier-A extraction task through a transformers model and record predictions.

Greedy decoding throughout: Tier-A is mechanical extraction, production sets
disable_thinking, and a benchmark needs to be reproducible. One note per
generation — no batching — so latency figures reflect the per-call cost the drain
actually pays.
"""

import argparse
import json
import re
import time

import torch
from transformers import AutoModelForCausalLM, AutoTokenizer

import prompt


def extract_json(text):
    """Recover the facts object from model output.

    Mirrors the tolerance the C provider path has to grow in practice: models wrap
    JSON in fences or leading prose. Returns (facts_list, parse_ok).
    """
    if not text:
        return [], False
    fence = re.search(r"```(?:json)?\s*(.*?)```", text, re.S)
    if fence:
        text = fence.group(1)
    # First balanced {...} run.
    start = text.find("{")
    while start != -1:
        depth, in_str, esc = 0, False, False
        for i in range(start, len(text)):
            ch = text[i]
            if esc:
                esc = False
                continue
            if ch == "\\" and in_str:
                esc = True
                continue
            if ch == '"':
                in_str = not in_str
                continue
            if in_str:
                continue
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    try:
                        obj = json.loads(text[start:i + 1])
                    except json.JSONDecodeError:
                        break
                    facts = obj.get("facts")
                    if isinstance(facts, list):
                        return [f for f in facts if isinstance(f, dict)], True
                    return [], True
        start = text.find("{", start + 1)
    return [], False


def build_inputs(tok, note, model_id):
    msgs = [
        {"role": "system", "content": prompt.system_prompt()},
        {"role": "user", "content": prompt.user_message(note)},
    ]
    kwargs = {}
    # Qwen3 exposes thinking mode through the chat template; Tier-A disables it.
    if "qwen3" in model_id.lower():
        kwargs["enable_thinking"] = False
    try:
        return tok.apply_chat_template(msgs, add_generation_prompt=True,
                                       tokenize=False, **kwargs)
    except TypeError:
        return tok.apply_chat_template(msgs, add_generation_prompt=True, tokenize=False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--gold", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--device", default="cuda")
    ap.add_argument("--max-new-tokens", type=int, default=512,
                    help="production cap is 8192; 512 is ample for this schema and "
                         "bounds a runaway small model. Truncation is recorded.")
    ap.add_argument("--dtype", default="bfloat16")
    args = ap.parse_args()

    prompt.verify_against_source()
    rows = [json.loads(l) for l in open(args.gold) if l.strip()]

    tok = AutoTokenizer.from_pretrained(args.model)
    model = AutoModelForCausalLM.from_pretrained(
        args.model,
        dtype=getattr(torch, args.dtype),
        device_map=args.device if args.device != "cpu" else None,
    )
    model.eval()
    if args.device == "cpu":
        model.to("cpu")

    load_mem = (torch.cuda.max_memory_allocated() / 2**30) if args.device == "cuda" else None

    with open(args.out, "w") as fh:
        for r in rows:
            text = build_inputs(tok, r["note"], args.model)
            enc = tok(text, return_tensors="pt").to(model.device)
            t0 = time.perf_counter()
            with torch.no_grad():
                out = model.generate(
                    **enc,
                    max_new_tokens=args.max_new_tokens,
                    do_sample=False,
                    pad_token_id=tok.pad_token_id or tok.eos_token_id,
                )
            if args.device == "cuda":
                torch.cuda.synchronize()
            dt = (time.perf_counter() - t0) * 1000
            gen = out[0][enc["input_ids"].shape[1]:]
            raw = tok.decode(gen, skip_special_tokens=True)
            facts, ok = extract_json(raw)
            fh.write(json.dumps({
                "id": r["id"],
                "model": args.model,
                "pred": facts,
                "parse_ok": ok,
                "raw": raw[:4000],
                "latency_ms": round(dt, 1),
                "completion_tokens": int(gen.shape[0]),
                "truncated": int(gen.shape[0]) >= args.max_new_tokens,
                "prompt_tokens": int(enc["input_ids"].shape[1]),
            }, ensure_ascii=False) + "\n")
            fh.flush()

    if load_mem is not None:
        print(json.dumps({"peak_vram_gib": round(
            torch.cuda.max_memory_allocated() / 2**30, 2)}))


if __name__ == "__main__":
    main()
