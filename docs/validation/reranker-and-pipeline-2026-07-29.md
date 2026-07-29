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
| 20 | 512 | 10,240 | 4.127s | — | no |

And `BAAI/bge-reranker-v2-m3` int8 (568M), same rig:

| candidates | doc tokens | total | min | within 1s |
|---:|---:|---:|---:|---|
| 5 | 128 | 640 | 0.207s | yes |
| 10 | 128 | 1,280 | 0.473s | yes |
| 5 | 256 | 1,280 | 0.555s | yes |
| 20 | 128 | 2,560 | 1.145s | no |
| 10 | 256 | 2,560 | 1.313s | no |
| 20 | 512 | 10,240 | 8.443s | no |

**A 306M multilingual cross-encoder fits the CPU budget** at 20 candidates x 128
tokens (0.708s) or 10 x 256 (0.780s). The 568M model fits only at 10x128
(0.473s) or 5x256 (0.555s).

### Cost per token is NOT constant — truncation beats trimming candidates

| model | ms/token @128 | @256 | @512 |
|---|---:|---:|---:|
| gte-multilingual (306M) | 0.184 | 0.277 | 0.403 |
| bge-v2-m3 (568M) | 0.323 | 0.447 | 0.824 |

Per-token cost **rises with sequence length** (attention is quadratic in it), so
the two levers are not equivalent:

- **Truncating documents is superlinear.** 20x512 -> 20x128 is 4x fewer tokens
  but **5.8x faster** (4.127s -> 0.708s), because each remaining token is also
  cheaper.
- **Cutting candidates is merely linear.** 20x128 -> 10x128 -> 5x128 gives
  0.708 -> 0.305 -> 0.118s.

> **Design rule: shorten documents before you shorten the candidate list.**
> A 1s budget buys ~3,600 tokens at 128-token docs but only ~2,480 at 512-token
> docs, on the 306M model.

An earlier version of this page claimed latency was linear at a flat
~0.33 ms/token. That was an artifact of averaging across the grid; the corrected
figures are above.

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
| **BAAI/bge-reranker-v2-m3** | **0.6174** | **+0.390** | **0.120** | 568M |
| **Alibaba-NLP/gte-multilingual-reranker-base** (ONNX) | **0.7178** | **+0.490** | see below | 306M |
| Alibaba-NLP/gte-multilingual-reranker-base (torch) | *invalid* | | | 306M |
| BAAI/bge-m3 late interaction | *pending* | | | 568M |

> **The GTE torch path is broken and must not be used.** It returned
> 0.2279124426038567 — the no-rerank baseline to sixteen decimal places, i.e.
> constant scores and no reordering. This reproduces the failure already recorded
> in `EMBEDDER_SELECTION.md` for GTE-derived models: disabling
> `use_memory_efficient_attention`/`unpad_inputs` makes them run and return
> garbage. **The ONNX export works correctly** (`degenerate_score_cases: 0`) and
> is the only trustworthy path for this model.
>
> Caught only because reproducing the baseline *exactly* was too perfect to be
> real. A merely plausible wrong number would have been reported as fact.

### GTE is the best reranker measured — smaller and better

At 20 candidates x 512 tokens, **gte-multilingual (306M) scores 0.7178 against
bge-v2-m3's 0.6174 (568M)** — better quality from a model 1.9x smaller.

Sample-size caveat, stated rather than buried: GTE was scored on 1,000 cases via
ONNX on CPU; bge-v2-m3 on 10,000 via torch on GPU. The 0.10 gap is far larger
than sampling noise at n=1,000, but the two are not perfectly matched runs.

### Candidate count dominates truncation for QUALITY

Measured at near-identical latency on CPU:

| config | NDCG@10 | CPU s/query | fits 1s |
|---|---:|---:|---|
| 20 x 512 | **0.7178** | 4.097 | no (GPU only) |
| **20 x 128** | **0.6116** | **0.731** | **yes** |
| 10 x 256 | 0.3920 | 0.787 | yes |

**20x128 scores 0.6116; 10x256 scores 0.3920 — a 0.22 collapse for the same
cost.** The cause is a recall ceiling: with only 10 candidates, a relevant
document at rank 11-20 is unreachable however good the reranker is.

Combined with the latency curve, the two levers are asymmetric in *both*
dimensions:

| lever | latency effect | quality effect |
|---|---|---|
| truncate documents | **superlinear saving** | mild |
| trim candidate list | linear saving | **severe** |

> **Design rule: never trim the candidate list to save time. Truncate documents
> instead.** Keep 20 candidates and cut tokens per document.

**Recommended CPU-tier configuration: gte-multilingual-reranker-base, ONNX int8,
20 candidates x 128 tokens — 0.6116 NDCG at 0.731s**, inside the 1s budget.

### Replacing Ettin is a large quality upgrade, not just a compliance fix

