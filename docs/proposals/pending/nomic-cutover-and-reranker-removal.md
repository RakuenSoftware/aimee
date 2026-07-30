# Handoff: nomic embedder + prefix support, and reranker removal

- **State:** ✅ **DONE.** All three work items landed on `rewrite/go-server-wfe`:
  prefix support (`8a925c322`, `7e1cce7d6`), the embedder registry that made the
  swap modular (`98f72a58f`), and the reranker removal (`af6bb1f6e` serving side,
  `a5d734205` kb side). Kept as the record of why; the evidence links below are
  the durable part.
- **Evidence:** [retrieval-stack-report](../../validation/retrieval-stack-report-2026-07-30.md),
  [reranker-and-pipeline](../../validation/reranker-and-pipeline-2026-07-29.md),
  raw artifacts in `benchmarks/results/reranker-2026-07-29/`.
- **Related:** [learning-to-rank-from-interactions](learning-to-rank-from-interactions.md),
  [embedder-query-document-prefixes](embedder-query-document-prefixes.md).

---

## What landed beyond the work order

Making the swap *modular* was added to the scope: every per-embedder fact —
coordinates, pooling, width, context, prefixes — now lives in
`scripts/embedders.json`, keyed by model identity. The gateway validates and loads
it; the supervisor evals `--embedder-descriptor` instead of its own `case`
statement. Switching embedders is one entry there plus `AIMEE_LLM_EMBED_MODEL`,
and an unregistered or half-declared model refuses to boot rather than being
served with inherited settings. Two silent failures found along the way: the
embedder was served with a hardcoded `--ctx-size 8192` against nomic's 2048
trained positions, and the query path's builtin fallback declared itself a
document.

## The decisions

1. **Adopt `nomic-embed-text-v2-moe` as the embedder on every tier** (768-dim).
2. **Build per-model query/document prefix support.** This is a *precondition*,
   not a follow-up — see the blocker below.
3. **Remove the reranker entirely.** Not replace: remove.

## ⚠️ The blocker that governs sequencing

Measured on 10,000 queries against the full 26,473-document corpus:

| configuration | NDCG@10 |
| --- | ---: |
| nomic **with** its card prefixes | **0.6075** |
| bekko-a25m (needs no prefix) | 0.5909 |
| nomic **prefix-free — what the code does today** | **0.5823** |

**Shipping nomic without prefix support is a regression**, both against the
alternative embedder and against the number the decision was made on. The
prefix work and the embedder swap must land together, or not at all.

---

## Work item 1 — prefix support

### The hard part

Prefixes require distinguishing a **query** from a **document**, and that
distinction must survive the API boundary. Today the kb calls `/embed` for both
and the gateway cannot tell them apart. So this is not a config change; it needs
an `input_type` (or equivalent) threaded from every call site through to the
embedding request.

### Requirements

- **Per model, not global.** nomic uses `search_query:` / `search_document:`;
  bekko-a25m defines none; Qwen3 uses an instruction sentence containing a
  newline. A single global prefix setting is wrong.
- **Bound to model identity.** A future embedder swap must not silently inherit
  the previous model's prefixes. This is the same failure class as
  `AIMEE_LLM_EMBED_POOLING`, which defaulted to `last` (correct for Qwen3) and is
  silently wrong for nomic — wrong vectors, no error, no dimension change.
- **Fail loudly on mismatch.** Prefer refusing to serve an unknown embedder over
  serving it with empty or inherited prefixes.

### Call sites to trace

`/embed` and `/embed_batch` in the aimee-llm gateway; the kb's
`memory_embed_http_post` path; code embedding (`kb_service_code_embed.c`) and the
three code-unit vectors in `kb_curator_index_code_unit.c` (`intent_vec`,
`sig_vec`, `body_vec` — decide which of these are "documents" and whether
`body_vec` raw code should carry a prose document prefix at all).

### Migration

Prefixes change every vector, so adopting them is a **full re-embed** — same
double-gated path as a dimension change (`aimee kb reembed` /
`db2_dim_change_reset`), even though the dimension is unchanged.

---

## Work item 2 — the nomic swap

Largely already staged in commit `9f2e7439` (present on this branch), which
pinned coordinates and flipped the pooling default. **Review it rather than
redo it**, then correct the parts that were later shown wrong.

### Verified facts (do not re-derive)

