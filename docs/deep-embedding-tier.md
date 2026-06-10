# Deep embedding tier (4B)

An optional, opt-in second embedding tier: a larger model re-embeds memories in
the background for higher-quality retrieval, backstopping the fast live tier. The
design is "fast 0.6B initial pass + comprehensive 4B pass later."

**Default OFF.** A lite deployment runs the live 0.6B embedder alone (~0.5 GB
RAM). The deep tier adds a ~16 GB resident 4B model and a second vector index, so
it is opt-in for hosts with the RAM. The deep model is lazy — never loaded until
the tier is actually used.

## How it works

| Column | Dim | Model | Written by | Read by |
|---|---|---|---|---|
| `embedding` | `halfvec(1024)` | 0.6B (live) | every memory write | fast per-turn recall |
| `embedding_deep` | `halfvec(2560)` | 4B (deep) | background backfill | opt-in `--deep` recall |

The two columns coexist — the 4B never replaces the 0.6B. A query embeds with one
model and searches the matching column, so the dimensions never need to reconcile.
Both are `halfvec` (fp16); 2560 exceeds pgvector's 2000-dim `vector`-index cap, so
halfvec is required (it also halves index memory at negligible quality cost).

- **Embedder** serves the 4B on `/embed_deep` (env `DEEP_MODEL`, default
  `perplexity-ai/pplx-embed-v1-4b`), lazy-loaded on first call.
- **Backfill** is a throttled `memory_maintenance` mode (`MEMORY_MAINTENANCE_MODE_DEEP_EMBED`):
  each cycle deep-embeds up to 25 memories that lack `embedding_deep`, oldest
  first, so a large corpus fills in gradually. The *first* batch triggers the
  embedder's one-time ~150 s 4B load.
- **Deep recall**: `aimee memory search --deep <query>` embeds the query with the
  4B (~seconds) and searches `embedding_deep`. It reuses the existing
  `memory.find_facts_scoped` `/v1` endpoint with a `deep` flag — no new RPC.

## Enabling it

1. **pgvector ≥ 0.7** is required (halfvec). The whole stack already requires it
   once embeddings are unified on halfvec.
2. Ensure the embedding columns are halfvec: a fresh schema apply already is; an
   existing `vector(N)` database runs `deploy/migrations/2026-embed-halfvec.sql`
   (a cast, no re-embed).
3. Point the embedder at a 4B-capable host and pair the live tier with int8 (the
   4B backstops the small drift):
   ```
   AIMEE_EMBEDDER_QUANTIZE=int8        # 0.6B served int8 (~3.3x faster)
   DEEP_MODEL=perplexity-ai/pplx-embed-v1-4b
   ```
4. Flip the gate and point the deep-embed command at the sidecar:
   ```
   aimee config set memory_deep_embedding_enabled 1
   aimee config set memory_deep_embedding_command "python3 /opt/aimee/scripts/embed-deep-remote.py"
   ```
5. The backfill runs on the normal maintenance cycle. Until `embedding_deep` is
   populated, `--deep` recall returns empty; live recall is unaffected throughout.

## Cost / when to use

- A 4B query embed is ~2.7 s on CPU, so `--deep` is for occasional, quality-first
  recall — never the default per-turn path.
- The backfill is deliberately slow (throttled). It's a "take your time" pass.
- On a lite (0.6B-only) deployment, leave it off and serve fp32 (`EMBEDDER_QUANTIZE`
  unset) for full live-tier quality.
