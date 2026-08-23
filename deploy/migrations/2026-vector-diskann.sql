-- 2026-vector-diskann.sql
--
-- Move every embedding column from halfvec (fp16) back to vector (fp32) and
-- hand the indexes to pgvectorscale's diskann. This reverses the type half of
-- 2026-embed-halfvec.sql, and it is not a reversal of judgement: halfvec was
-- the right choice while pgvector's HNSW was the index, and it stops being an
-- option the moment diskann is.
--
-- WHY THE TYPE HAS TO CHANGE. pgvectorscale registers no operator class for
-- halfvec. Its pinned 0.9.0 release declares exactly four -- vector_cosine_ops,
-- vector_l2_ops, vector_ip_ops, vector_smallint_label_ops -- and defines no
-- types of its own (vectorscale.control: `requires = 'vector'`). So
--     CREATE INDEX ... USING diskann (embedding halfvec_cosine_ops)
-- cannot be created at all; Postgres answers "operator class halfvec_cosine_ops
-- does not exist for access method diskann". The tree built exactly that
-- statement, inside an exception handler that logged a NOTICE and continued, so
-- every diskann index silently failed and left the table with no vector index
-- and a sequential scan behind every search.
--
-- THIS IS NOT A DOWNGRADE. Measured against pgvector 0.8.0 + pgvectorscale
-- 0.9.0, diskann indexes `vector` all the way to the type's own 16000-dimension
-- ceiling (1024, 4096, 8192 and 16000 all build; an 8192-dim row round-trips
-- through a cosine query). pgvector's HNSW refuses vector above 2000 and
-- halfvec above 4000. The indexable ceiling therefore goes UP, from 4000 to
-- 16000. Above 16000 no in-Postgres type can hold the vector and an external
-- vector database module is required.
--
-- THE COST IS STORAGE. vector is 4 bytes per dimension against halfvec's 2, so
-- every embedding column doubles on disk. At 1024 dims that is 4 KB per row
-- instead of 2 KB. Budget for it before running this on a large corpus.
--
-- THIS IS A CAST, NOT A RE-EMBED: the dimension is unchanged
-- (halfvec(1024) -> vector(1024)), so stored values are preserved in place --
-- fp16 -> fp32 is exact, every fp16 value being representable in fp32. No
-- re-embedding and no recall change beyond the index rebuild.
--
-- REQUIRES pgvectorscale (the `vectorscale` extension), which in turn requires
-- pgvector. Verify with
--   SELECT extname, extversion FROM pg_extension WHERE extname IN ('vector','vectorscale');
--
-- AFTER RUNNING: restart aimee-server/kb so schema.sql creates the diskann
-- indexes (CREATE INDEX IF NOT EXISTS). Until then vector search falls back to
-- a sequential scan -- correct, just slower.
--
-- Idempotent: re-running is a no-op once every embedding column is vector.
--
-- Back up DB2 first.

BEGIN;

-- Fail loudly rather than half-applying. A database that cannot build a diskann
-- index has nowhere to put these columns' indexes after the cast.
DO $CHECK$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_extension WHERE extname = 'vectorscale') THEN
        RAISE EXCEPTION 'diskann migration needs the vectorscale extension (pgvectorscale). Install it and run CREATE EXTENSION vectorscale, then re-run.';
    END IF;
    PERFORM 'vector'::regtype;
EXCEPTION WHEN undefined_object THEN
    RAISE EXCEPTION 'diskann migration needs pgvector (vector type missing); pgvectorscale requires it too. Install pgvector, then re-run.';
END
$CHECK$;

-- Drop the pgvector HNSW indexes. ALTER COLUMN TYPE cannot proceed while an
-- index depends on the column, and these index an operator class the new type
-- does not use. schema.sql creates the diskann replacements on next startup.
DROP INDEX IF EXISTS idx_memory_embeddings_hnsw;
DROP INDEX IF EXISTS idx_memory_embeddings_deep_hnsw;
DROP INDEX IF EXISTS idx_kb_embeddings_hnsw;
DROP INDEX IF EXISTS idx_kb_pdf_embeddings_hnsw;
DROP INDEX IF EXISTS idx_code_embeddings_hnsw;
DROP INDEX IF EXISTS idx_curator_entity_vectors_hnsw;
DROP INDEX IF EXISTS idx_curator_narrative_vectors_hnsw;
DROP INDEX IF EXISTS idx_curator_claim_vectors_subj_attr_hnsw;
DROP INDEX IF EXISTS idx_curator_claim_vectors_value_hnsw;
DROP INDEX IF EXISTS idx_curator_code_unit_vectors_intent_hnsw;
DROP INDEX IF EXISTS idx_curator_code_unit_vectors_signature_hnsw;
DROP INDEX IF EXISTS idx_curator_code_unit_vectors_body_hnsw;