| | |
| --- | --- |
| repo / file | `ggml-org/Nomic-Embed-Text-V2-GGUF` / `nomic-embed-text-v2-moe-q8_0.gguf` |
| revision | `498da4a128ed12a423efb6f9b0242dbac80209bf` |
| sha256 | `36c5817bc25f379e62021f49efde05b10ed3b0c93ab8059c43173a7a5de73565` |
| architecture | `nomic-bert-moe`, supported by the pinned `LLAMA_TAG=b9775` — **no runtime bump needed** |
| pooling | **`mean`** (was `last`; silently wrong, produces well-formed bad vectors) |
| dimension | 768, uniform across tiers |
| Q8_0 quality cost | **−0.0037** vs bf16 — acceptable |
| max context | 2048 (`max_trained_positions`); the tokenizer's `model_max_length: 512` is an artifact, not a limit |

### Corrections to that commit's claims

- "Uniform 768-d is an architectural win" — **overstated**. `db2_effective_dim`
  and `db2_dim_change_reset` already handle per-deployment dimensions; uniformity
  saves a re-embed when switching tiers, which is a convenience.
- "2.7x slower on CPU" — **actually 3.6x** (598 tok/s measured through
  llama.cpp Q8_0, vs a25m's 2,155). An intermediate figure of 4,278 tok/s was
  GPU-contaminated and is withdrawn: `-ngl 0` is overridable by llama.cpp's
  auto-fit, so verify VRAM does not grow rather than trusting the flag.

---

## Work item 3 — remove the reranker

### Why removal rather than replacement

Across **20 configurations and two embedders**, the best reranker result was
**+0.0032 NDCG@10**; most were negative. The effect worsens as the embedder
improves (GTE's gain halved from +0.0032 to +0.0016 when dense went 0.5909 →
0.6075), because a reranker's ceiling sits below the ranking it is asked to
improve. The incumbent `ettin-reranker` is additionally **English-only**, while
every candidate embedder is multilingual.

Late interaction (`colbert-xm`) was also tested: excellent cost profile (3.2 ms
per query, 42 GB per million docs) and **catastrophic quality** (−0.15). All
cascade and RRF-fusion combinations of the two rerankers were negative.

### What to delete

- `rerank_coords()` and the rerank role in `scripts/aimee-llm-supervisor.sh`
- `RERANK_*` env plumbing (`AIMEE_LLM_RERANK_TIER/MODE/BATCH/UBATCH/CTX/PARALLEL`)
- the `head.npz` fetch and `RERANK_ASSET_BASE`
- `scripts/aimee_llm_rerank_head.py`
- `.github/workflows/publish-rerank-artifacts.yml` and the
  `rerank-artifacts-v1` release dependency
- the `/rerank` endpoint in the gateway — **and its kb call sites in the same
  change**, so no dangling endpoint or 404 path is left behind

### Do not delete blindly

Confirm no consumer outside the kb calls `/rerank` (the smoothnas plugin
manifests reference reranker URLs). Removal is a net simplification: it also
eliminates the only component requiring a pre-converted GGUF release artifact.

---

## What NOT to do

- **Do not re-run the embedder or reranker benchmarks.** Results are cached in
  `/opt/rr/` on CT 106 (`.253`) and committed under `benchmarks/results/`.
  Re-run only to validate correctness or to answer a genuinely new question.
- **Do not adopt Qwen3.** 0.6B was the weakest model measured on this corpus
  (0.5810); 4B ties nomic at 3.3x the vector storage and 3.1x slower embedding.
  Its strong *published* code scores are suspect: it appears to have trained on
  CodeSearchNet, which would make those numbers memorisation rather than
  capability, and it came **last on code** on our corpus (0.7325 vs nomic 0.8104).
- **Do not trust the GTE reranker's torch path** if it is ever revisited — it
  returns constant scores (it reproduced the no-rerank baseline to sixteen
  decimal places). Only the ONNX export works.

## The bigger opportunity, deliberately out of scope here

Hybrid retrieval (BM25 + RRF) measured **+0.1168 Recall@10** over dense alone —
roughly **35x** the best reranker result — using aimee's existing FTS leg and an
untuned fusion constant. And RRF `k=10` beat the textbook `k=60` on every metric.
That, plus [learning-to-rank](learning-to-rank-from-interactions.md), is where
the remaining quality lives. This handoff covers the cutover only.
