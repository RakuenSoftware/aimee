# Retrieval stack: measurements and paths forward

**2026-07-29/30.** Synthesis of an overnight measurement campaign on embedders,
rerankers, and the query-latency budget. Supporting detail in
[embedder-selection-frozen-ab-v1](embedder-selection-frozen-ab-v1.md) and
[reranker-and-pipeline](reranker-and-pipeline-2026-07-29.md); raw artifacts in
[`benchmarks/results/reranker-2026-07-29/`](../../benchmarks/results/reranker-2026-07-29/).

---

## Executive summary

1. **The reranker may not be earning its place.** Against a dense-ordered top-20
   — what production actually feeds it — reranking *costs* 0.07-0.09 NDCG. The
   reranker's ceiling sits below what a modern embedder already achieves.
2. **The embedder choice is worth less than it appeared.** a25m and nomic differ
   by 0.016-0.021 NDCG. The reranking question is an order of magnitude larger.
3. **nomic's advantage is conditional on work that does not exist.** Its lead
   requires query/document prefixes; aimee has no prefix plumbing, and without it
   a25m leads.
4. **The current reranker is disqualified on multilingual grounds** and, if a
   reranker is kept at all, better options exist that are also simpler to ship.
5. **Late interaction posts the best retrieval quality measured** (0.946
   Recall@10) but costs 743 GB per million documents as configured.

---

## 1. Embedders

Each model with its own card prefix and native pooling — every model at its best.

| model | NDCG@10 | R@10 | dim | code | prose | cited | GPU vec/s |
|---|---:|---:|---:|---:|---:|---:|---:|
| **nomic-embed-text-v2-moe** | **0.6072** | 0.8007 | 768 | **0.8104** | 0.5157 | 0.6344 | 82.7 |
| Qwen3-Embedding-4B | 0.6061 | **0.8100** | 2560 | 0.7394 | **0.5274** | 0.6988 | 26.4 |
| bekko-embedding-v1-a25m | 0.5909 | 0.7816 | 384 | 0.7718 | 0.4841 | **0.7170** | **510.7** |
| Qwen3-Embedding-0.6B | 0.5810 | 0.7765 | 1024 | 0.7325 | 0.4930 | 113.1 |

**Qwen3 is out.** 0.6B is the weakest thing measured; 4B ties nomic while costing
3.3x the vector storage and embedding 3.1x slower. The ladder tops out at parity
with a 475M model, so "we can offer 4B and 8B" buys a more expensive route to the
same place.

### The prefix problem decides between the remaining two

The suite scores each model **with** its card prefix. aimee serves **without**
one. For a prefix-dependent model those are different numbers:

| model | with prefix | prefix-free (what ships today) |
|---|---:|---:|
| nomic-v2-moe | 0.6072 | 0.5823 |
| Qwen3-0.6B | 0.5810 | 0.5275 |
| **a25m** | **0.5909** | **0.5909** (its card defines none) |

**On paper nomic leads. As the system is built, a25m leads.** Two coherent
positions:

- **build per-model prefix support** -> nomic at 0.6072, largest code margin
- **do not** -> a25m at 0.5909, needing no new machinery at all

Selecting on the first column while serving the second is the incoherent option,
and it is what the repository currently does.

### Costs beyond quality

| | a25m | nomic-v2-moe |
|---|---|---|
| CPU throughput | **2,155 tok/s** | 598 tok/s (**3.6x slower**) |
| Vector width | 384 | 768 |
| Prefix machinery | none needed | required, plus a full re-embed to adopt |
| Maturity | 10 days old, no published baselines | 18 months, MTEB-checkable |
| Author track record | 54 models, 272k downloads, active since 2024 | established |

---

## 2. Rerankers

### The incumbent is disqualified

`cross-encoder/ettin-reranker-*` is **English-only** (`language: ['en']`), while
every candidate embedder is multilingual. It must be replaced or removed.

### Capability, measured on unsorted candidates

| reranker | NDCG@10 | GPU s/query | params |
|---|---:|---:|---:|
| **gte-multilingual-reranker-base** (ONNX) | **0.7178** | see below | 306M |
| BAAI/bge-reranker-v2-m3 | 0.6174 | 0.120 | 568M |
| ettin-reranker-68m (disqualified) | 0.2969 | 0.054 | 68M |
| no rerank | 0.2279 | 0 | — |

GTE is the best reranker measured and is **1.9x smaller** than the runner-up.
Both replacements are `seq-cls`, so they convert to GGUF whole and delete the
`head.npz` + `publish-rerank-artifacts.yml` machinery Ettin requires.

> **The GTE torch path is broken** — it returns constant scores (it reproduced
> the no-rerank baseline to sixteen decimal places). Only the **ONNX** export is
> usable. This bug silently corrupted two experiments before it was caught.

