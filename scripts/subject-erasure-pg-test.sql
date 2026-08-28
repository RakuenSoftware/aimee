\set ON_ERROR_STOP on

-- Destructive-path acceptance test for ACG-031. Run as the migration/database
-- administrator against a disposable database after schema.sql, schema_roles.sql,
-- and schema_grants.sql. The transaction is always rolled back.
BEGIN;

DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_runtime'
                   AND NOT rolbypassrls AND NOT rolsuper) THEN
    RAISE EXCEPTION 'runtime role is not the expected NOBYPASSRLS authority';
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_privacy_erasure'
                   AND NOT rolcanlogin AND NOT rolinherit AND rolbypassrls
                   AND NOT rolsuper) THEN
    RAISE EXCEPTION 'privacy erasure definer posture is invalid';
  END IF;
  IF EXISTS (SELECT 1 FROM pg_auth_members m
              JOIN pg_roles granted ON granted.oid=m.roleid
              JOIN pg_roles member ON member.oid=m.member
             WHERE granted.rolname='aimee_kb_privacy_erasure'
                OR member.rolname='aimee_kb_privacy_erasure') THEN
    RAISE EXCEPTION 'privacy erasure definer has a membership edge';
  END IF;
END $$;

SET LOCAL session_replication_role=replica;
INSERT INTO memories(id,key,content,owner_principal,source_session) VALUES
  (-92001,'subject-erasure-a','secret-a','erase-a@example.test','erase-db1-a'),
  (-92002,'subject-erasure-b','keep-b','erase-b@example.test','erase-db1-b');
INSERT INTO memories(id,key,content,owner_principal,updated_at) VALUES
  (-92601,'retention-old','old secret','retention-a','2000-01-01 00:00:00'),
  (-92602,'retention-new','new keep','retention-b',pg_now_text());
INSERT INTO memories(id,key,content,owner_principal,sensitivity,created_at,updated_at) VALUES
  (-92603,'retention-restricted','restricted old','retention-c','restricted',
   '2000-01-01 00:00:00',pg_now_text());
INSERT INTO memory_units(id,memory_id,unit_type,unit_text)
  VALUES(-92101,-92001,'fact','derived secret');
INSERT INTO kb_documents(id,project,file_path,file_hash,chunk_index,content,owner_principal)
  VALUES(-92201,'erase-project-a','erase-a.md','ha',0,'secret document','erase-a@example.test'),
        (-92202,'erase-project-b','erase-b.md','hb',0,'keep document','erase-b@example.test');
INSERT INTO kb_documents(id,project,file_path,file_hash,chunk_index,content,owner_principal,updated_at)
  VALUES(-92611,'retention-project-a','retention-old.md','rho',0,'old document',
           'retention-a','2000-01-01 00:00:00'),
        (-92612,'retention-project-b','retention-new.md','rhn',0,'new document',
           'retention-b',pg_now_text());
INSERT INTO kb_file_index(id,project,file_path,file_hash,owner_principal)
  VALUES(-92301,'erase-project-a','erase-a.md','ha','erase-a@example.test'),
        (-92302,'erase-project-b','erase-b.md','hb','erase-b@example.test');
INSERT INTO kb_file_index(id,project,file_path,file_hash,owner_principal)
  VALUES(-92621,'retention-project-a','retention-old.md','rho','retention-a'),
        (-92622,'retention-project-b','retention-new.md','rhn','retention-b');
INSERT INTO kb_doc_regions(id,chunk_id,document_key,page_no,quote,line_index)
  VALUES(-92401,-92201,'erase-a.md',1,'derived region',0),
        (-92631,-92611,'retention-old.md',1,'old derived region',0);
INSERT INTO kb_table_cells(id,region_id,document_key,page_no,cell_text)
  VALUES(-92501,-92401,'erase-a.md',1,'derived cell'),
        (-92641,-92631,'retention-old.md',1,'old derived cell');
INSERT INTO derived_memory_registry(derived_kind,derived_memory_id,current_status)
  VALUES('summary','subject-erasure-derived','fresh'),
        ('summary','retention-derived','fresh');
INSERT INTO derived_memory_dependencies(derived_kind,derived_memory_id,input_kind,input_id)
  VALUES('summary','subject-erasure-derived','document','-92201');
INSERT INTO derived_memory_dependencies(derived_kind,derived_memory_id,input_kind,input_id)
  VALUES('summary','retention-derived','document','-92611');
SET LOCAL session_replication_role=origin;

ALTER TABLE kb_documents ENABLE ROW LEVEL SECURITY;
ALTER TABLE kb_documents FORCE ROW LEVEL SECURITY;
ALTER TABLE kb_file_index ENABLE ROW LEVEL SECURITY;
ALTER TABLE kb_file_index FORCE ROW LEVEL SECURITY;

-- PostgreSQL owns the immutable producer outbox; the separately credentialed
-- worker owns the SQLite WORM chain. Preserve every pre-existing intent across
-- the destructive operation and leave chain construction outside this session.
CREATE TEMP TABLE subject_erasure_prior_outbox ON COMMIT DROP AS
  SELECT * FROM kb_audit_outbox;