-- Corpus tables carry their own per-table index, created by
-- pgvec_ensure_corpus_index rather than by schema.sql. Drop those too: they are
-- named for the method that built them and the module recreates them.
DO $CORPUS$
DECLARE
    r RECORD;
BEGIN
    FOR r IN
        SELECT c.relname AS idx
          FROM pg_class c
          JOIN pg_namespace n ON n.oid = c.relnamespace
         WHERE n.nspname = 'public'
           AND c.relkind = 'i'
           AND c.relname LIKE 'idx\_%\_hnsw'
    LOOP
        EXECUTE format('DROP INDEX IF EXISTS %I', r.idx);
        RAISE NOTICE 'diskann migration: dropped corpus index %', r.idx;
    END LOOP;
END
$CORPUS$;

-- Cast every halfvec(N) column to vector(N) in place. Dynamic so no column is
-- missed -- corpus tables are created at runtime and are not named here.
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
           AND format_type(a.atttypid, a.atttypmod) LIKE 'halfvec(%'
    LOOP
        dim := (regexp_match(r.typ, '\((\d+)\)'))[1];
        EXECUTE format('ALTER TABLE %I ALTER COLUMN %I TYPE vector(%s) USING %I::vector(%s)',
                       r.tbl, r.col, dim, r.col, dim);
        RAISE NOTICE 'diskann migration: %.% % -> vector(%) (cast, data preserved)',
                     r.tbl, r.col, r.typ, dim;
    END LOOP;
END
$MIG$;

-- A bare, undimensioned `halfvec` column cannot be indexed by anything and the
-- dimensioned loop above skips it, since format_type reports it as plain
-- `halfvec`. Cast it at the dimension the rest of the corpus already uses,
-- read from a sibling column, so this script stays config-free.
DO $BARE$
DECLARE
    r          RECORD;
    target_dim text;
BEGIN
    SELECT (regexp_match(format_type(a.atttypid, a.atttypmod), '\((\d+)\)'))[1]
      INTO target_dim
      FROM pg_attribute a
      JOIN pg_class c     ON c.oid = a.attrelid
      JOIN pg_namespace n ON n.oid = c.relnamespace
     WHERE n.nspname = 'public' AND c.relkind = 'r' AND NOT a.attisdropped
       AND format_type(a.atttypid, a.atttypmod) LIKE 'vector(%'
     LIMIT 1;

    IF target_dim IS NULL THEN
        RAISE NOTICE 'diskann migration: no dimensioned vector column to take a reference dimension from; bare columns left for a fresh schema apply';
        RETURN;
    END IF;

    FOR r IN
        SELECT c.relname AS tbl, a.attname AS col
          FROM pg_attribute a
          JOIN pg_class c     ON c.oid = a.attrelid
          JOIN pg_namespace n ON n.oid = c.relnamespace
         WHERE n.nspname = 'public' AND c.relkind = 'r' AND NOT a.attisdropped
           AND format_type(a.atttypid, a.atttypmod) IN ('halfvec', 'vector')
    LOOP
        EXECUTE format('ALTER TABLE %I ALTER COLUMN %I TYPE vector(%s) USING %I::vector(%s)',
                       r.tbl, r.col, target_dim, r.col, target_dim);
        RAISE NOTICE 'diskann migration: %.% (undimensioned) -> vector(%)',
                     r.tbl, r.col, target_dim;
    END LOOP;
END
$BARE$;

COMMIT;

-- Report what the columns are now, so the operator can see the migration landed
-- without having to know which tables to look in.
DO $REPORT$
DECLARE
    remaining int;
BEGIN
    SELECT count(*) INTO remaining
      FROM pg_attribute a
      JOIN pg_class c     ON c.oid = a.attrelid
      JOIN pg_namespace n ON n.oid = c.relnamespace
     WHERE n.nspname = 'public' AND c.relkind = 'r' AND NOT a.attisdropped
       AND format_type(a.atttypid, a.atttypmod) LIKE 'halfvec%';
    IF remaining > 0 THEN
        RAISE WARNING 'diskann migration: % halfvec column(s) still present', remaining;
    ELSE
        RAISE NOTICE 'diskann migration: every embedding column is vector; restart aimee to build the diskann indexes';
    END IF;
END
$REPORT$;