### Usefulness, measured on dense-ordered candidates — the production question

| embedder | dense only | + rerank 20x128 | + rerank 10x256 |
|---|---:|---:|---:|
| a25m | **0.5903** | 0.5206 (-0.070) | 0.5470 (-0.043) |
| nomic + prefix | **0.6116** | 0.5246 (-0.087) | 0.5599 (-0.052) |

**Reranking degrades a ranking a modern embedder already produced.** The
reranker's best score at these truncations (~0.594) is below dense retrieval's
0.612, so its reorderings are on average backwards.

The two tables are not in conflict — they answer different questions. Capability
(can it sort a random list?) is not usefulness (can it beat the embedder?). Only
the second is the production question, and it had never been run.

**Truncation is ruled out.** The same test at full length still degrades:

| embedder | dense | + rerank 20x512 | + rerank 20x256 |
|---|---:|---:|---:|
| a25m | **0.5903** | 0.5381 (-0.052) | 0.5238 (-0.066) |
| nomic + prefix | **0.6116** | 0.5543 (-0.057) | 0.5340 (-0.078) |

Truncation makes it worse (-0.070 at 128 tokens) but is not the cause.
**bge-reranker-v2-m3 degrades this pipeline at every configuration tested.**

### The rule this implies, and why it does not condemn all rerankers

A reranker helps only if **its capability exceeds the dense ranking it is
handed**:

| reranker | capability (unsorted) | dense ranking | can it help? |
|---|---:|---:|---|
| bge-reranker-v2-m3 | 0.6174 | 0.6116 | parity — **no** |
| **gte-multilingual-reranker-base** | **0.7178** | 0.6116 | **+0.106 — possibly** |

bge-v2-m3 is at parity with dense retrieval, so it can only shuffle. GTE scores
0.106 higher — and it was then tested in-pipeline.

### Resolved: GTE improves the pipeline, but only at full document length

600 cases, dense-ordered top-20, GTE via ONNX (`degenerate: 0`):

| embedder | dense | + GTE 20x128 | + GTE 20x512 |
|---|---:|---:|---:|
| a25m | 0.5934 | 0.4976 (**-0.096**) | **0.6136 (+0.020)** |
| nomic + prefix | 0.6092 | 0.5024 (**-0.107**) | **0.6172 (+0.008)** |

**Reranking helps — at 512 tokens only.** At 128 tokens it is catastrophic. The
helpful configuration costs **4.1s on CPU** (outside budget) and ~0.09s on GPU.

**Correction to the design rule stated earlier in this campaign.** The rule
"never trim the candidate list — truncate documents instead" was derived from the
*unsorted* reranking view, where truncation cost only 0.023. Against a
*dense-ordered* list truncation costs 0.10 and destroys the entire benefit.
Beating an already-good ranking requires full document context; sorting a random
list does not. The rule holds for reranker *capability* and is wrong for
reranker *usefulness* — which is the production case.

### A good reranker compresses the embedder gap

| | dense | after GTE 20x512 |
|---|---:|---:|
| nomic minus a25m | **0.0158** | **0.0036** |

Reranking removes 77% of the difference between the two embedders. With a
reranker in the pipeline, a25m is within 0.004 of nomic while being 3.6x faster
on CPU, half the vector width, and needing no prefix machinery at all.

**This is the strongest argument in the campaign for a25m**, and it only appears
when the embedder and reranker are measured together rather than separately.

### If a reranker is kept: how to configure it

Latency is superlinear in document length but only linear in candidate count,
while **quality depends overwhelmingly on candidate count**:

| bge-v2-m3 config | NDCG@10 | GPU s/q | CPU s/q |
|---|---:|---:|---:|
| 20 x 512 | 0.6174 | 0.091 | 8.44 |
| **20 x 128** | **0.5944** | **0.033** | **1.15** |
| 10 x 256 | 0.3541 | 0.027 | 1.31 |
| 5 x 256 | 0.2081 | 0.018 | 0.56 |

Cutting 512->128 tokens costs 0.023 NDCG. Cutting 20->10 candidates costs
**0.24**. Five candidates (0.208) scores *below not reranking at all* (0.228).

> **Design rule: never trim the candidate list to save time — truncate documents.**

**CPU-viable recommendation if a reranker is kept:** gte-multilingual, ONNX int8,
**20 candidates x 128 tokens — 0.6116 NDCG at 0.731s**, inside the 1s budget.

---

## 3. Late interaction

`bge-m3` multi-vector (ColBERT), 800 cases:

| metric | value |
|---|---:|
| NDCG@10 | 0.7014 |
| **Recall@10** | **0.946** (best measured) |
| Query time | 0.058s (0.031 encode + 0.027 MaxSim) |
| Index time | 0.0106 s/doc |
| Storage | **743 GB per million docs** |

