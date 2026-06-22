# Embedder retrieval-quality probe (LoCoMo)

A reproducible, **embedder-isolated** retrieval benchmark — a precursor to the
full ship-floor gate in
[unified-llm-container](../proposals/pending/unified-llm-container.md) (the
`nDCG/MRR ≥ 95% of pplx baseline` acceptance gate). It screens a candidate
embedder *in isolation* (no reranker, no fusion, no LLM) so an unviable model is
caught before the heavier full-pipeline sweep
([embedder-sweep.md](../embedder-sweep.md)) is stood up.

Harness: [`benchmarks/embedder_gate.py`](../../benchmarks/embedder_gate.py).

## What it measures

LoCoMo (`locomo10.json`, 10 multi-session conversations) as a **direct
retrieval** task, mirroring `benchmarks/locomo/bench_aimee_direct.py`:

- **Corpus** = the conversation's turns, each `dia_id → "speaker: text"`.
- **Query** = each `qa` question; a turn is **relevant** iff its `dia_id` is in
  the question's `evidence`. (Questions with no evidence — adversarial /
  unanswerable rows — are skipped.)
- Retrieval is cosine over L2-normalized embeddings, **within each conversation**.
- **Metrics** (averaged over all questions): **Recall@5, Recall@10, MRR** — the
  same metrics the ship-floor gate uses.

Any OpenAI-compatible `/v1/embeddings` endpoint is a candidate, so a
**llama.cpp + Vulkan** server is a drop-in. This is the part the GPU exercises.

## How it was run

- **Hardware:** AMD Radeon RX 7900 XTX (24 GB, RADV/Vulkan) on `.254`.
- **Runtime:** `llama.cpp` server build `b9754`, prebuilt `…-vulkan-x64`, all
  layers on GPU (`-ngl 99`). One `llama-server --embeddings` per model.
- **Candidates** (apples-to-apples — same runtime + hardware, each with its
  model-recommended query/doc instruction prefix):
  - `qwen3-0.6b` — `Qwen/Qwen3-Embedding-0.6B-GGUF` Q8_0 (1024-dim) — the
    proposal's CPU/GPU default embedder slot (0.6B @ 1024).
  - `bge-base-en-v1.5` — Q8_0 (768-dim) — strong reference.
  - `nomic-v1.5` — `nomic-embed-text-v1.5` Q8_0 (768-dim) — strong reference.

Reproduce:

```bash
# on a host with the 7900XTX + the prebuilt llama.cpp vulkan build:
llama-server -m Qwen3-Embedding-0.6B-Q8_0.gguf --embeddings --port 8899 -ngl 99 --ctx-size 8192 -ub 8192
python3 benchmarks/embedder_gate.py --dataset locomo10.json \
    --endpoint http://localhost:8899/v1/embeddings --name qwen3-0.6b --output res_qwen3.json \
    --query-prefix $'Instruct: Given a question, retrieve the conversation turns that answer it\nQuery: '
```

## Results

Run 2026-06-22 on the 7900XTX. LoCoMo, 10 conversations, **1982** answerable
questions. Artifacts: [`benchmarks/results/embedder-gate/`](../../benchmarks/results/embedder-gate/).

| model | dim | Recall@5 | Recall@10 | MRR |
|---|---|---|---|---|
| **nomic-embed-text-v1.5** | 768 | **0.6060** | 0.6907 | **0.4741** |
| qwen3-embedding-8b | 4096 | 0.5908 | **0.6917** | 0.4510 |
| qwen3-embedding-0.6b | 1024 | 0.5061 | 0.5974 | 0.3783 |
| qwen3-embedding-4b | 2560 | 0.5040 | 0.5943 | 0.3888 |
| bge-base-en-v1.5 | 768 | 0.4995 | 0.6105 | 0.3681 |

**Finding (decisive for the proposal).** On LoCoMo conversational retrieval,
**`nomic-embed-text-v1.5` (768-dim) beats every Qwen3-Embedding size** — including
the 8B at 4096-dim — on Recall@5 (0.606 vs 0.591) and MRR (0.474 vs 0.451), and
ties the 8B on Recall@10. Two more facts sharpen this:
- **Scaling Qwen3 0.6B→4B buys nothing here** (R@5 0.504 vs 0.506; MRR 0.389 vs
  0.378) — the 4B is not a meaningful upgrade on this benchmark.
- Only **Qwen3-8B** closes most of the gap, and it does so at **4096-dim (5.3×
  nomic's vector size)** — far more storage, scan, and HNSW cost for *worse* R@5.

So nomic wins on **both** axes that matter for the default slot: quality **and**
dimension. The proposal's all-Qwen3 embed ladder
([unified-llm-container](../proposals/pending/unified-llm-container.md) §"model
registry") is dominated by a single 768-dim model on this benchmark; the default
should be **nomic-embed-text-v1.5**, and the GPU tiers warrant re-examination
(an 8B at 4096-dim that loses R@5 to a 768-dim model is a poor high tier here).

**Fairness / reproducibility.** Each model uses its own card-recommended prefix,
recorded in its result JSON (`query_prefix`/`doc_prefix`): Qwen3 the canonical
`Instruct: …\nQuery: ` (newline preserved via `--query-prefix-file`), bge the
`Represent this sentence…` prefix, nomic the `search_query:`/`search_document:`
pair. All vectors are L2-normalized so the dot product is exactly cosine. Metrics
are over the **1982 answerable** questions only — a question with empty `evidence`
(LoCoMo's adversarial/unanswerable rows) has no relevant turn to retrieve, so
retrieval Recall/MRR are undefined for it; this is answerable-only retrieval by
construction, not a silent filter.

**Qwen3 prompt-sensitivity + size checks (resolved).** Qwen3-Embedding is
prompt-sensitive, so the gap was re-tested with the canonical `Instruct:\nQuery:`
newline restored: **identical** (R@5 0.5061 vs 0.5060) — not a prompt-format
artifact. The "try a bigger Qwen3" hypothesis was also tested directly: 4B gives
no lift over 0.6B, and 8B (4096-dim) still trails nomic on R@5/MRR. So neither
prompt format nor model size rescues Qwen3 to nomic's level on this benchmark.
All Qwen3 runs use the same canonical prefix (recorded in each artifact).

**Caveats (do not over-read).** LoCoMo is one (hard, conversational) benchmark;
LongMemEval and the full aimee pipeline (rerank + fusion) can reorder these. This
is an embedder screen, not the production cutover verdict — but it is now a
5-model screen across the whole Qwen3 ladder, which is a strong signal.

## Scope and honesty notes

- This is **embedder-isolated** retrieval, **not** the full aimee pipeline
  (which adds reranking + fusion). It screens the embedder; it is *not* the
  production cutover number.
- It is **not yet the formal ship-floor gate**: that gate is defined against a
  pinned **pplx/ettin baseline fixture** and run through the aimee retrieval
  pipeline (`embedder-sweep.sh` → `aimee eval`). Two follow-ups are required for
  the formal gate: (1) record the pplx/ettin baseline as the comparison anchor;
  (2) realign `embedder-sweep.sh`/`bench_aimee_direct.py` with the current
  `aimee eval` interface (it still calls the retired `aimee-client eval <suite>`)
  and a pgvector DB. This probe is the cheap screen that justifies that effort.
