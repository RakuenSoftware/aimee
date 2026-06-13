# Retrieval Stack: Embedding + Reranking

aimee's semantic memory, KB document/code RAG, and deep-curator vector stores
all share a **single embedder per deployment**. There is no two-tier (fast +
deep) arrangement — one model embeds everything, and every vector column is
sized to that model's output dimension.

## The embedder

The embedder is any command that reads text on stdin and writes a JSON float
array on stdout (`scripts/embedder-server.py` provides an HTTP sidecar wrapper
for the HuggingFace / sentence-transformers stack). Two reference models:

| Model | `embedding_dim` | Notes |
|-------|-----------------|-------|
| `pplx-embed-v1-4b` (default) | `2560` | Higher quality; the default, since embedding throughput is not the bottleneck. Needs more RAM/compute. Exceeds pgvector's 2000-dim `vector` index cap, which is why all columns use `halfvec`. |
| `pplx-embed-v1-0.6b` | `1024` | Lighter tier — fast, low memory. Published as the `aimee-embedder-0.6b` image. |

Pick one. The default `aimee-embedder` image bakes the 4B. To run the lighter
0.6B instead, point at the `aimee-embedder-0.6b` image (`AIMEE_EMBEDDER_IMAGE`)
and set `embedding_dim: 1024` — no rebuild needed.

## Configuration

Two config keys define the embedder:

- `embedding_command` — the command that produces embeddings.
- `embedding_dim` — the dimension that command emits (1024 or 2560). This is the
  single source of truth for vector-column dimensions.

Keep the two in sync: `embedding_dim` must equal the dimension
`embedding_command` actually returns, or vector inserts will be rejected by
Postgres.

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
2000 to 4000 — required for the 4B's 2560 dims.

The code-side embedding buffers are sized by `EMBED_MAX_DIM` (2560 in
`src/headers/aimee.h`), large enough to hold either model's output; the embed
helpers store whatever dimension the embedder actually returns.

## Switching embedders on an existing database

`db2_init()` never reshapes an existing column (a routine schema-apply must not
touch the corpus). If you change `embedding_dim` against a populated database,
the schema-apply emits a `NOTICE`: the columns are still the old type/dimension
and vector ops degrade until you migrate. Switching dimension (e.g. 0.6B ⇆ 4B,
or off legacy 384-dim all-MiniLM) requires **re-embedding** the corpus at the
new dimension; a same-dimension `vector → halfvec` change only needs an in-place
cast (`deploy/migrations/2026-embed-halfvec.sql`). Back up DB2 first.

## Reranking

An optional cross-encoder reranker refines the top-k of a recall before it is
returned. It is configured independently of the embedder:

- `memory_rerank_enabled` — master toggle.
- `memory_rerank_command` — the reranker command (e.g. the Ettin
  `cross-encoder/ettin-reranker-400m-v1` cross-encoder served by the embedder
  sidecar).
- `memory_rerank_mode`, `memory_rerank_top_k` — strategy and depth.

The reranker is dimension-agnostic — it scores `(query, candidate)` text pairs
directly and is unaffected by the embedder choice above.
