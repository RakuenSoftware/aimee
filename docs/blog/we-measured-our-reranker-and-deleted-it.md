# We spent a night measuring our retrieval stack, and deleted the reranker

*Draft — 2026-07-30. Numbers from `docs/validation/`, raw artifacts in
`benchmarks/results/`.*

We set out to answer a small question: which embedding model should our knowledge
base use? We ended up answering a much larger one, which is that our reranker —
a component nobody was questioning — was making retrieval *worse*.

This is a writeup of what we measured, what we got wrong, and the one
methodological lesson that turned out to matter more than any individual number.

## The setup

Our KB does dense retrieval over ~26k documents, then reranks the top candidates
with a cross-encoder before answering. Standard architecture. We had a fresh
evaluation suite — 10,000 queries against the full corpus, built from our own
content, with three categories: prose, code, and cited artifacts.

The plan was to pick an embedder and move on.

## Trap 1: benchmark scores are not deployed scores

Our first candidate won its benchmark decisively. Then we noticed the harness was
scoring every model **with its card-recommended prefix** — `search_query:` /
`search_document:` for one model, an instruction sentence for another, nothing at
all for a third.

Our serving code applies no prefixes.

| model | with card prefix | prefix-free (as served) |
| --- | ---: | ---: |
| nomic-embed-text-v2-moe | 0.6072 | **0.5823** |
| Qwen3-Embedding-0.6B | 0.5810 | **0.5275** |
| bekko-a25m | 0.5909 | 0.5909 *(card defines none)* |

The ranking **inverts** between the two columns. A model that needs no prefix
carries its benchmark score into production intact; a prefix-dependent one does
not. We had been about to select on the left column and ship the right one.

**Lesson: a benchmark number is only a deployment number if the consumer
reproduces the benchmark's input conditions.**

## Trap 2: capability is not usefulness

With the embedder settled we turned to the reranker, which was English-only and
had to be replaced for multilingual support anyway.

Measured against the suite's reranking view — 20 candidates in arbitrary order,
one relevant — reranking looked transformative:

| reranker | NDCG@10 |
| --- | ---: |
| no rerank | 0.2279 |
| ettin-68m (incumbent) | 0.2969 |
| bge-reranker-v2-m3 | 0.6174 |
| gte-multilingual-reranker-base | **0.7178** |

A clean +0.49 over doing nothing. Obvious win.

Except that view feeds the reranker **randomly ordered** candidates. Production
feeds it the dense top-k, which is already well ordered. So we ran the pipeline
end to end, over the full corpus, 10,000 queries:

| pipeline | NDCG@10 | vs dense |
| --- | ---: | ---: |
| dense only | **0.5909** | — |
| + GTE @ depth 10 | 0.5803 | −0.0106 |
| + GTE @ depth 20 | 0.5861 | −0.0048 |
| + GTE @ depth 50 | 0.5942 | **+0.0032** |

Reranking *degrades* the result at every depth anyone would actually run.

The mechanism is visible in the numbers. The reranker's standalone capability
tops out around 0.59–0.62 at these truncations — which is where dense retrieval
already sits. **Its ceiling is below the ranking it is being asked to improve**,
so on average every reordering is a step backwards.

Those two tables answer different questions. *Can this model sort a random list?*
is not *can this model beat my embedder?* Only the second is the production
question, and it had never been run.

## Trap 3: the fix that was measured on 600 queries

An earlier run on a 600-query subsample showed reranking helping by **+0.020**.
At 10,000 queries the same configuration measured **−0.0048**. A sign flip.

We nearly shipped a recommendation on the subsample.

## Late interaction: right architecture, wrong model

Cross-encoders are expensive because cost scales with `candidates × tokens`, paid
per query, uncacheable. Late interaction (ColBERT-style) precomputes document
token vectors at index time; query time is one encode plus MaxSim.

The cost profile is everything you would want:

| | colbert-xm | GTE cross-encoder |
| --- | ---: | ---: |
| query cost | **3.2 ms** | 143 ms |
| storage | 41 GB / million docs | none |

**45× cheaper per query.** And then the quality:

| pipeline | NDCG@10 | vs dense |
| --- | ---: | ---: |
| dense only | **0.5909** | — |
| colbert-xm @ depth 20 | 0.4663 | −0.1247 |
| colbert-xm @ depth 50 | 0.4437 | −0.1473 |
| cascade colbert→GTE | 0.5346 | −0.0563 |
| RRF fusion | 0.5342 | −0.0567 |

It gets *worse* with more candidates, meaning it actively promotes irrelevant
documents. Every cascade and fusion variant failed too.

We think this is a domain-mismatch result rather than an indictment of late
interaction — the cost profile proves the architecture works. But it is the only
licence-clean multilingual ColBERT available, so the architecture currently has
no viable candidate for us.

## The result

Across **ten** reranking configurations, exactly one beat dense retrieval: GTE at
depth 50, by **+0.0032**, for 143 ms per query on GPU and unaffordable on CPU.

So we deleted the reranker. That removes a GGUF conversion pipeline, a separate
score-head artifact, a release workflow, and an entire serving component — and
it makes the CPU and GPU tiers return identical rankings, which they previously
could not.

Modern retrieval-trained embedders appear to have closed the gap that rerankers
were introduced to fill. Our incumbent reranker was worth "4–5 points" when it
was adopted; measured against a current embedder it is worth less than nothing.

## The lesson that mattered most

Six substantive claims we made during this work were wrong and corrected only by
measuring:

- CPU reranking feasibility — off by **10×** (extrapolated from the wrong runtime)
- late-interaction speedup — off by **3×**
- storage cost — off by **5.7×** (assumed 128-dim vectors, the model emitted 1024)
- "latency is linear in tokens" — it is superlinear
- "truncate documents, don't trim candidates" — true for capability, false for usefulness
- "uniform embedding dimensions are an architectural win" — the system already handled it

But the pattern underneath is the important part. **Almost every failure was
silent, not loud:**

- a pooling default that produced well-formed *wrong* vectors
- a prefix flag worth 0.025 NDCG that nothing warned about
- `-ngl 0` silently overridden by an auto-fit heuristic
- `-np 4` quietly quartering the context window to 512 tokens
- a GPU ONNX provider silently falling back to CPU — a 22-hour run masquerading as a 35-minute one
- a reranker returning **constant scores**, which reproduced the no-rerank baseline to sixteen decimal places

That last one is the one to sit with. We caught it *only* because matching the
baseline exactly was too perfect to be real. Had it returned 0.21 instead of
0.2279, we would have written off the best reranker we tested and never known.

None of these threw an error. Each produced a plausible number.

**If you take one thing from this: on a retrieval stack, the dominant failure
mode is silent-wrong, not loud-wrong.** Record provenance — model, precision,
device, truncation, sample size, harness — for every figure. A number without it
is not evidence.
