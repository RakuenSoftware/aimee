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

### The dimension-drift guard

Sizing the columns at one dimension and then *serving* queries at another is the
worst failure mode: queries embed at the new dimension, the corpus is stored at
the old one, and vector search silently returns nothing — no error, just empty
results. To make that impossible, the first schema-apply records the dimension it
materialized the columns at in a small `kb_meta(key, value)` table
(`schema_embedding_dim`), and every later apply checks against it.

`db_apply_schema_postgres()`, right after the DDL applies, calls
`db2_embedding_dim_record_or_check()`. That function first rejects a non-positive
configured dimension — returning an error *before* it touches `kb_meta`, so an
invalid dimension can never be written as the authoritative value — and then
performs a single atomic statement:

```sql
INSERT INTO kb_meta (key, value) VALUES ('schema_embedding_dim', :dim)
  ON CONFLICT (key) DO UPDATE SET value = kb_meta.value
  RETURNING value;
```

The `DO UPDATE SET value = kb_meta.value` clause deliberately writes the existing
value back unchanged (not `EXCLUDED.value`), so on a row that already exists the
recorded dimension is *preserved* and `RETURNING` yields it; on a fresh row the
just-inserted dimension is returned. The function then compares that returned
value to the configured dimension:

- **First apply** — no row yet: the configured dimension is inserted and
  returned, so it matches; the apply succeeds.
- **Matching apply** — the recorded dimension equals the configured one: no-op,
  the apply succeeds.
- **Mismatch** — the recorded dimension differs: the function returns an error,
  so `db2_init()` fails and the server/kb refuses to start, with a remediation
  message naming both dimensions and pointing at the migration procedure below
  ("Switching embedders on an existing database").

Because the record-and-read is one atomic upsert, there is no SELECT-then-INSERT
window: a deployment is never able to record one dimension while another is
already stored, so a misconfigured second start reads the committed dimension and
is refused rather than silently proceeding at the wrong dimension. If the
`kb_meta` value is ever corrupt or non-numeric (e.g. hand-edited), the function
returns an error reporting it for manual repair rather than silently treating it
as "no recorded dimension" and overwriting it. The check is written against the
`aimee_pg_*` abstraction layer, so the same logic runs on Postgres in production
and is exercised against the SQLite test shim; see
`src/tests/test_embedding_dim.c`.

## Switching embedders on an existing database

`db2_init()` never reshapes an existing column (a routine schema-apply must not
touch the corpus). Changing `embedding_dim` against a populated database to a
**different dimension** is now refused at startup by the dimension-drift guard
above — the server/kb will not come up until the corpus and the config agree,
precisely because the alternative (booting and serving) silently breaks search.
Switching dimension (0.6b 1024 ⇆ 4b 2560) therefore requires **re-embedding** the
corpus at the new dimension. Note that the schema-apply uses
`CREATE TABLE IF NOT EXISTS`, so it will **not** alter an existing table — simply
deleting the vector *rows* leaves the `halfvec` columns at their old dimension. To
move the dimension the dim-sized tables must be **dropped** so the next
schema-apply recreates them at the new `embedding_dim`. Each one is a derived
embedding/index table (a `point_id` key plus the `halfvec` vector and denormalized
lookup columns); their contents are rebuilt by re-embedding from the source rows
(`kb_documents`, memories, code units, and the curator artifacts), so dropping
them loses only the derived vectors, not the source corpus.

Back up DB2 first, then, with the kb **stopped** (so nothing reads or rewrites the
columns mid-migration):

1. Set the new `embedding_dim` (config or `AIMEE_EMBEDDING_DIM`).
2. In a single transaction, drop every dim-sized table and clear the recorded
   dimension so the next start re-records it:

   ```sql
   BEGIN;
   DROP TABLE IF EXISTS
       kb_embeddings,
       memory_embeddings,
       code_embeddings,
       curator_entity_vectors,
       curator_narrative_vectors,
       curator_claim_vectors,
       curator_code_unit_vectors,
       evidence_vectors,
       exemplar_vectors;
   DELETE FROM kb_meta WHERE key = 'schema_embedding_dim';
   COMMIT;
   ```

   (Drop the tables and clear `schema_embedding_dim` together: leaving the row set
   makes the next start refuse, while clearing it without dropping the tables would
   re-record the new dimension against columns still sized at the old one — exactly
   the drift the guard exists to prevent.)
3. Start the kb. With the tables gone the schema-apply recreates them at the new
   `embedding_dim`, and with the `kb_meta` row gone the guard treats this as a
   first apply and re-records the new dimension. The curator drain then re-embeds,
   backfilling any source row missing a vector.

Chunk text and `file_contents` are untouched throughout. A same-dimension
`vector → halfvec` change is different: it only needs an in-place cast
(`deploy/migrations/2026-embed-halfvec.sql`) and leaves `schema_embedding_dim`
unchanged, since the dimension has not moved.

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