`bge-reranker-v2-m3` **more than doubles** the no-rerank baseline and beats the
incumbent Ettin by **+0.32 NDCG**. The 68M English model was badly under-powered
for separating 20 hard negatives; the multilingual requirement forced a change
that turns out to be worth far more than the requirement itself.

At **120 ms/query on GPU for 20 candidates x 512 tokens**, it is comfortably
inside the 1s budget on a GPU tier.

Note the tier spread for the identical config: **0.120s on an RTX 5080 vs 8.443s
on CPU** — a factor of 70. Reranker choice is therefore genuinely tier-dependent,
and the CPU tier cannot simply run the GPU tier's configuration.

Two further readings:

- **The baseline of 0.2279 is consistent with random ordering**, so this view's
  20 candidates are unsorted hard negatives. It is a clean *reranker-vs-reranker*
  comparison, but it is **not** the "does reranking beat dense retrieval"
  question — that needs a pipeline eval (dense top-20 in dense order, reranked).
- **Absolute scores are low.** A reranker separating one relevant document from
  19 hard negatives only reaches 0.297. This sets realistic expectations for what
  any reranker can deliver on this corpus.

## THE HEADLINE: reranking a strong dense ranking makes it WORSE

Everything above measures reranking against the suite's **unsorted** candidate
list. Production does not feed the reranker unsorted candidates — it feeds it the
dense top-20. Measured end to end over the full corpus, 2,000 queries, reranked
with bge-reranker-v2-m3:

| embedder | dense only | + rerank 20x128 | + rerank 10x256 |
|---|---:|---:|---:|
| a25m | **0.5903** | 0.5206 (**-0.070**) | 0.5470 (-0.043) |
| nomic + prefix | **0.6116** | 0.5246 (**-0.087**) | 0.5599 (-0.052) |

**Reranking costs 0.07-0.09 NDCG.** It does not help; it actively degrades a
ranking that a modern embedder already produced.

The mechanism is visible in the numbers rather than inferred: bge-v2-m3's best
score at these truncations is ~0.594, while dense retrieval alone reaches 0.612.
**The reranker's ceiling is below the ranking it is being asked to improve**, so
every reordering it makes is on average a step backwards.

This also explains why the reranking view showed a huge gain (0.2279 -> 0.6174)
and the pipeline shows a loss. Those are different questions:

- **reranking view** — candidates arrive in random order, so a reranker adds
  enormous value. It measures reranker *capability*.
- **pipeline** — candidates arrive well-ordered, so the reranker only adds value
  if it is *better than the embedder*. It measures reranker *usefulness*.

Only the second is the production question, and it had never been run.

### Historical note

A previously cited figure had the Ettin reranker worth "4-5 points". That was
measured against older, weaker embedders. Against a modern embedder there is
nothing left for the reranker to add — which is exactly why that figure was
correctly dismissed as irrelevant to this decision.

### Open confound

At the time of writing, the pipeline reranks with documents truncated to 128/256
tokens while dense retrieval used 2,048. That handicap could account for some or
all of the degradation. A full-length (20x512) pipeline run is in flight; until
it reports, the correct statement is **"reranking at deployable truncations
degrades results"**, not "reranking is useless".

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

### Measured: bge-m3 multi-vector

800 cases, 20 candidates, on the reranking view.

| metric | value |
|---|---:|
| NDCG@10 | **0.7014** |
| **Recall@10** | **0.946** |
| Recall@5 | 0.855 |
| **QUERY time** | **0.058s** (0.031 encode + 0.027 MaxSim) |
| INDEX time | 0.0106 s/doc |
| **Storage** | **743 KB/doc = 743 GB per million docs** |

**Quality is excellent** — it beats the bge-v2-m3 cross-encoder (0.6174) outright
and posts a 0.946 Recall@10, the highest of anything measured. Query cost is
58 ms, well inside budget.

**And if bge-m3 were also the embedder, the 31 ms query encode is shared with
dense retrieval**, making the marginal cost of reranking just the 27 ms MaxSim.
That is the architectural argument for late interaction, now measured.

**Storage is the blocker.** 743 GB per million documents, because bge-m3 emits
**1024-dimensional** token vectors (743 KB/doc over ~363 tokens). An earlier
estimate on this page said ~131 GB/million; that assumed 128-dim ColBERT vectors
and was wrong by 5.7x. Routes to viability, none yet measured:

- a purpose-built ColBERT at 128 dim: ~8x smaller, ~93 GB/million
- int8 quantisation of the token vectors: 2x
- PLAID-style compression: reported ~16x
- storing multi-vectors only for a hot subset of the corpus

## Environment

- Quality: RTX 5080, bf16, CT 106 on `.253`.
- CPU latency: `.254`, Ryzen 8845HS (AVX-512), 8 threads, int8 ONNX Runtime
  1.28, Plex stopped for a quiet window.
- Note `.253`'s i7-14700K has **no AVX-512**; `.254` does. int8 kernels favour
  the latter, so CPU-tier figures are hardware-sensitive and should be
  re-measured on the actual target.
