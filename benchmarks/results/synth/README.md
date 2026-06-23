# Synth/curator benchmark harness

The harness behind [`RESULTS.md`](RESULTS.md). Measures the curator's real job —
**async structured-JSON extraction** — with **sampler-level grammar enforcement** (a
generic-JSON GBNF), because `response_format: json_object` is silently ignored by
llama.cpp b9761 and otherwise confounds the scores (it measures freeform-JSON luck).

## Scripts
- **`synth_bench.py PORT MODEL NAME [CORPUS]`** — runs the corpus through an
  OpenAI-compatible `/v1/chat/completions` endpoint with grammar-enforced JSON,
  scores doc/code schema conformance + valid-JSON rate + tok/s, and **saves every raw
  extraction** into `res4_<NAME>.json` for downstream re-scoring/judging.
- **`serve_and_bench.sh MODELFILE MODE LABEL`** — driver: starts `llama-server`
  (`-fa on`, GBNF via the harness), waits for health, runs `synth_bench.py`, tears the
  server down. `MODE` = `gpu` (`-ngl 99`) | `cpu` (`-ngl 0`). One model at a time.
- **`rescore_strict.py res4_<NAME>.json`** — re-scores the saved raws with the *strict*
  rubric (a malformed primary artifact costs points + a validity-rate term), which
  de-saturates the lenient schema check. No re-generation needed.

## Corpus (not committed)
The 60-sample corpus is built per-host from the real `~/dev` + `~/gow` workspace (every
aimee-supported language) and is **not** committed — it is workspace-specific and large.
`synth_bench.py` takes the corpus path as `argv[4]`. Each sample is:
```json
{"role":"extract_doc","lang":"Markdown","project":"...","input":{"file_path","heading_path","content"}}
{"role":"extract_code","lang":"Rust","project":"...","input":{"file_path","symbol","line","kind","body"}}
```

## Content judge
The blind content judge (faithfulness / completeness / structure over a fixed
one-per-language + docs subset) is run by hand from the saved `res4_*.json` raws; see
`RESULTS.md` for methodology and scores. Paths in `serve_and_bench.sh` reflect the
bench-box layout (`/mnt/media/synthbench`).
