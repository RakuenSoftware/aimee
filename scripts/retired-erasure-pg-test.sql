-- Retired functions must be absent on fresh and upgraded stores. No data is
-- deleted by this compatibility cleanup; historical receipt tables are retained.
DO $$
DECLARE routine TEXT;
BEGIN
 FOREACH routine IN ARRAY ARRAY[
 'kb_subject_erasure_begin(text,text,jsonb)',
 'kb_subject_erasure_complete(text,text,bigint)',
 'kb_subject_erasure_ack(text,text,text,bigint)',
 'kb_subject_erasure_require_owners(text)',
 'kb_memory_retention_reap(integer)',
 'kb_memory_sensitivity_retention_reap(text,integer)',
 'kb_document_retention_reap(integer)'] LOOP
  IF to_regprocedure(routine) IS NOT NULL THEN
   RAISE EXCEPTION 'retired destructive routine remains callable: %',routine;
  END IF;
 END LOOP;
END $$;
\echo 'retired-erasure-pg: PASS'
