# Retrieval Stack: Embedding + Reranking

aimee's semantic memory, KB document/code RAG, and deep-curator vector stores
all share a **single embedder per deployment**. There is no two-tier (fast +
deep) arrangement — one model embeds everything, and every vector column is
sized to that model's output dimension.

## The embedder

The embedder is any command that reads text on stdin and writes a JSON float
array on stdout (`scripts/embedder-server.py` provides an HTTP sidecar wrapper
for the HuggingFace / sentence-transformers stack). It ships in two tiers — one
embedder and a cross-encoder reranker sized to the same tier, baked into one
image. (The reranker emits a scalar score, so its size is a quality choice, not
a dimension constraint.)

| Image | Embedder (`embedding_dim`) | Reranker |
|-------|----------------------------|----------|
| `aimee-embedder` (default) | `pplx-embed-v1-4b` (`2560`) | `ettin-reranker-1b` |
| `aimee-embedder-0.6b` | `pplx-embed-v1-0.6b` (`1024`) | `ettin-reranker-400m` |

### Choosing a tier

Run one tier per deployment. The 4b/1b image is the default; the 0.6b/400m
image is for hosts that can't spare the memory.

**4b + 1b (default).** Best retrieval quality and reranking. Pick it when the
host has the RAM and embedding throughput isn't your bottleneck — most server
deployments.
- Recall and ranking are noticeably better, especially on large or noisy
  corpora and on queries that lean on meaning over keywords.
- Costs: the model weights are several GB (slower image pull and first build),
  the default image holds ~20 GB resident in fp32 (~16 GB embedder + ~4 GB
  reranker), and a CPU embed runs ~1–2 s versus the 0.6b's ~0.2 s. None of that
  is in the request hot path — embedding happens at ingest and on the query, not
  per token — but it sets a RAM floor and slows a cold re-embed of a large corpus.

**0.6b + 400m (light).** Pick it for laptops, small VMs, CI, or any host short
on memory.
- ~2 GB of weights (embedder + reranker), a couple of GB resident, embeds in
  ~0.2 s. Fits a small host; fast to pull, fast to re-embed.
- Lower recall and weaker reranking than the 4b/1b — fine for smaller corpora
  and keyword-ish queries, weaker on large-corpus semantic search.

Rule of thumb: default to 4b/1b; drop to 0.6b/400m only when RAM or pull size
forces it. The retrieval pipeline (hybrid search, reranking, fusion) is
identical either way — only the model sizes differ.

### Switching tiers

The default `aimee-embedder` image bakes the 4b + 1b. To run the light tier,
point at the `aimee-embedder-0.6b` image and set the dimension to match:

```bash
AIMEE_EMBEDDER_IMAGE=ghcr.io/rakuensoftware/aimee-embedder-0.6b:latest
AIMEE_EMBEDDING_DIM=1024   # or embedding_dim: 1024 in aimee.yaml
```

No rebuild needed — both images are published. On an **empty** database that's
all it takes. On a **populated** one the dimension change needs a re-embed (the
`halfvec` columns are sized to `embedding_dim`, so 1024 and 2560 vectors can't
share a column) — see [Switching embedders on an existing
database](#switching-embedders-on-an-existing-database) below. Build a custom
pairing with `--build-arg EMBEDDER_MODEL=… --build-arg RERANKER_MODEL=…`.

## Configuration

Two config keys define the embedder:

- `embedding_command` — the command that produces embeddings.
- `embedding_dim` — the dimension that command emits (1024 for the light image,
  2560 for the default; any value for a custom-built image). This is the
  single source of truth for vector-column dimensions.

Keep the two in sync: `embedding_dim` must equal the dimension
`embedding_command` actually returns, or vector inserts will be rejected by
Postgres.

`AIMEE_EMBEDDING_DIM` overrides `embedding_dim` from the environment — useful for
a containerized deploy with no writable `aimee.yaml` (set it alongside
`AIMEE_EMBEDDER_IMAGE` when pinning the 0.6b tier: `AIMEE_EMBEDDING_DIM=1024`).

## How the dimension flows into the schema

The DB2 schema (`src/db2/schema.sql`) declares its embedding columns with a
`halfvec(__EMBED_DIM__)` placeholder rather than a hard-coded dimension:

```sql
CREATE TABLE IF NOT EXISTS memory_embeddings (
    point_id  BIGINT PRIMARY KEY,
    embedding halfvec(__EMBED_DIM__),
    ...
);
```

At startup the server / aimee-kb call `db2_set_embedding_dim(cfg.embedding_dim)`
(from the loaded config) before `db2_init()`. When `db2_init()` applies the
schema, `db_apply_schema_postgres()` substitutes the placeholder with the
configured dimension — the one place the schema is materialized — so every
`halfvec` column (`memory_embeddings`, `kb_embeddings`, and the
`curator_*_vectors` tables) is created at the right size. An unset or
out-of-range value falls back to the `2560` default (the default embedder is the
4B) rather than emitting invalid DDL.

`halfvec` (fp16) is used throughout: it halves index memory versus `vector`
(fp32) at negligible recall cost, and it lifts the index dimension ceiling from
2000 to 4000 — required for the 4B's 2560 dims. This needs pgvector ≥ 0.7 (the
bundled `pgvector/pgvector:pg16` image has it); older pgvector caps at 2000.

The code-side embedding buffers are sized by `EMBED_MAX_DIM` (2560 in
`src/headers/aimee.h`), large enough to hold either model's output; the embed
helpers store whatever dimension the embedder actually returns.

## Switching embedders on an existing database

`db2_init()` never reshapes an existing column (a routine schema-apply must not
touch the corpus). If you change `embedding_dim` against a populated database,
the schema-apply emits a `NOTICE`: the columns are still the old type/dimension
and vector ops degrade until you migrate. Switching dimension (0.6b 1024 ⇆ 4b
2560) requires **re-embedding** the corpus at the new dimension. Drop the
embedding rows (`kb_embeddings`, `memory_embeddings`, the `curator_*_vectors`),
restart the kb so it recreates the columns at the new `embedding_dim`, then let
the curator drain re-embed — it backfills any `kb_documents` chunk missing a
vector. Chunk text and `file_contents` are untouched. A same-dimension
`vector → halfvec` change only needs an in-place cast
(`deploy/migrations/2026-embed-halfvec.sql`). Back up DB2 first.

## Reranking

An optional cross-encoder reranker refines the top-k of a recall before it is
returned. It is served by the same embedder sidecar, sized to match the embedder
tier: the default `aimee-embedder` image bakes the **1B** Ettin reranker
(`cross-encoder/ettin-reranker-1b-v1`) alongside the 4B embedder; the
`aimee-embedder-0.6b` image bakes the **400M** reranker
(`cross-encoder/ettin-reranker-400m-v1`) alongside the 0.6B embedder. Build-time
override: `--build-arg RERANKER_MODEL=<hf id>`.

It is configured independently of the embedder dimension:

- `memory_rerank_enabled` — master toggle.
- `memory_rerank_command` — the reranker command (the Ettin cross-encoder served
  by the embedder sidecar, via `rerank-remote.py`).
- `memory_rerank_mode`, `memory_rerank_top_k` — strategy and depth.

The reranker is dimension-agnostic — it scores `(query, candidate)` text pairs
directly and is unaffected by the embedder choice above.
