# Synthesizer / curator model benchmark (2026-06, grammar-enforced + judged)

The curator/synth LLM does **async structured-JSON extraction** (`scripts/curator-extract.py`:
doc → `{artifacts:[doc_summary, claim, entity]}`; code → `{artifacts:[code_unit]}`) over an
OpenAI-compatible `/v1/chat/completions` endpoint. It is **throughput-bound** (drains an
extraction queue), not latency-bound. So the axes are: **reliable structured JSON +
schema/content fidelity + tok/s**.

## Why this was redone (the first pass was confounded)
The original harness scored on `response_format: json_object`, which **llama.cpp b9761
silently ignores** — models emitted arbitrary JSON and the metric measured *freeform-JSON
luck, not quality*. It produced false findings ("every Qwen slips structurally", "granite-3b
regressed", "8B beats 32B"). Proven: granite-3b's raw-parse was 0.5 under `json_object`;
forcing a **generic-JSON GBNF grammar at the sampler** took it to 1.0 and doc-schema
0.167 → 0.833. Every old number was discarded and the sweep rerun grammar-enforced.

## Method
- **Corpus** (`synth_corpus_full.json`, 60 samples) drawn from the **real ~/dev + ~/gow
  workspace**: 24 doc chunks across 24 projects + 36 code units covering **every
  aimee-supported language** — C, C++, Rust, Go, Python, TypeScript, JavaScript, Java, C#,
  Ruby, Bash, Swift, Kotlin, Scala — plus CSS, HTML, Markdown. (Swift/Kotlin/Scala
  synthesized; the workspace has none.)
- **Grammar-enforced** JSON (sampler GBNF) so validity is guaranteed; the score measures
  content/structure, not luck.
- **Three uniform layers, identical for every model:**
  1. **Strict schema rubric** (`rescore_strict.py`) — a malformed *primary* artifact
     (doc_summary / code_unit) costs points, plus a validity-rate term. The lenient rubric
     saturated at 1.0 even for the 4B model, so it can't rank; the strict one de-saturates.
  2. **Corpus-wide valid-JSON rate** — exposes truncation/reliability (a model that runs
     long and gets cut off mid-object).
  3. **Content judge** — Claude (Opus 4.8), blind, fixed 22-item set (one per language + 6
     docs), scoring faithfulness / completeness / structure (0–2 each → 0–1).
- 7900XTX, Q4_K_M, llama.cpp b9761, `-fa on`, **one model at a time**.

## Results — primary sweep (current-gen, 9 distinct models)

| model | judge | strict doc | strict code | valid JSON | tok/s | VRAM |
|---|---|---|---|---|---|---|
| **gemma-4-12B** | 0.98 | 1.00 | 1.00 | 1.00 | 53 | ~8 GB |
| gemma-4-26B-A4B (MoE) | 0.99 | 1.00 | 1.00 | 1.00 | 84 | ~17 GB |
| gemma-4-31B | 1.00 | 1.00 | 1.00 | 1.00 | 27 | ~19 GB |
| granite-4.1-8B | 0.95 | 0.94 | 1.00 | 1.00 | 88 | ~6 GB |
| granite-4.1-30B | 0.99 | 0.95 | 1.00 | 0.98 | 32 | ~17 GB |
| qwen3.6-35B-A3B | 0.99 | 0.88 | 1.00 | 0.92 | 89 | ~21 GB |
| qwen3.6-27B | 1.00 | 0.79 | 1.00 | 0.88 | 32 | ~16 GB |
| **gemma-4-E4B** | 0.91 | 0.64 | 1.00 | 1.00 | 95 / 7.5cpu | ~5 GB |
| granite-4.1-3B | 0.80 | 0.79 | 0.89 | 0.95 | 140 / 10.5cpu | ~2 GB |

## Results — CPU tier: 2026 ≤4B bake-off
The user asked whether any current (2026) ≤4B model beats gemma-4-E4B for the CPU tier.
Seven were run through the identical pipeline (same corpus, grammar, strict rubric, blind
judge):

| model | params | judge | strict doc | strict code | valid JSON | tok/s | verdict |
|---|---|---|---|---|---|---|---|
| **gemma-4-E4B** *(incumbent)* | 4B(E) | **0.91** | 0.64 | **1.00** | **1.00** | 95 | **keep** |
| granite-4.1-3B | 3B | 0.80 | 0.79 | 0.89 | 0.95 | 140 | runner-up |
| smollm3-3b | 3B | 0.77 | **0.83** | 0.74 | 0.92 | 165 | doc-heavy only |
| nemotron-3-nano-4b | 4B | 0.64 | 0.72 | 0.90 | 1.00 | 124 | reject |
| granite-4.0-h-micro | 3.2B | — | 0.59 | 0.83 | 0.98 | 112 | reject |
| granite-4.0-micro | 3.4B | — | 0.40 | 0.99 | 1.00 | 143 | reject (doc) |
| phi-4-mini | 3.8B | — | 0.30 | 0.97 | 0.97 | 140 | reject (doc) |
| lfm2.5-8b-a1b | 8B/1A | — | 0.19 | 0.28 | 1.00 | 225 | reject |
| lfm2.5-1.2b | 1.2B | — | 0.02 | 0.22 | 1.00 | 347 | reject |

**No 2026 small model beats gemma-4-E4B.** Each trades gemma's one weakness (doc-flattening)
for a worse one: smollm3 has the best doc structure of any ≤4B model but its code
**echoes the schema's placeholder strings** (`"assumptions/guarantees or empty"`, `["concept"]`)
and truncates ~8%; nemotron **malforms docs** (claim/entity as sibling keys) and also
placeholder-echoes code; phi-4-mini and granite-4.0-micro have great code but doc 0.30/0.40;
both LFM2.5 are too weak to structure. gemma-4-E4B's perfect code + 100% reliability + 0.91
content carry it.

## Findings
- **Faithfulness is universal** among capable models under grammar enforcement — no
  hallucination on extraction. The differentiators are **structure** and **reliability**.
- **gemma-4-E4B flattens `doc_summary`** to a string (strict-doc 0.64); the larger gemma-4
  variants do not (1.00).
- **Qwen3.6 is content-capable but reliability-risky** — 8–12% of outputs truncate even
  under grammar (valid-JSON 0.88–0.92); ~1-in-10 lost extractions for an async curator.
- **MoE for throughput:** gemma-4-26B-A4B activates ~4B of 26B params/token → faster decode
  than the dense 12B (84 vs 53 tok/s) despite a larger resident footprint.

## Decision (operator)
Quality is a three-way tie across gemma-4 12B/26B/31B (all perfect strict + valid-JSON; judge
deltas sub-noise); the pick splits on **deployment VRAM**, because the *unified* container
co-hosts the embedder + reranker on the same GPU:
- **Unified GPU (shared) → gemma-4-12B** — best quality-per-VRAM (~8 GB co-resides cleanly);
  53 tok/s is ample for a queue drain.
- **Dedicated synth GPU → gemma-4-26B-A4B** — best quality-per-second when VRAM isn't
  contended.
- **CPU → gemma-4-E4B** — best ≤4B option (empirically, vs. the 2026 field); doc-flattening
  is its one weakness. `smollm3-3b` is the alternative for doc-heavy / code-light workloads.
- **Dropped:** dense gemma-4-31B (dominated), Qwen3.6 (reliability), granite-4.1-3B and all
  other ≤4B candidates (weaker on the combined task).

Completes the model-selection trilogy: embedder (#624), reranker (#628), synth (here).
