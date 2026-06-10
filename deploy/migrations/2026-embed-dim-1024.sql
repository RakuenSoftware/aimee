-- 2026-embed-dim-1024.sql
--
-- One-time DB2 (Postgres + pgvector) migration for the embedder change from
-- all-MiniLM-L6-v2 (384-dim) to perplexity-ai/pplx-embed-v1-0.6b (1024-dim).
--
-- WHY THIS IS A SEPARATE, MANUAL SCRIPT (not auto-applied on startup):
--   The old 384-dim vectors are NOT convertible to 1024 dims — switching the
--   embedder means the entire corpus must be re-embedded. This script CLEARS the
--   stored embeddings (sets the vector columns to NULL) and reshapes the columns
--   to vector(1024); the rows/metadata are kept. The base schema deliberately
--   only WARNS about a dimension mismatch rather than doing this automatically,
--   so a routine restart can never silently wipe the corpus.
--
-- BEFORE RUNNING:
--   1. BACK UP DB2 (pg_dump) — this clears every stored embedding.
--   2. Deploy the new embedder image (EMBEDDER_MODEL=perplexity-ai/pplx-embed-v1-0.6b)
--      and confirm /health reports "dim": 1024.
--   Ideally test on a restored copy of the DB first.
--
-- AFTER RUNNING:
--   3. Restart aimee-server/kb so schema.sql rebuilds the HNSW indexes on the
--      now-1024-dim columns (CREATE INDEX IF NOT EXISTS).
--   4. Re-embed the corpus:  aimee memory reembed --start  (then --cutover),
--      or let normal ingestion/maintenance repopulate embeddings over time.
--   Until re-embedded, semantic recall is degraded (lexical/BM25 still works).
--
-- Idempotent: re-running is a no-op once every vector column is already
-- vector(1024).

BEGIN;

-- Drop the pgvector (HNSW) indexes that sit on the embedding columns; an
-- ALTER COLUMN TYPE that changes the dimension cannot proceed while an index
-- depends on the column. They are recreated by schema.sql on the next startup.
DROP INDEX IF EXISTS idx_memory_embeddings_hnsw;
DROP INDEX IF EXISTS idx_kb_embeddings_hnsw;
DROP INDEX IF EXISTS idx_code_embeddings_hnsw;
DROP INDEX IF EXISTS idx_curator_entity_vectors_hnsw;
DROP INDEX IF EXISTS idx_curator_narrative_vectors_hnsw;
DROP INDEX IF EXISTS idx_curator_claim_vectors_subj_attr_hnsw;
DROP INDEX IF EXISTS idx_curator_claim_vectors_value_hnsw;
DROP INDEX IF EXISTS idx_curator_code_unit_vectors_intent_hnsw;
DROP INDEX IF EXISTS idx_curator_code_unit_vectors_signature_hnsw;
DROP INDEX IF EXISTS idx_curator_code_unit_vectors_body_hnsw;

-- Reshape every `vector(N != 1024)` column to vector(1024), clearing the old
-- (incompatible-dimension) embeddings. Done dynamically so no column is missed.
DO $MIG$
DECLARE
    r RECORD;
BEGIN
    FOR r IN
        SELECT c.relname AS tbl, a.attname AS col,
               format_type(a.atttypid, a.atttypmod) AS typ
          FROM pg_attribute a
          JOIN pg_class c     ON c.oid = a.attrelid
          JOIN pg_namespace n ON n.oid = c.relnamespace
         WHERE n.nspname = 'public'
           AND c.relkind = 'r'
           AND NOT a.attisdropped
           AND format_type(a.atttypid, a.atttypmod) LIKE 'vector(%'
           AND format_type(a.atttypid, a.atttypmod) <> 'vector(1024)'
    LOOP
        EXECUTE format('ALTER TABLE %I ALTER COLUMN %I TYPE vector(1024) USING NULL',
                       r.tbl, r.col);
        RAISE NOTICE 'embed-dim migration: %.% % -> vector(1024) (cleared; re-embed required)',
                     r.tbl, r.col, r.typ;
    END LOOP;
END
$MIG$;

-- Reset the re-embed rollover bookkeeping so a fresh `aimee memory reembed
-- --start` re-embeds from the beginning against the new embedder.
UPDATE memory_reembed_progress SET last_id = 0, done = 0, finished_at = NULL WHERE id = 1;

COMMIT;
