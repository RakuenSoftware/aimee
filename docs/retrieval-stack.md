# Retrieval stack (embedder, reranker, halfvec)

The live retrieval foundation: the embedding model, the cross-encoder reranker,
CPU serving, and the on-disk vector format. The optional higher-quality 4B layer
on top is documented in [deep-embedding-tier.md](deep-embedding-tier.md).

## Live embedder — pplx-embed-v1-0.6b (1024-dim)

The default embedder is **`perplexity-ai/pplx-embed-v1-0.6b`** (Feb 2026, MIT,
Qwen3-based, 1024-dim, retrieval-optimized), replacing the 2021
`all-MiniLM-L6-v2` (384-dim). It is **prefix-free** — no query/passage role
strings to thread through — so the integration just embeds text.

- `embedding_dim = 1024` (config). The schema embedding columns are 1024-dim.
- Served by the persistent embedder sidecar (`scripts/embedder-server.py`) over
  HTTP `POST /embed` (raw UTF-8 text in, JSON float array out). The kb talks to it
  via `scripts/embed-remote.py` (`embedding_command`).
- Override the model at build/run time with `EMBEDDER_MODEL`; it must match
  `embedding_dim` and the schema vector columns.

### CPU serving and the int8 toggle

Measured on a 32-core host (per embed of a short text):

| Mode | ms/embed | Notes |
|---|---|---|
| fp32 @ 32 threads | 269 | a single short embed doesn't scale past ~8 threads |
| fp32 @ 8 threads (default) | 190 | `EMBEDDER_THREADS` default `min(8, ncpu)` |
| **int8 dynamic @ 8 threads** | **58** | `EMBEDDER_QUANTIZE=int8` — ~3.3x faster |

`EMBEDDER_QUANTIZE=int8` uses torch dynamic quantization (pure torch — no
`optimum`/ONNX, so it composes with the reranker's `transformers` requirement).
int8 drifts the embedding ~0.90 cosine vs fp32, so the **policy** follows the
tiers' roles:

- **fp32** (default) when the 0.6B is the only tier — it *is* the answer, so it
  must be full quality.
- **int8** when the 4B deep tier is enabled — the 0.6B is then only the *fast
  interim* (the authoritative answer comes from the 4B), so trading a little of its
  precision for ~3.3x speed is exactly the point: get the user something fast, let
  the authoritative 4B follow. This is the whole reason the 0.6B runs reduced-
  precision. See [deep-embedding-tier.md](deep-embedding-tier.md).

The embedder is config-decoupled, so this is realized at deploy time: the
compose embedder service reads `EMBEDDER_QUANTIZE` from
`${AIMEE_EMBEDDER_QUANTIZE:-fp32}`. (A q4 ONNX path was evaluated and dropped — it
needs `optimum`, which pins `transformers` below what the reranker requires.)

## Cross-encoder reranker — Ettin (default off)

A second-pass reranker scores `(query, candidate)` pairs after the bi-encoder
recall. Default model: **`cross-encoder/ettin-reranker-400m-v1`** (JHU, May 2026,
Apache-2.0, ModernBERT). It is **lazy-loaded** in the embedder process and served
on `POST /rerank` (JSON `[[query, cand], ...]` in, JSON scores out); the kb calls
it via `scripts/rerank-remote.py`.

- Default **off**: enable with
  `aimee config set memory_rerank_enabled 1` +
  `memory_rerank_command "python3 /opt/aimee/scripts/rerank-remote.py"`. On any
  failure the C caller silently falls back to hybrid ordering.
- Ettin needs **`transformers >= 5.2`** (older versions can't instantiate its
  ModernBERT tokenizer); the embedder image pins it.

## halfvec — all embedding columns are fp16

Every embedding column is **`halfvec`** (pgvector's fp16 vector type), not
`vector` (fp32). halfvec uses ~half the index memory and storage and speeds up
HNSW, at negligible quality cost for normalized-embedding cosine recall (~0.999
cosine vs fp32 — nothing like the int8 drift). It also lets the live `halfvec(1024)`
and deep `halfvec(2560)` tiers share one type (2560 exceeds pgvector's 2000-dim
`vector`-index cap, so the deep tier *requires* halfvec regardless).

- **Requires pgvector ≥ 0.7.0** system-wide (halfvec was added there). Verified on
  the operational DB (pgvector 0.8.x).
- A fresh schema apply creates halfvec columns directly.

## Migrations

| Migration | What | Re-embed? |
|---|---|---|
| `deploy/migrations/2026-embed-dim-1024.sql` | MiniLM 384-dim → pplx 1024-dim (clears + reshapes the vector columns) | **Yes** — the 384-dim vectors are not convertible |
| `deploy/migrations/2026-embed-halfvec.sql` | `vector(N)` → `halfvec(N)` (in-place cast) + rebuild HNSW | No — same dim, fp32→fp16 cast |
| `deploy/migrations/2026-embed-deep-halfvec.sql` | add the opt-in deep columns/indexes (memory + curator) | No |

**Deploy ordering for the halfvec cut:** an existing `vector(N)` database must run
`2026-embed-halfvec.sql` *before* the new code — a `::halfvec` upsert cannot write
to a `vector` column. Back up DB2 first. Both migrations are idempotent.

## Embedder service summary

`scripts/embedder-server.py` (one process, lazy models):

| Route | Serves |
|---|---|
| `POST /embed` | live 0.6B embedding (1024-dim) |
| `POST /embed_deep` | optional 4B deep embedding (2560-dim), lazy |
| `POST /rerank` | optional Ettin cross-encoder scores, lazy |
| `GET /health` | `{model, dim, quantize, deep_model, deep_dim}` |

Env: `EMBEDDER_MODEL`, `DEEP_MODEL`, `RERANKER_MODEL`, `EMBEDDER_THREADS`,
`EMBEDDER_QUANTIZE`, `EMBEDDER_PORT`.