SET LOCAL ROLE aimee_kb_runtime;
-- schema_grants.sql deliberately gives the runtime role a hardened default
-- search_path. Pin the test's application schema explicitly after SET ROLE so
-- this exercises privileges/RLS rather than failing name lookup first.
SET LOCAL search_path = public, pg_catalog;
DO $$
DECLARE first_run RECORD; second_run RECORD; created BOOLEAN;
        sensitivity_reaped BIGINT; memory_reaped BIGINT; document_reaped BIGINT;
BEGIN
  IF (SELECT count(*) FROM kb_documents WHERE owner_principal='erase-a@example.test')<>0 THEN
    RAISE EXCEPTION 'runtime bypassed content RLS';
  END IF;
  SELECT * INTO first_run FROM kb_subject_erasure_begin(
    'erase-acceptance-0123456789','erase-a@example.test','["erase-db1-a"]'::JSONB);
  IF first_run.deleted_memories<>1 OR first_run.deleted_documents<>1 OR first_run.already_done THEN
    RAISE EXCEPTION 'unexpected first erasure result: %',row_to_json(first_run);
  END IF;
  SELECT * INTO second_run FROM kb_subject_erasure_begin(
    'erase-acceptance-0123456789','erase-a@example.test','["erase-db1-a"]'::JSONB);
  IF second_run.deleted_memories<>1 OR second_run.deleted_documents<>1 OR NOT second_run.already_done THEN
    RAISE EXCEPTION 'erasure retry was not idempotent: %',row_to_json(second_run);
  END IF;
  created := kb_subject_erasure_complete('erase-acceptance-0123456789','privacy-test-operator',1);
  IF NOT created OR kb_subject_erasure_complete(
      'erase-acceptance-0123456789','privacy-test-operator',1) THEN
    RAISE EXCEPTION 'completion was not exactly once';
  END IF;
  sensitivity_reaped := kb_memory_sensitivity_retention_reap('restricted',30);
  memory_reaped := kb_memory_retention_reap(90);
  document_reaped := kb_document_retention_reap(90);
  IF sensitivity_reaped<>1 OR memory_reaped<>1 OR document_reaped<>1 THEN
    RAISE EXCEPTION 'unexpected retention counts: sensitivity %, memories %, documents %',
      sensitivity_reaped,memory_reaped,document_reaped;
  END IF;
END $$;
RESET ROLE;

DO $$
BEGIN
  IF EXISTS (SELECT 1 FROM memories WHERE owner_principal='erase-a@example.test') OR
     EXISTS (SELECT 1 FROM kb_documents WHERE owner_principal='erase-a@example.test') OR
     EXISTS (SELECT 1 FROM derived_memory_registry
              WHERE derived_memory_id='subject-erasure-derived') OR
     EXISTS (SELECT 1 FROM kb_doc_regions WHERE id=-92401) OR
     EXISTS (SELECT 1 FROM kb_table_cells WHERE id=-92501) THEN
    RAISE EXCEPTION 'subject mutable or derived data remains';
  END IF;
  IF NOT EXISTS (SELECT 1 FROM memories WHERE owner_principal='erase-b@example.test') OR
     NOT EXISTS (SELECT 1 FROM kb_documents WHERE owner_principal='erase-b@example.test') THEN
    RAISE EXCEPTION 'another subject was erased';
  END IF;
  IF EXISTS (SELECT 1 FROM memories WHERE id IN (-92601,-92603)) OR
     EXISTS (SELECT 1 FROM kb_documents WHERE id=-92611) OR
     EXISTS (SELECT 1 FROM derived_memory_registry WHERE derived_memory_id='retention-derived') OR
     EXISTS (SELECT 1 FROM kb_doc_regions WHERE id=-92631) OR
     EXISTS (SELECT 1 FROM kb_table_cells WHERE id=-92641) OR
     NOT EXISTS (SELECT 1 FROM memories WHERE id=-92602) OR
     NOT EXISTS (SELECT 1 FROM kb_documents WHERE id=-92612) THEN
    RAISE EXCEPTION 'retention did not delete only expired mutable/derived content';
  END IF;
  IF (SELECT count(*) FROM kb_audit_outbox WHERE action='subject.erase.completed'
        AND subject='erase-acceptance-0123456789')<>1 THEN
    RAISE EXCEPTION 'completion intent count is not one';
  END IF;
  IF EXISTS (SELECT 1 FROM kb_subject_erasure_request r
              WHERE row_to_json(r)::TEXT LIKE '%erase-a@example.test%') OR
     EXISTS (SELECT 1 FROM kb_audit_outbox WHERE action='subject.erase.completed'
              AND detail LIKE '%erase-a@example.test%') THEN
    RAISE EXCEPTION 'raw subject leaked into the erasure journal/evidence';
  END IF;
  IF EXISTS (
    SELECT 1
      FROM subject_erasure_prior_outbox prior
      LEFT JOIN kb_audit_outbox current USING (outbox_id)
     WHERE current.outbox_id IS NULL
        OR to_jsonb(current) IS DISTINCT FROM to_jsonb(prior)) THEN
    RAISE EXCEPTION 'a prior audit outbox intent changed';
  END IF;
END $$;

ROLLBACK;
\echo 'subject-erasure-pg: PASS'
