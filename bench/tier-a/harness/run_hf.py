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

import prompt

# gemma-4 emits its reasoning on a separate channel and then the answer. The
# llama.cpp server splits those into reasoning_content and content for us; a raw
# transformers decode is one stream, so the split has to happen here. It matters:
# extract_json() takes the span from the first '{' to the LAST '}', mirroring
# mf_commit_facts, and a reasoning block containing a brace would corrupt that
# span and be scored as malformed model output.
# The markers are ASYMMETRIC and that is easy to get wrong: `<|channel>thought`
# OPENS the thought channel and `<channel|>` CLOSES it — the pipe moves from the
# left of the word to the right. Splitting on the opener alone leaves the entire
# chain-of-thought in the answer, and because that text quotes the schema
# (`{"facts":`), extract_json's first-'{'-to-last-'}' span then runs from inside
# the reasoning to the end of the real answer and fails to parse. Observed
# directly: 375-token generations scoring parse_ok=False with reasoning_chars=0.
THOUGHT_OPEN = "<|channel>"
THOUGHT_CLOSE = "<channel|>"
TURN_END = "<turn|>"


def split_reasoning(text):
    """Return (answer, reasoning), mirroring the llama.cpp server's split of
    content from reasoning_content so both runtimes present the same thing to
    the scorer. No close marker means no thought channel — the thinking-off
    case — and the text passes through untouched."""
    text = text or ""
    cut = text.rfind(THOUGHT_CLOSE)
    if cut == -1:
        return text, ""
    reasoning = text[:cut]
    if reasoning.startswith(THOUGHT_OPEN):
        reasoning = reasoning[len(THOUGHT_OPEN):]
        # Remove the channel NAME, as a prefix. str.lstrip("thought") would strip
        # any leading run of t/h/o/u/g/h/t characters, so a reasoning block that
        # opened with "The note..." would lose letters off the front.
        if reasoning.startswith("thought"):
            reasoning = reasoning[len("thought"):]
        reasoning = reasoning.lstrip()
    answer = text[cut + len(THOUGHT_CLOSE):]
    end = answer.find(TURN_END)
    return (answer[:end] if end != -1 else answer), reasoning

# torch and transformers are imported inside main(), not here, because
# run_llamacpp.py imports CONF_FLOOR and extract_json from this module and needs
# neither. At module scope they made the llama.cpp runner depend on a ~2GB GPU
# stack it never calls: the .254 host has no torch, and the challenger control
# died on `ModuleNotFoundError: No module named 'torch'` after downloading 10GB
# of weights. It only ever worked on .253 because that box happens to have torch
# installed for the transformers lane.


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


def build_inputs(tok, note, model_id, sys_prompt, thinking=None):
    msgs = [
        {"role": "system", "content": sys_prompt},
        {"role": "user", "content": prompt.user_message(note)},
    ]
    kwargs = {}
    # Both families expose thinking through the chat template and both DEFAULT IT
    # OFF, so leaving it unset is a choice, not a neutral position. gemma-4's
    # template reads `enable_thinking | default(false)` and, when on, injects a
    # `<|think|>` token into the first system turn. Production does not suppress
    # thinking (kb_curator_provider.c:198), so the benchmark must be able to send
    # it or it measures a configuration we do not ship.
    if thinking is not None:
        kwargs["enable_thinking"] = bool(thinking)
    try:
        return tok.apply_chat_template(msgs, add_generation_prompt=True,
                                       tokenize=False, **kwargs)
    except TypeError:
        return tok.apply_chat_template(msgs, add_generation_prompt=True, tokenize=False)


