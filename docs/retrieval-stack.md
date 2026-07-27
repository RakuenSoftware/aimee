# Retrieval stack

Retrieval is hybrid. Lexical, dense, graph, code, scope, recency, and confidence signals produce a
candidate set; optional reranking and synthesis refine it.

```text
query -> normalize/rewrite -> parallel candidate sources -> fuse
      -> evidence build -> rerank -> confidence/abstain -> optional synthesis
```

No stage may claim it ran when its dependency was unavailable. Degraded results state which signals
were used.

## Embedding

One embedder identity and dimension applies to a deployment. The KB stores derived vectors in DB2;
`aimee-llm` serves the model.

The standard tiers use:

| Tier | Embedding width | Reranker |
| --- | ---: | --- |
| CPU | 1024 | small cross-encoder |
| GPU tiers | 2560 | larger cross-encoder |

Check [Inference tiers](AIMEE_KB_SYNTH_TIERS.md) for the current model names and hardware estimates.

The configured dimension must equal the model output. DB2 records the dimension used to create its
vector columns and refuses startup on drift. Silent empty vector search is worse than a hard start
failure.

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

## Reranking

The cross-encoder scores `(query, candidate)` text pairs and does not depend on embedding dimension.
It runs on a bounded top-k after fusion. If unavailable, the response returns the fused order and an
explicit degradation signal.

## Evidence and abstention

The evidence builder keeps source IDs, spans/pages, relationship paths, freshness, and score
components. Confidence considers coverage and contradictions, not only the top similarity score.

A weak or conflicting set may return an abstention. Optional synthesis receives the bounded evidence
and must cite it.

## Configuration and checks

Use the [generated configuration](gen/configuration.md) for embedder URL, dimension, fusion,
reranking, top-k, and evidence gates.

After a retrieval change, run lexical-only, dense-only, fused, degraded, scope-negative, dimension
drift, and benchmark cases. Record corpus hash, model identity, config, and latency with the result.
