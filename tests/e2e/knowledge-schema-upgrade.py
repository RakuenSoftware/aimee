#!/usr/bin/env python3
"""Test schema 45 against persisted erasure states in a disposable replay DB.

Requires initialized AIMEE_KB_STORE_REPLAY_URL. All fixture changes roll back.
"""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def migration_test_sql():
    schema = (ROOT / 'src/modules/kb/c/schema.sql').read_text().replace(
        '__EMBED_DIM__', os.environ.get('EMBEDDER_DIMS', '384'))
    before = """
BEGIN;
ALTER TABLE kb_subject_erasure_request DROP CONSTRAINT kb_subject_erasure_request_state_check;
INSERT INTO kb_subject_erasure_request(request_id,subject_digest,state,memory_count,document_count,db1_count)
SELECT 'schema45-fixture-'||state,encode(sha256(convert_to('schema45-subject','UTF8')),'hex'),state,7,3,2
FROM unnest(ARRAY['pending','db2_done','completed']) state;
ALTER TABLE kb_subject_erasure_request ADD CONSTRAINT kb_subject_erasure_request_state_check
 CHECK(state IN ('pending','db2_done','completed'));
"""
    after = """
DO $$ DECLARE result RECORD; BEGIN
 IF (SELECT count(*) FROM kb_subject_erasure_request WHERE request_id LIKE 'schema45-fixture-%'
      AND memory_count=7 AND document_count=3 AND db1_count=2) <> 3 THEN
  RAISE EXCEPTION 'migration changed saved erasure counts'; END IF;
 IF (SELECT state FROM kb_subject_erasure_request WHERE request_id='schema45-fixture-db2_done') <> 'knowledge_done'
 OR (SELECT state FROM kb_subject_erasure_request WHERE request_id='schema45-fixture-pending') <> 'pending'
 OR (SELECT state FROM kb_subject_erasure_request WHERE request_id='schema45-fixture-completed') <> 'completed' THEN
  RAISE EXCEPTION 'migration did not preserve erasure progress'; END IF;
 BEGIN
  UPDATE kb_subject_erasure_request SET state='db2_done' WHERE request_id='schema45-fixture-pending';
  RAISE EXCEPTION 'retired state accepted';
 EXCEPTION WHEN check_violation THEN NULL; END;
 SELECT * INTO result FROM kb_subject_erasure_begin('schema45-fixture-db2_done','schema45-subject','[]');
 IF NOT result.already_done OR result.deleted_memories<>7 OR result.deleted_documents<>3 THEN
  RAISE EXCEPTION 'resumed erasure lost saved counts or replay status'; END IF;
 SELECT * INTO result FROM kb_subject_erasure_begin('schema45-fixture-completed','schema45-subject','[]');
 IF NOT result.already_done OR result.deleted_memories<>7 OR result.deleted_documents<>3 THEN
  RAISE EXCEPTION 'completed erasure replay changed'; END IF;
END $$;
ROLLBACK;
"""
    return before + schema + '\n' + schema + '\n' + after


def main():
    env = dict(os.environ)
    # psql expands a URI supplied as dbname; PGDATABASE is only a literal
    # database-name default and does not select the URI's host or credentials.
    result = subprocess.run(['psql', '--dbname', env['AIMEE_KB_STORE_REPLAY_URL'],
                             '-X', '-q', '-v', 'ON_ERROR_STOP=1'],
                            input=migration_test_sql(), text=True, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    print(('PASS' if result.returncode == 0 else 'FAIL') +
          ' schema 45 migration, reapply, saved counts and erasure replay')
    if result.returncode:
        print('PostgreSQL connection failed' if result.returncode == 2 else
              'PostgreSQL rejected the schema upgrade regression transaction')
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
