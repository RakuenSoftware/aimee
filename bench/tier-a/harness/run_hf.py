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


# Production drops any fact below this confidence (MF_CONF_FLOOR).
CONF_FLOOR = 0.6


def extract_json(text):
    """Mirror mf_commit_facts() in src/kb/kb_memory_facts.c exactly.

    Production takes the span from the first '{' to the LAST '}', parses it,
    requires a "facts" array, and drops any fact with an empty subject, relation
    or object, or confidence below MF_CONF_FLOOR. Anything that model output does
    NOT survive here would commit nothing in the drain, so the benchmark must
    apply the same filter or it measures a system we do not run.

    Returns (facts, parse_ok, schema_ok, malformed) where parse_ok means the span
    was valid JSON and schema_ok means it carried a "facts" array. Those are
    reported separately: a model that emits valid JSON of the wrong shape commits
    nothing, but it has not *abstained* — conflating the two would flatter it on
    the empty-gold notes.

    The confidence floor is NOT applied here. It is applied by the caller so the
    same run yields both a production-faithful score and a floor-free one. That
    split matters: the prompt's own schema example contains the literal
    "confidence":0.0, and small models copy it, so a model can extract a fact
    perfectly and still have production discard it at MF_CONF_FLOOR. Scoring only
    the floored view would report that as an extraction failure.
    """
    if not text:
        return [], False, False, 0
    start, end = text.find("{"), text.rfind("}")
    if start == -1 or end < start:
        return [], False, False, 0
    try:
        obj = json.loads(text[start:end + 1])
    except json.JSONDecodeError:
        return [], False, False, 0
    if not isinstance(obj, dict) or not isinstance(obj.get("facts"), list):
        return [], True, False, 0
    kept, malformed = [], 0
    for f in obj["facts"]:
        if not isinstance(f, dict):
            malformed += 1
            continue
        s, r, o = (f.get(k) if isinstance(f.get(k), str) else "" for k in
                   ("subject", "relation", "object"))
        c = f.get("confidence")
        c = float(c) if isinstance(c, (int, float)) else 0.0
        if not s or not r or not o:
            malformed += 1
            continue
        kept.append({"subject": s, "relation": r, "object": o, "confidence": c})
    return kept, True, True, malformed


def build_inputs(tok, note, model_id, sys_prompt):
    msgs = [
        {"role": "system", "content": sys_prompt},
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
    ap.add_argument("--no-kv-cache", action="store_true",
                    help="disable the KV cache. Needed for granite-4.0-350m, where "
                         "transformers selects a hybrid Mamba cache the non-hybrid "
                         "checkpoint cannot satisfy. Slower, identical outputs.")
    ap.add_argument("--repetition-penalty", type=float, default=None,
                    help="DIAGNOSTIC: production sets none. Used only to test "
                         "whether a model's repetition loop is rescuable.")
    ap.add_argument("--signature-prompt", action="store_true",
                    help="REJECTED EXPERIMENT: send type signatures alongside "
                         "predicate names. Regressed 4/5 models; kept reproducible.")
    ap.add_argument("--conf-fixed-prompt", action="store_true",
                    help="ABLATION: send the production prompt with the schema "
                         "example's confidence literal raised from 0.0 to 0.9. "
                         "Not what production sends.")
    args = ap.parse_args()

    prompt.verify_against_source()
    sys_prompt = (prompt.system_prompt_conf_fixed() if args.conf_fixed_prompt
                  else prompt.system_prompt_with_signatures() if args.signature_prompt
                  else prompt.system_prompt())
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
    no_cache = False  # set if the first generate() proves the KV cache unusable

    with open(args.out, "w") as fh:
        for r in rows:
            text = build_inputs(tok, r["note"], args.model, sys_prompt)
            enc = tok(text, return_tensors="pt").to(model.device)
            gen_kwargs = dict(
                max_new_tokens=args.max_new_tokens,
                do_sample=False,
                pad_token_id=tok.pad_token_id or tok.eos_token_id,
                **({"repetition_penalty": args.repetition_penalty}
                   if args.repetition_penalty else {}),
            )
            t0 = time.perf_counter()
            with torch.no_grad():
                try:
                    out = model.generate(**enc, use_cache=not (args.no_kv_cache or no_cache),
                                         **gen_kwargs)
                except ValueError as e:
                    # Several Granite 4.0 checkpoints are non-hybrid but transformers
                    # selects a hybrid Mamba cache for them, so generate() raises.
                    # Retry once without the cache — same outputs, just slower —
                    # rather than special-casing model ids in every sweep script.
                    if "LinearAttention" not in str(e):
                        raise
                    no_cache = True
                    print(f"note: KV cache unusable for {args.model}; retrying without it",
                          flush=True)
                    out = model.generate(**enc, use_cache=False, **gen_kwargs)
            if args.device == "cuda":
                torch.cuda.synchronize()
            dt = (time.perf_counter() - t0) * 1000
            gen = out[0][enc["input_ids"].shape[1]:]
            raw = tok.decode(gen, skip_special_tokens=True)
            facts, ok, schema_ok, malformed = extract_json(raw)
            floored = [f for f in facts if f["confidence"] >= CONF_FLOOR]
            fh.write(json.dumps({
                "id": r["id"],
                "model": args.model,
                # pred is what production would commit; pred_nofloor is the same
                # extraction with MF_CONF_FLOOR lifted.
                "pred": floored,
                "pred_nofloor": facts,
                "parse_ok": ok,
                "schema_ok": schema_ok,
                "malformed_facts": malformed,
                "dropped_by_conf_floor": len(facts) - len(floored),
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
