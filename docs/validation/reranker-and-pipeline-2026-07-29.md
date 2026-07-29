# Reranker selection and query-latency budget (2026-07-29)

> **STATUS: IN PROGRESS.** CPU latency measurements are complete and decisive.
> GPU quality measurements for the multilingual candidates are still running;
> sections marked *pending* will be filled in when they land.

## Why this exists

Two constraints were established that invalidate the current reranker:

1. **`cross-encoder/ettin-reranker-*` is English-only** (`language: ['en']`,
   50,368-token English ModernBERT vocab). Every embedder under consideration is
   multilingual, so today's stack retrieves multilingually and then reranks with
   a model that never saw the language. The reranker must be replaced.
2. **Reranking runs synchronously inside the user's query**, and the budget is
   **under 1 second**. Index-time cost is unconstrained.

## The multilingual reranker field is thin

Licence-filtered (CC-BY-NC excluded, consistent with the rule that dropped
embeddinggemma and Nemotron):

| model | params | licence | head | note |
|---|---:|---|---|---|
| Alibaba-NLP/gte-multilingual-reranker-base | 306M | apache-2.0 | seq-cls | smallest viable |
| BAAI/bge-reranker-v2-m3 | 568M | apache-2.0 | seq-cls | 18.8M downloads |
| BAAI/bge-m3 (multi-vector) | 568M | MIT | late interaction | dense+sparse+ColBERT in one |
| antoinelouis/colbert-xm | 853M | MIT | late interaction | multilingual ColBERT |

Everything else is a language-specific distill or non-commercial. **There is no
small multilingual cross-encoder** — the floor is 306M, 4.5x Ettin's 68M.

Both seq-cls candidates are a **simplification** over today's setup: Ettin's score
head does not survive GGUF conversion, which is why aimee carries `head.npz`,
`aimee_llm_rerank_head.py`, and the whole `publish-rerank-artifacts.yml` release
pipeline. A seq-cls reranker converts whole and deletes all of that.

## CPU latency — measured, and it changes the conclusion

`gte-multilingual-reranker-base`, **int8 ONNX**, on `.254` (Ryzen 8845HS w/
AVX-512, 8 threads, Plex stopped). Min of 5 repeats.

| candidates | doc tokens | total tokens | min | median | within 1s |
|---:|---:|---:|---:|---:|---|
| 5 | 128 | 640 | 0.118s | 0.119s | yes |
| 10 | 128 | 1,280 | 0.305s | 0.305s | yes |
| 5 | 256 | 1,280 | 0.330s | 0.331s | yes |
| **20** | **128** | **2,560** | **0.708s** | 0.709s | **yes** |
| 10 | 256 | 2,560 | 0.780s | 0.787s | yes |
| 5 | 512 | 2,560 | 0.891s | 0.891s | yes |
| 20 | 256 | 5,120 | 1.670s | 1.674s | no |
| 10 | 512 | 5,120 | 1.899s | 1.906s | no |

**A 306M multilingual cross-encoder fits the CPU budget.** Not at 20x512, but
comfortably at 20 candidates x 128 tokens, or 10 x 256.

**Latency is linear in total tokens at ~0.33 ms/token**, and depends on the
*product* of candidates and truncation, not on either alone. That yields a
design rule:

> **The 1s CPU budget buys roughly 3,000 tokens.** Spend it as 20x150, 10x300,
> or 6x512.

### Correction to an earlier estimate

An earlier analysis in this session predicted ~34s for 20x512 and concluded CPU
multilingual reranking was infeasible. The measured figure is ~3.4s — **a 10x
overestimate**, caused by extrapolating from torch fp32 rather than measuring
int8 ONNX. The infeasibility claim was wrong; the CPU tier can have a
multilingual reranker.

This matters beyond the number: aimee is not bound to llama.cpp after 0.2.0, and
**int8 ONNX / OpenVINO is materially faster than a GGUF path for encoder models
on CPU**. Both leading embedders already ship ONNX and OpenVINO artifacts
(a25m publishes `onnx/model_qint8_avx512.onnx`).

## Reranker quality — partial

Measured on the frozen-ab-v1 **reranking view**: 10,000 cases, 20 candidates
each, exactly one relevant.

| reranker | NDCG@10 | vs baseline | GPU s/query | params |
|---|---:|---:|---:|---:|
| no rerank (suite candidate order) | 0.2279 | — | 0 | — |
| cross-encoder/ettin-reranker-68m (English, disqualified) | 0.2969 | +0.069 | 0.054 | 68M |
| Alibaba-NLP/gte-multilingual-reranker-base | *pending* | | | 306M |
| BAAI/bge-reranker-v2-m3 | *pending* | | | 568M |
| BAAI/bge-m3 late interaction | *pending* | | | 568M |

Two readings already:

- **The baseline of 0.2279 is consistent with random ordering**, so this view's
  20 candidates are unsorted hard negatives. It is a clean *reranker-vs-reranker*
  comparison, but it is **not** the "does reranking beat dense retrieval"
  question — that needs a pipeline eval (dense top-20 in dense order, reranked).
- **Absolute scores are low.** A reranker separating one relevant document from
  19 hard negatives only reaches 0.297. This sets realistic expectations for what
  any reranker can deliver on this corpus.

## Late interaction — the structural option

Cross-encoder cost is `candidates x tokens`, paid per query, uncacheable. Late
interaction (ColBERT-style) precomputes **document token vectors at index time**;
query time is one query encode plus MaxSim, which is dot products over
precomputed vectors.

That cost profile matches the stated constraint exactly: index time is free,
query time is not. It also stops the cost scaling with candidate count —
reranking 100 candidates costs nearly what 20 does.

An earlier claim in this session put the saving at "~1000x". The honest figure is
**~300x**, dominated by the query encode; MaxSim itself is negligible. **If
`bge-m3` were also the embedder, the query encode is shared with dense retrieval
and the marginal cost of reranking approaches zero.**

The real cost is storage:

| config | per doc | per 1M docs |
|---|---:|---:|
| 512 tok x 128 dim, fp16 | 131 KB | ~131 GB |
| 512 tok x 96 dim, int8 | 49 KB | ~49 GB |
| ColBERT compression (PLAID / 2-bit) | ~8 KB | ~8 GB |

Measurement of bge-m3 multi-vector quality, and its index-vs-query cost split, is
*pending*.

## Environment

- Quality: RTX 5080, bf16, CT 106 on `.253`.
- CPU latency: `.254`, Ryzen 8845HS (AVX-512), 8 threads, int8 ONNX Runtime
  1.28, Plex stopped for a quiet window.
- Note `.253`'s i7-14700K has **no AVX-512**; `.254` does. int8 kernels favour
  the latter, so CPU-tier figures are hardware-sensitive and should be
  re-measured on the actual target.
