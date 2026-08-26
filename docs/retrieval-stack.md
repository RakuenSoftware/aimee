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

One embedder identity and dimension applies to a corpus under one KB storage authority. The KB stores
derived vectors in DB2 and owns the embedding role. The selected model can run inside its container
or at a configured remote endpoint. The bundled `bekko-a25m` is 384-dimension.

The embedder is selected in the wizard's Deploy topology step. Until one is selected the KB serves a
builtin lexical embedder. Retrieval works, but it is keyword matching rather than vector search.

Check [KB model tiers](AIMEE_KB_SYNTH_TIERS.md) for sizing an internal synthesis role.

The configured dimension must equal the model output. DB2 records the dimension used to create its
vector columns and refuses startup on drift. Silent empty vector search is worse than a hard start
failure.

### One bundled embedder, or your own

`bekko-a25m` ships inside the `aimee-kb` container, with its weights baked into the image.
After the wizard selects it, a fresh install embeds without an inference service, GPU, model
download, or network access. Until that selection is saved, the KB uses its builtin lexical
embedder. `bekko-a25m` is **384-dimensional**.

| | `bekko-a25m` (bundled) |
| --- | ---: |
| NDCG@10 (frozen-ab-v1) | 0.5909 |
| dimension | 384 |
| context | 8192 |
| prefixes | none. Its card defines none, so its benchmark number carries into production unchanged |
| vocab | 256k, multilingual |

**For an embedder not included in the selected KB image, use a remote role.** Point
`EMBEDDER_URL` (or the wizard's "External endpoint" option) at a GPU-served endpoint. That is
the current profile's route to a wider or stronger embedder, and it is why the measurement winner is
not bundled:
`nomic-embed-text-v2-moe` scored 0.6075 against bekko's 0.5909, but it is 768-dim, ~6x
slower on CPU, needs its card prefixes to reach that number at all, and cost 1.8GB of
image. The evidence for both is in
[the selection report](validation/embedder-selection-frozen-ab-v1.md).

An external embedder needs its **dimension supplied**: the kb sizes its vector columns
from it and cannot derive the width of an endpoint it does not serve. Nothing applies
prefixes on that path either, so a prefix-dependent model must apply its own.

Operators can declare additional models with `EMBEDDERS_EXTRA`, giving the pooling, width,
context and prefixes. Nobody can infer those for you, and each one changes the vectors.
An overlay entry whose weights are not baked is reachable only as an external endpoint.

**Changing the embedder is destructive.** The wizard requires a typed acknowledgement because:

- a different width requires rebuilding the pgvector columns and every derived vector;
- the same width with different pooling or prefixes still invalidates every vector because those
  settings define the vector space.

The acknowledgement does not run a migration. The KB refuses to start when its recorded identity
does not match the endpoint. Follow [Change the KB embedder](runbooks/change-embedder.md) before
saving a different choice for an active corpus.

### What defines the vector space

Width is not identity. Pooling and the query/document prefixes change every vector while leaving
both the dimension and the model name untouched: well-formed vectors, right width, right name,
different space, collapsed recall and no error anywhere. Both have happened: nomic served with
`last` pooling (from an earlier model contract), and nomic served prefix-free, which
measured 0.5823 NDCG@10 against 0.6075 with its card prefixes.

The selected embedding role publishes a `serving_id` on `/health` (the model key plus a digest over
pooling and the prefix pair), and the KB records it in `kb_meta.schema_embedder_serving_id` on first start
against a corpus. A later start whose endpoint reports a different `serving_id` **refuses** and
names both values. The dimension-reset command cannot repair a same-dimension identity change;
follow [Change the KB embedder](runbooks/change-embedder.md) for the supported replacement path.

There is deliberately no compat list here, unlike the model-identity guard: two models can be shown
to agree by measuring cosine, but a changed prefix pair is definitionally a different space. Two
limits worth knowing:

- An endpoint that reports no `serving_id` (a legacy or third-party embedder) leaves the guard
  inactive rather than refusing, so upgrades do not strand existing deployments. The **builtin**
  lexical embedder does declare one (`builtin/lexical-v1`) because it shares the bundled model's
  384 width. Without an identity, switching between the two would be invisible to both guards.
- A corpus embedded before the guard existed adopts the current identity on its first start, because
  it is indistinguishable from a fresh one. If such a corpus was built while prefixes were disabled,
  re-embed it once by hand: the guard cannot detect drift it never recorded a baseline for.

## Changing dimension

`aimee kb reembed` is a dimension-change reset. It does not rebuild a same-dimension corpus; when
the target equals the recorded dimension, it reports that no dimension change is needed and exits.
Use a fresh DB2 and re-ingest authoritative sources for same-dimension model or serving-identity
changes.

Before changing it:

1. back up DB2;
2. stop KB writers;
3. record the old model, dimension, and row counts;
4. enable `kb.reembed_on_dim_change` in the KB's own configuration;
5. review `aimee kb reembed --dry-run --target-dim <new-dimension>`;
6. run `aimee kb reembed --confirm --target-dim <new-dimension>`;
7. switch to the new embedder before allowing requeued work to complete;
8. run `aimee memory embed --all` to restore memory vectors;
9. compare source counts, vector counts, recall, and latency before reopening traffic.

Do not merely delete vector rows. The PostgreSQL column still has its old dimension. Do not clear the
dimension marker without rebuilding the columns.

Do not maintain a hand-written table list. The server-side plan owns the current derived tables and
refuses unknown half-vector or foreign-key conditions unless the operator makes the override explicit.

## Fusion

Lexical search covers exact names and identifiers. Dense search bridges wording. Entity and code
graphs carry structure. Reciprocal-rank or the configured fusion mode combines ranked lists without
pretending their raw scores share one scale.

Scope and authorization apply before candidates reach the result. A later filter is not sufficient
because ranking and timing can leak excluded data.

## No cross-encoder rerank stage

There is no reranker. Measured across 20 configurations and two embedders, the best cross-encoder
result was +0.0032 NDCG@10 and most were negative. A reranker's ceiling sits below a strong dense
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
