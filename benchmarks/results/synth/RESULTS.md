# Synthesizer / curator model benchmark (2026-06-22)

The curator/synth LLM does **async structured-JSON extraction** (`scripts/curator-extract.py`:
doc → {entities, claims, doc_summary}; code → {code_unit: summary, side_effects,
domain_concepts}; story-continuity) via an OpenAI-compatible `/chat/completions`
endpoint, parsing the first JSON object. So the model must: produce **reliable
structured JSON**, follow the schema, and keep **throughput** up (queue drain; async,
not latency-bound).

Benchmark: the real `curator-extract` doc + code prompts over 12 samples (6 doc from
aimee docs, 6 code units from `src/`), `temperature=0`, `enable_thinking=false`,
`response_format=json_object`. Scored: schema conformance (doc + code), parse rate,
tok/s. Hardware: 7900XTX (24GB, Vulkan, llama.cpp b9761), Q4_K_M, **one model at a
time**.

| tier | model | doc_schema | code_schema | parse | tok/s | JSON |
|---|---|---|---|---|---|---|
| CPU | **gemma-4-E4B** (current) | 1.0 | 1.0 | 1.0 | 98 | clean |
| CPU/small | granite-4.0-h-tiny | 0.96 | 1.0 | 1.0 | 130 | clean |
| small-GPU | qwen3-4b | 0.41 | 0.17 | 0.33 | 134 | malformed |
| small-GPU | qwen3-8b | 1.0 | 0.33 | 0.67 | 102 | malformed (code) |
| GPU | **granite-4.0-h-small** (32B-A9B) | 1.0 | 1.0 | 1.0 | 64 | clean |
| GPU | Qwen3.6-35B-A3B | 0.33 | 1.0 | 0.67 | 90 | malformed |
| GPU | Qwen3.6-27B (dense) | 0.0 | 1.0 | 0.5 | 33 | malformed; slowest |

## Findings
- **Function-calling-tuned models (Granite, Gemma) produce reliable curator JSON**
  (parse 1.0, schema 0.96–1.0). **Every Qwen model slips structurally** — malformed
  JSON (e.g. an extra closing brace `...}}]}`) on code-heavy content → parse 0.33–0.67,
  despite `response_format=json_object`. This is the curator's exact job (parse JSON),
  so reliability matters.
- **gemma-4-E4B**: perfect (1.0/1.0/1.0), reliable, 98 tok/s.
- **granite-4.0-h-small**: perfect (1.0/1.0/1.0), reliable, 64 tok/s — best GPU pick.
- granite-4.0-h-tiny is the fastest (130 tok/s) and clean — a strong CPU/small option.
- The pinned **Qwen3.6 tiers underperform here** (JSON reliability + the 27B is slowest
  at 33 tok/s).

## Fairness caveat
Qwen's failures are **JSON-validity**, not necessarily content — *when* Qwen parses,
schema can be perfect (qwen3-8b doc=1.0). **Grammar-constrained decoding** (the curator's
planned enhancement, `kb_curator_extract.c:35`) would force valid JSON for all models and
shift the comparison to content quality. A grammar-constrained content re-run is the
definitive tiebreaker if Qwen3.6 is reconsidered.

## Decision (operator, 2026-06-22)
**CPU = gemma-4-E4B; GPU = granite-4.0-h-small.** Both reliable, perfect schema. Drops
the Gemma-12B/26B and Qwen3.6 GPU tiers in favor of a single Granite GPU model.
