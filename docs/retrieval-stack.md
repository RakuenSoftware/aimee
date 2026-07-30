# Retrieval stack

Retrieval is hybrid. Lexical, dense, graph, code, scope, recency, and confidence signals produce a
candidate set; optional synthesis refines it.

```text
query -> normalize/rewrite -> parallel candidate sources -> fuse
      -> evidence build -> confidence/abstain -> optional synthesis
```

No stage may claim it ran when its dependency was unavailable. Degraded results state which signals
were used.

## Embedding

One embedder identity and dimension applies to a deployment. The KB stores derived vectors in DB2;
`aimee-llm` serves the model.

Every tier serves the same 768-dim embedder, so the tier is a GPU-offload choice and an
index built under one tier is readable under another.

Check [Inference tiers](AIMEE_KB_SYNTH_TIERS.md) for the current model names and hardware estimates.

The configured dimension must equal the model output. DB2 records the dimension used to create its
vector columns and refuses startup on drift. Silent empty vector search is worse than a hard start
failure.

### One bundled embedder, or your own

`bekko-a25m` ships inside the `aimee-kb` container, with its weights baked into the image.
A fresh install embeds immediately — no inference service, no GPU, no model download, no
network. It is **384-dimensional**.

| | `bekko-a25m` (bundled) |
| --- | ---: |
| NDCG@10 (frozen-ab-v1) | 0.5909 |
| dimension | 384 |
| context | 8192 |
| prefixes | none — its card defines none, so its benchmark number carries into production unchanged |
| vocab | 256k, multilingual |

**Above 384 dimensions, run your own embedder.** Point `AIMEE_EMBEDDER_URL` (or the
wizard's "External endpoint" option) at a GPU-served endpoint. That is the supported route
to a wider or stronger embedder, and it is why the measurement winner is not bundled:
`nomic-embed-text-v2-moe` scored 0.6075 against bekko's 0.5909, but it is 768-dim, ~6x
slower on CPU, needs its card prefixes to reach that number at all, and cost 1.8GB of
image. The evidence for both is in
[the selection report](validation/embedder-selection-frozen-ab-v1.md).

An external embedder needs its **dimension supplied**: the kb sizes its vector columns
from it and cannot derive the width of an endpoint it does not serve. Nothing applies
prefixes on that path either, so a prefix-dependent model must apply its own.

Operators can declare additional models with `EMBEDDERS_EXTRA`, giving the pooling, width,
context and prefixes — nobody can infer those for you, and each one changes the vectors.
An overlay entry whose weights are not baked is reachable only as an external endpoint.

**Changing the embedder is destructive.** The wizard requires a typed confirmation,
because:

- a different width rebuilds the pgvector columns *and* re-embeds everything;
- the same width with different pooling or prefixes still re-embeds everything, since
  those define the vector space.

The kb refuses to start until the corpus matches what the endpoint serves, so the choice
is gated up front rather than discovered at the next boot.

### What defines the vector space

Width is not identity. Pooling and the query/document prefixes change every vector while leaving
both the dimension and the model name untouched — well-formed vectors, right width, right name,
different space, collapsed recall and no error anywhere. Both have happened: nomic served with
`last` pooling (correct for the previous Qwen3 embedder), and nomic served prefix-free, which
measured 0.5823 NDCG@10 against 0.6075 with its card prefixes.

So the gateway publishes a `serving_id` on `/health` — the model key plus a digest over pooling and
the prefix pair — and the KB records it in `kb_meta.schema_embedder_serving_id` on first start
against a corpus. A later start whose endpoint reports a different `serving_id` **refuses**, naming
both values, and the remediation is a full re-embed:

```bash
aimee kb reembed
```

There is deliberately no compat list here, unlike the model-identity guard: two models can be shown
to agree by measuring cosine, but a changed prefix pair is definitionally a different space. Two
limits worth knowing:

- An endpoint that reports no `serving_id` (a legacy embedder, or a gateway predating the field)
  leaves the guard inactive rather than refusing, so upgrades do not strand existing deployments.
- A corpus embedded before the guard existed adopts the current identity on its first start, because
  it is indistinguishable from a fresh one. If such a corpus was built while prefixes were disabled,
  re-embed it once by hand — the guard cannot detect drift it never recorded a baseline for.

## Changing dimension

A same-dimension model change can re-embed in place. Moving between 1024 and 2560 requires rebuilding
the dimensioned vector tables from source rows.

Before changing it:

1. back up DB2;
2. stop KB writers;
3. record the old model, dimension, and row counts;
4. drop only the derived vector tables named by the migration/runbook for that build;
5. clear the recorded schema dimension in the same transaction;
6. set the new model and dimension;
7. start the KB and let backfill finish;
8. compare source counts, vector counts, recall, and latency before reopening traffic.

Do not merely delete vector rows. The PostgreSQL column still has its old dimension. Do not clear the
dimension marker without rebuilding the columns.

Use the checked-in migration for the exact table list. Hand-written lists go stale as new evidence
tables are added.

## Fusion

Lexical search covers exact names and identifiers. Dense search bridges wording. Entity and code
graphs carry structure. Reciprocal-rank or the configured fusion mode combines ranked lists without
pretending their raw scores share one scale.

Scope and authorization apply before candidates reach the result. A later filter is not sufficient
because ranking and timing can leak excluded data.

## No cross-encoder rerank stage

There is no reranker. Measured across 20 configurations and two embedders, the best cross-encoder
result was +0.0032 NDCG@10 and most were negative — a reranker's ceiling sits below a strong dense
ranking, so the effect shrank as the embedder improved. Hybrid BM25+RRF fusion measured +0.1168
Recall@10 over dense alone, roughly 35x the best rerank result, which is where the remaining quality
lives. See [the retrieval-stack report](validation/retrieval-stack-report-2026-07-30.md).

## Evidence and abstention

The evidence builder keeps source IDs, spans/pages, relationship paths, freshness, and score
components. Confidence considers coverage and contradictions, not only the top similarity score.

A weak or conflicting set may return an abstention. Optional synthesis receives the bounded evidence
and must cite it.

## Configuration and checks

Use the [generated configuration](gen/configuration.md) for embedder URL, dimension, fusion,
top-k, and evidence gates.

After a retrieval change, run lexical-only, dense-only, fused, degraded, scope-negative, dimension
drift, and benchmark cases. Record corpus hash, model identity, config, and latency with the result.
