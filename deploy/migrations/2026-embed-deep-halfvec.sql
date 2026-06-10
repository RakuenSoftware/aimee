-- 2026-embed-deep-halfvec.sql
--
-- Opt-in DB2 migration for the deep embedding tier (config
-- memory_deep_embedding_enabled). It adds the halfvec(2560) column +
-- HNSW index that hold the background 4B "comprehensive" re-embed
-- (embedder /embed_deep, DEEP_MODEL=perplexity-ai/pplx-embed-v1-4b).
--
-- WHY OPT-IN: the deep tier adds a second, ~16GB resident embedding model and a
-- second vector index. Lite deployments (the default) run the 0.6b q4 embedder
-- alone and never need this. The base schema.sql also adds the column/index
-- exception-guarded on startup, so this script is mainly for (a) enabling the
-- tier explicitly with a LOUD failure if pgvector lacks halfvec — rather than the
-- silent NOTICE a routine schema apply emits — and (b) verifying the result.
--
-- REQUIREMENTS:
--   * pgvector >= 0.7.0 (halfvec type + halfvec_cosine_ops). 2560 dims exceed
--     pgvector's 2000-dim `vector` index cap, so the deep tier uses halfvec
--     (HNSW-indexable up to 4000 dims).
--   * The embedder image must serve /embed_deep (it lazy-loads the 4B on first
--     call). Confirm GET /health reports a non-empty "deep_model".
--
-- AFTER RUNNING:
--   * Set the gate:  aimee config set memory_deep_embedding_enabled 1
--     and point memory_deep_embedding_command at the /embed_deep client.
--   * The background pass backfills embedding_deep over time; until then deep
--     recall is unavailable and normal 0.6b recall is unaffected.
--
-- Idempotent: re-running is a no-op once the column + index already exist.

BEGIN;

-- Fail loudly (not a NOTICE) if halfvec is unavailable, so an operator who
-- explicitly opted into the deep tier learns immediately rather than discovering
-- a silently-skipped column later.
DO $CHECK$
BEGIN
    PERFORM 'halfvec'::regtype;
EXCEPTION WHEN undefined_object THEN
    RAISE EXCEPTION 'deep tier needs pgvector >= 0.7.0 (halfvec type missing). Upgrade pgvector, then re-run.';
END
$CHECK$;

-- Add the deep column (no-op if present). 2560-dim halfvec; NULL until backfilled.
ALTER TABLE memory_embeddings ADD COLUMN IF NOT EXISTS embedding_deep halfvec(2560);

-- HNSW over the deep column (empty/cheap until the 4B backfill populates it).
CREATE INDEX IF NOT EXISTS idx_memory_embeddings_deep_hnsw
    ON memory_embeddings USING hnsw (embedding_deep halfvec_cosine_ops);

COMMIT;
