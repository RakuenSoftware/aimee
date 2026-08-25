-- Which vector index did the schema actually build?
--
-- pgvectorscale's StreamingDiskANN is the DEFAULT, and HNSW is what a database
-- without the extension falls back to. Asserted against the database rather
-- than read out of the code, because the code said both things at once until
-- recently: schema.sql chose DiskANN when the extension was present while
-- pgvec_ensure_index created HNSW unconditionally, so which one a deployment
-- ended up with depended on which ran last.
\pset pager off
\set ON_ERROR_STOP on

DO $$
DECLARE
    has_extension  BOOLEAN;
    diskann_count  INT;
    hnsw_count     INT;
BEGIN
    SELECT EXISTS (SELECT 1 FROM pg_extension WHERE extname = 'vectorscale')
      INTO has_extension;

    SELECT count(*) INTO diskann_count
      FROM pg_indexes WHERE indexdef LIKE '%USING diskann%';
    SELECT count(*) INTO hnsw_count
      FROM pg_indexes WHERE indexdef LIKE '%USING hnsw%';

    RAISE NOTICE 'vectorscale installed: %  diskann indexes: %  hnsw indexes: %',
        has_extension, diskann_count, hnsw_count;

    IF has_extension THEN
        -- The default. Every vector index the schema builds must be DiskANN,
        -- and any HNSW left over would be a table that took the fallback while
        -- the extension was available.
        IF diskann_count = 0 THEN
            RAISE EXCEPTION 'pgvectorscale is installed but no DiskANN index was built';
        END IF;
        IF hnsw_count > 0 THEN
            RAISE EXCEPTION
                'pgvectorscale is installed and % HNSW index(es) were built anyway', hnsw_count;
        END IF;
        RAISE NOTICE 'ok: with pgvectorscale present, every vector index is DiskANN';
    ELSE
        -- The fallback. HNSW is the only method available, and a DiskANN index
        -- could not have been created at all.
        IF diskann_count > 0 THEN
            RAISE EXCEPTION 'no pgvectorscale, yet % DiskANN index(es) exist', diskann_count;
        END IF;
        IF hnsw_count = 0 THEN
            RAISE EXCEPTION 'no pgvectorscale and no HNSW index either; nothing is indexed';
        END IF;
        RAISE NOTICE 'ok: without pgvectorscale, the schema falls back to HNSW';
    END IF;
END
$$;

-- The tables the live pgvec_ensure_index path covers. These are the ones that
-- disagreed with the schema, so they are named rather than counted.
SELECT tablename, indexdef LIKE '%diskann%' AS is_diskann
  FROM pg_indexes
 WHERE tablename IN ('memory_embeddings', 'kb_embeddings', 'code_embeddings')
   AND (indexdef LIKE '%USING diskann%' OR indexdef LIKE '%USING hnsw%')
 ORDER BY tablename;