Its cost profile matches the constraint exactly — heavy at index time, trivial
per query — and **if bge-m3 were also the embedder the query encode is shared**,
leaving 27 ms of MaxSim as the true marginal cost. It would also supply the
learned-sparse leg the architecture already anticipates, from one model.

### Storage: bge-m3 is the problem, not late interaction

Late interaction stores **one vector per token**, so the cost is
`tokens x dims x bytes`. bge-m3 emits **1024-dim** vectors, which is 8x wider
than a purpose-built ColBERT:

| model | vectors/doc | dims | per doc | per million |
|---|---:|---:|---:|---:|
| bge-m3 (measured) | 363 | 1024 | 743 KB | **743 GB** |
| colbert-xm (fp16) | 256 | 128 | 64 KB | **66 GB** |
| colbert-xm (int8) | 256 | 128 | 32 KB | **33 GB** |
| *(dense embedding, for scale)* | 1 | 768 | 1.5 KB | 1.5 GB |

**bge-m3 multi-vector is not viable** — 743 GB per million, and it was already
the slowest embedder measured (316 tok/s CPU). It loses on both axes.

**colbert-xm is 11x smaller (22x at int8).** 33 GB per million is an ordinary
index size, so late interaction is not inherently storage-prohibitive; that
particular model was. An earlier version of this page framed 743 GB as the cost
of the architecture, which was misleading.

`antoinelouis/colbert-xm` (MIT, XLM-R, multilingual) is the **only** licence-clean
multilingual ColBERT — answerai-colbert-small is English-only, and jina-colbert-v2
and Reason-ModernColBERT are CC-BY-NC.

**Open question against it:** its `doc_maxlen` is fixed at **256 tokens**, and we
measured that reranking a dense-ordered list needs **512** to beat it (at 256 the
cross-encoder lost 0.066-0.078). Whether late interaction has the same
context-length sensitivity is unmeasured, and it is the thing to test before
committing to this path.

---

## 4. Paths forward

**A. Ships today — a25m, no reranker.** 0.5903-0.5934 dense. No prefix work, no
reranker machinery, 2,155 tok/s on CPU, 384-dim vectors. Deletes the Ettin
release pipeline outright. *Blocked on nothing.*

**B. a25m + GTE reranking on the GPU tier only.** 0.6136 — beats plain nomic
dense (0.6116) using the *cheaper* embedder. GTE ONNX at 20x512, ~0.09s on GPU.
CPU tier runs dense-only, because no rerank config is both affordable and
beneficial there (20x512 is 4.1s; 20x128 loses 0.10). *Blocked on GPU ONNX
serving; note this means CPU and GPU tiers return different rankings.*

**C. nomic + per-model prefix support.** 0.6092-0.6116 dense, 0.6172 with GTE.
The best absolute numbers, but the margin over (B) is 0.0036 once a reranker is
present. *Blocked on building prefix plumbing and a full re-embed.*

**D. Late interaction.** Best retrieval quality measured (0.946 Recall@10) and
architecturally the cleanest fit for a sub-1s budget. *Blocked on a storage
story — 743 GB/million is not shippable as configured.*

**Recommendation: A now, B next, D as the research track.** (C) is hard to
justify: it costs prefix plumbing and a full re-embed to buy 0.0036 over (B).

The reranker is worth keeping — but only on GPU, only at full document length,
and only with GTE. On CPU the evidence says do not rerank at all.

---

## 5. Method note

Six substantive claims made during this campaign were wrong and corrected by
measurement — the harness discrepancy (it was a prefix flag), CPU rerank
feasibility (10x pessimistic), the late-interaction speedup (3x optimistic),
latency linearity, late-interaction storage (5.7x optimistic), and the value of
uniform embedding dimensions.

The dominant failure mode on this stack is **silent-wrong, not loud-wrong**: a
pooling default that produces well-formed bad vectors, a prefix flag that changes
a score by 0.025, an `-ngl 0` that auto-fit overrides, a reranker that returns
constant scores, a `-np 4` that quarters the context window. Each produced a
plausible number rather than an error.

Every figure in this report therefore carries its provenance — model, precision,
device, truncation, sample size, and harness — because on this evidence a number
without provenance is not trustworthy.

---

# Addendum: hybrid retrieval (BM25 + RRF) — the largest effect measured

Added 2026-07-30 after the reranker work concluded. Raw artifacts:
`benchmarks/results/reranker-2026-07-29/hybrid-*.json`, `fusion-frontier-*.json`.

## Result

10,000 queries, full 26,473-document corpus, BM25 over aimee's existing lexical
signal, fused by Reciprocal Rank Fusion with the dense leg.

