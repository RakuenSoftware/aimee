-- 2026-embed-halfvec.sql
--
-- Convert every embedding column from vector (fp32) to halfvec (fp16), unifying
-- on one vector type. halfvec halves index memory + storage and speeds up HNSW
-- at negligible quality cost (fp16 ~= fp32 for normalized-embedding cosine
-- recall — ~0.999 cosine, nothing like int8). pgvector added halfvec for exactly
-- this. It also lets the live and the deep (halfvec(2560)) tiers share one type.
--
-- THIS IS A CAST, NOT A RE-EMBED: the dimension is unchanged (vector(1024) ->
-- halfvec(1024)), so the stored values are preserved in place. No re-embedding,
-- no recall downtime beyond the index rebuild. (Contrast 2026-embed-dim-1024.sql,
-- which changes the dimension and DOES require a re-embed.)
--
-- REQUIRES pgvector >= 0.7.0 (halfvec type + halfvec_cosine_ops). Verify with
--   SELECT extversion FROM pg_extension WHERE extname='vector';
-- A fresh schema apply already creates halfvec columns; this migrates an existing
-- vector(N) database. Back up DB2 first.
--
-- AFTER RUNNING: restart aimee-server/kb so schema.sql recreates the HNSW indexes
-- with halfvec_cosine_ops (CREATE INDEX IF NOT EXISTS). Until then vector search
-- falls back to a sequential scan (correct, just slower).
--
-- Idempotent: re-running is a no-op once every embedding column is already halfvec.

BEGIN;

-- Fail loudly if halfvec is unavailable rather than half-applying.
DO $CHECK$
BEGIN
    PERFORM 'halfvec'::regtype;
EXCEPTION WHEN undefined_object THEN
    RAISE EXCEPTION 'halfvec unification needs pgvector >= 0.7.0 (halfvec type missing). Upgrade pgvector, then re-run.';
END
$CHECK$;

-- Drop the pgvector indexes on the embedding columns; ALTER COLUMN TYPE cannot
-- proceed while an index depends on the column. schema.sql recreates them with
-- halfvec_cosine_ops on the next startup.
DROP INDEX IF EXISTS idx_memory_embeddings_hnsw;
DROP INDEX IF EXISTS idx_memory_embeddings_deep_hnsw;
DROP INDEX IF EXISTS idx_kb_embeddings_hnsw;
DROP INDEX IF EXISTS idx_code_embeddings_hnsw;
DROP INDEX IF EXISTS idx_curator_entity_vectors_hnsw;
DROP INDEX IF EXISTS idx_curator_narrative_vectors_hnsw;
DROP INDEX IF EXISTS idx_curator_claim_vectors_subj_attr_hnsw;
DROP INDEX IF EXISTS idx_curator_claim_vectors_value_hnsw;
DROP INDEX IF EXISTS idx_curator_code_unit_vectors_intent_hnsw;
DROP INDEX IF EXISTS idx_curator_code_unit_vectors_signature_hnsw;
DROP INDEX IF EXISTS idx_curator_code_unit_vectors_body_hnsw;

-- Cast every vector(N) column to halfvec(N) in place (fp32 -> fp16; same dim, data
-- preserved). Dynamic so no column is missed.
DO $MIG$
DECLARE
    r   RECORD;
    dim text;
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
    LOOP
        dim := (regexp_match(r.typ, '\((\d+)\)'))[1];
        EXECUTE format('ALTER TABLE %I ALTER COLUMN %I TYPE halfvec(%s) USING %I::halfvec(%s)',
                       r.tbl, r.col, dim, r.col, dim);
        RAISE NOTICE 'halfvec migration: %.% % -> halfvec(%) (cast, data preserved)',
                     r.tbl, r.col, r.typ, dim;
    END LOOP;
END
$MIG$;

COMMIT;