def main():
    # Imported here: see the note at the top of the file. Only the transformers
    # path needs these, and only main() takes that path.
    global torch, AutoModelForCausalLM, AutoTokenizer
    import torch
    from transformers import AutoModelForCausalLM, AutoTokenizer

    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--gold", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--device", default="cuda")
    ap.add_argument("--max-new-tokens", type=int, default=512,
                    help="production cap is 8192; 512 is ample for this schema and "
                         "bounds a runaway small model. Truncation is recorded.")
    ap.add_argument("--dtype", default="bfloat16")
    think = ap.add_mutually_exclusive_group()
    think.add_argument("--thinking", dest="thinking", action="store_true",
                       help="enable the chat template's thinking mode. Production "
                            "does not suppress thinking, so this is the shipped "
                            "configuration for gemma-4.")
    think.add_argument("--no-thinking", dest="thinking", action="store_false",
                       help="explicitly suppress thinking.")
    ap.set_defaults(thinking=None)
    ap.add_argument("--no-kv-cache", action="store_true",
                    help="disable the KV cache. Needed for granite-4.0-350m, where "
                         "transformers selects a hybrid Mamba cache the non-hybrid "
                         "checkpoint cannot satisfy. Slower, identical outputs.")
    ap.add_argument("--repetition-penalty", type=float, default=None,
                    help="DIAGNOSTIC: production sets none. Used only to test "
                         "whether a model's repetition loop is rescuable.")
    ap.add_argument("--load-4bit", action="store_true",
                    help="NF4 via bitsandbytes, so a model too large for 15.5GB of "
                         "VRAM runs resident instead of offloading to CPU at ~75s a "
                         "note. Quantisation is a confound: only compare a 4-bit run "
                         "against another 4-bit run.")
    ap.add_argument("--gpu-budget", default=None,
                    help="cap GPU allocation (e.g. 13GiB) so accelerate leaves room "
                         "for activations instead of filling VRAM with weights.")
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
    load_kwargs = dict(
        dtype=getattr(torch, args.dtype),
        device_map=args.device if args.device != "cpu" else None,
    )
    if args.load_4bit:
        from transformers import BitsAndBytesConfig
        load_kwargs["quantization_config"] = BitsAndBytesConfig(
            load_in_4bit=True,
            bnb_4bit_quant_type="nf4",
            bnb_4bit_compute_dtype=getattr(torch, args.dtype),
            bnb_4bit_use_double_quant=True,
            # bitsandbytes refuses any CPU/disk dispatch unless this is set, and a
            # 26B/35B at NF4 does not fit 15.5GB, so the spill is unavoidable.
            llm_int8_enable_fp32_cpu_offload=True,
        )
        if args.gpu_budget:
            load_kwargs["max_memory"] = {0: args.gpu_budget, "cpu": "80GiB"}
    model = AutoModelForCausalLM.from_pretrained(args.model, **load_kwargs)
    model.eval()
    if args.device == "cpu":
        model.to("cpu")

    load_mem = (torch.cuda.max_memory_allocated() / 2**30) if args.device == "cuda" else None
    no_cache = False  # set if the first generate() proves the KV cache unusable

    with open(args.out, "w") as fh:
        for r in rows:
            text = build_inputs(tok, r["note"], args.model, sys_prompt, args.thinking)
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
            decoded = tok.decode(gen, skip_special_tokens=False)
            raw, reasoning = split_reasoning(decoded)
            raw = tok.decode(tok(raw, add_special_tokens=False)["input_ids"],
                             skip_special_tokens=True)
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
                "quantization": "nf4" if args.load_4bit else args.dtype,
                # Recorded for the same reason run_llamacpp.py records them: a
                # thinking run that produced no reasoning did not have thinking
                # on, and without this the only symptom is a score that quietly
                # matches the thinking-off arm.
                "thinking": args.thinking,
                "reasoning_chars": len(reasoning),
                "reasoning": reasoning[:1000],
            }, ensure_ascii=False) + "\n")
            fh.flush()

    if load_mem is not None:
        print(json.dumps({"peak_vram_gib": round(
            torch.cuda.max_memory_allocated() / 2**30, 2)}))


if __name__ == "__main__":
    main()