| pipeline | NDCG@10 | R@10 |
| --- | ---: | ---: |
| a25m dense | 0.5909 | 0.7816 |
| nomic dense | 0.6075 | 0.8006 |
| BM25 alone | 0.6213 | 0.8470 |
| a25m + BM25 (RRF) | 0.6206 | 0.8642 |
| **nomic + BM25 (RRF60)** | **0.6337** | 0.8668 |
| **nomic + BM25 (RRF10)** | — | **0.9034** |

**BM25 alone beats every dense embedder measured.** Fusion beats everything, and
the embedder choice composes with it rather than competing — nomic+hybrid
(0.6337) exceeds a25m+hybrid (0.6206) by roughly the same margin as their dense
scores differ.

## Why: the constraint was recall, not ordering

| pool | contains the labelled document |
| --- | ---: |
| dense top-50 only | 0.8899 (nomic) / 0.8735 (a25m) |
| **dense ∪ BM25 top-50** | **0.9739 / 0.9735** |

Dense retrieval missed the target entirely for **11–13%** of queries. **No
reranker can recover those** — which is precisely why 20 reranking
configurations produced at best +0.0032. Reranking reorders a fixed pool;
lexical fusion changes what is in the pool.

A prediction was recorded before measuring: if the recall-ceiling explanation is
right, Recall@10 should move more than NDCG@10. It did — in the same
configuration (nomic, `k=60`), **+0.0662 vs +0.0262**, a factor of 2.5. The
+0.1028 recall figure quoted elsewhere is the `k=10` variant, for which NDCG@10
was not measured; it must not be paired against a `k=60` NDCG delta.

## The top-1 tradeoff, and a variant with none

Fusion improves recall but can cost top-1 precision, because RRF sees only rank
position and discards score magnitude. This matters for any consumer that takes
the first result rather than reading a whole context.

| variant (nomic) | R@1 | R@10 | R@50 |
| --- | ---: | ---: | ---: |
| dense | 0.3875 | 0.8006 | 0.8899 |
| bm25 | 0.3620 | 0.8470 | 0.9315 |
| rrf60 | 0.3699 | 0.8668 | 0.9649 |
| **rrf10** | 0.3769 | **0.9034** | 0.9649 |
| **tiered** (dense owns rank 1) | **0.3875** | 0.8742 | 0.9649 |

- **`rrf10` dominates `rrf60` on every metric.** The textbook `k=60` is simply
  wrong for this corpus. Free quality from a constant.
- **`tiered` has zero top-1 regression by construction** and still gains +0.074
  R@10 — unconditionally safe where the consumer set is unknown.
- `rrf10` buys a further +0.029 R@10 for −0.011 R@1.

Note `tiered` is safe but **not optimal**: dense wins rank 1 only on average
(0.3875 vs BM25's 0.3620), so forcing dense to own rank 1 forfeits the queries
where BM25's top hit was correct.

## Caveats

- **The queries may be lexically derived from their documents.** Suite queries
  read as document summaries with key terms appended, which flatters BM25.
  Treat **BM25's absolute win as suspect** and the **+10 points of pool recall as
  robust** — decorrelated retrieval finding different documents is far less
  sensitive to phrasing.
- **Labels are silver, one positive per query** out of 26,473 documents. R@1 of
  0.3875 is *not* "correct 38% of the time"; retrieving a different but genuinely
  relevant document scores as a miss. Relative comparisons hold (all methods saw
  identical labels); absolute precision figures are pessimistic by an unknown
  amount.
- Score-fusion variants approximated dense scores from rank position, because
  stage 1 persisted candidate ids but not scores. Those rows are indicative;
  the RRF and tiered rows are exact.

## Consequence for the roadmap

Hybrid fusion on the selected embedder (nomic, RRF `k=60` — one configuration,
both metrics from `hybrid-nomic.json`) delivered **+0.0262 NDCG@10 and +0.0662
Recall@10** over dense alone. That is **8.2x the best reranker result** on
NDCG@10, the metric the two share, plus a recall gain reranking cannot produce at
all — using infrastructure that already exists (the FTS leg) and a constant
nobody tuned. Tuning the constant to `k=10` takes Recall@10 to **+0.1028**;
NDCG@10 was not measured for that variant.

An earlier revision of this section stated "roughly 35x". That divided a
Recall@10 gain by an NDCG@10 gain, and drew the two from different embedders and
different fusion variants (a25m at `k=10` against nomic at `k=60`). Quote deltas
from one embedder and one variant, as above.
Combined with [learning-to-rank](../proposals/pending/learning-to-rank-from-interactions.md),
which would learn that combination from real interactions instead of guessing it,
this is where the remaining retrieval quality lives.
