-- 2026-embed-vector.sql
--
-- Convert every embedding column from halfvec (fp16) back to vector (fp32).
-- This REVERSES 2026-embed-halfvec.sql, and the reason is pgvectorscale.
--
-- halfvec was adopted to make HNSW work at 2560 dimensions. pgvector's HNSW puts
-- the vector in a single 8 KB index page, so it caps at 2000 dimensions for
-- `vector` and 4000 for `halfvec` -- the same ~8000-byte budget, which is why the
-- ratio is exactly 2:1.
--
-- pgvectorscale's StreamingDiskANN is a different access method with its own
-- on-disk layout and does not inherit that cap. But it ships diskann operator
-- classes for `vector` ONLY:
--
--     ERROR:  operator class "halfvec_cosine_ops" does not exist
--             for access method "diskann"
--
-- So a halfvec column can never use diskann at all. At aimee's default 384/768
-- dimensions the storage difference is ~1.5 KB vs ~3 KB per row, and both index
-- methods work on `vector` -- so the trade is worth taking.
--
-- THIS IS A CAST, NOT A RE-EMBED: the dimension is unchanged
-- (halfvec(768) -> vector(768)), so stored values are preserved in place. fp16 ->
-- fp32 is lossless in this direction (every fp16 value is representable in fp32);
-- the original fp32 -> fp16 conversion was the lossy one, and that loss is not
-- recovered here. For normalized-embedding cosine recall the difference is
-- ~0.999 cosine, so no re-embed is required.
--
-- REQUIRES pgvector (vector type). pgvectorscale is OPTIONAL: without it the
-- indexes rebuild as HNSW, which is what they already were.
--
-- AFTER RUNNING: restart aimee-server/kb so schema.sql recreates the indexes with
-- vector_cosine_ops -- diskann where pgvectorscale is installed, HNSW otherwise
-- (CREATE INDEX IF NOT EXISTS). Until then vector search falls back to a
-- sequential scan (correct, just slower).
--
-- If your embedder's dimension exceeds what both methods can index, the schema
-- says so on apply and the answer is an external vector provider module; the
-- relational store keeps the canonical vectors either way.
--
-- Back up DB2 first. Idempotent: re-running is a no-op once every embedding
-- column is already `vector`.

BEGIN;

-- Drop the indexes on the embedding columns; ALTER COLUMN TYPE cannot proceed
-- while an index depends on the column. schema.sql recreates them on next start.
-- Both spellings are dropped: a database may carry either, depending on whether
-- pgvectorscale was present when it was last applied.
DO $DROPIDX$
DECLARE
    r RECORD;
BEGIN
    FOR r IN
        SELECT indexname
          FROM pg_indexes
         WHERE schemaname = 'public'
           AND (indexdef ILIKE '%USING hnsw%' OR indexdef ILIKE '%USING diskann%')
    LOOP
        EXECUTE format('DROP INDEX IF EXISTS %I', r.indexname);
        RAISE NOTICE 'vector migration: dropped index % (schema.sql recreates it)',
                     r.indexname;
    END LOOP;
END
$DROPIDX$;

-- Cast every halfvec(N) column to vector(N) in place. Dynamic so no column is
-- missed, and driven by the catalog rather than a hand-maintained list -- the
-- forward migration's fixed DROP list had already drifted from the schema.
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
        RAISE NOTICE 'vector migration: %.% % -> vector(%) (cast, data preserved)',
                     r.tbl, r.col, r.typ, dim;
    END LOOP;
END
$MIG$;

-- Bare, undimensioned `halfvec` columns, if any exist: same treatment as the
-- forward migration gave bare `vector`. The column carries no dimension of its
-- own, so take it from a sibling that does.
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
     WHERE n.nspname = 'public'
       AND c.relkind = 'r'
       AND NOT a.attisdropped
       AND format_type(a.atttypid, a.atttypmod) LIKE 'vector(%'
     LIMIT 1;

    IF target_dim IS NULL THEN
        RAISE NOTICE 'vector migration: no dimensioned column to copy; bare columns left for a fresh schema apply';
        RETURN;
    END IF;

    FOR r IN
        SELECT c.relname AS tbl, a.attname AS col
          FROM pg_attribute a
          JOIN pg_class c     ON c.oid = a.attrelid
          JOIN pg_namespace n ON n.oid = c.relnamespace
         WHERE n.nspname = 'public'
           AND c.relkind = 'r'
           AND NOT a.attisdropped
           AND format_type(a.atttypid, a.atttypmod) = 'halfvec'
    LOOP
        EXECUTE format('ALTER TABLE %I ALTER COLUMN %I TYPE vector(%s) USING %I::vector(%s)',
                       r.tbl, r.col, target_dim, r.col, target_dim);
        RAISE NOTICE 'vector migration: %.% bare halfvec -> vector(%)',
                     r.tbl, r.col, target_dim;
    END LOOP;
END
$BARE$;

COMMIT;
