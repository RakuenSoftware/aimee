#!/usr/bin/env python3
"""Real PostgreSQL checks for representation migration and immutable history.

Run only against a disposable Docker daemon. No application or existing volume
is used. Vault transport is a deterministic in-process fixture here; the release
qualification additionally verifies the real authenticated C Vault.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import tempfile
import unittest
import uuid
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/upgrade'))
import memory_custody as custody


class VaultFixture:
    def __init__(self):
        self.values = {}

    def call(self, verb, body):
        assert verb == 'screen_content'
        text = body['content']
        if body.get('key') == 'retained_payload_secret':
            ref = '[[vault:v1:'+str(uuid.UUID(hashlib.sha256(text.encode()).hexdigest()[:32]))+']]'
            self.values[ref] = text
            return 200,dict(status='ok',verdict='redact',redacted=ref)
        safe = text
        for value in ('active@example.invalid', 'retired@example.invalid', 'history@example.invalid', 'proposal@example.invalid'):
            if value in safe:
                ref = '[[vault:v1:'+str(uuid.UUID(hashlib.sha256(value.encode()).hexdigest()[:32]))+']]'
                self.values[ref] = value
                safe = safe.replace(value, ref)
        return 200, dict(status='ok', verdict='allow' if text == safe else 'redact', redacted=safe)


@unittest.skipUnless(os.environ.get('AIMEE_CUSTODY_POSTGRES_IMAGE'), 'set AIMEE_CUSTODY_POSTGRES_IMAGE on a disposable Docker host')
class CustodyPostgres(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.container = 'custody-pg-'+secrets.token_hex(6)
        subprocess.run(['docker','run','-d','--name',cls.container,'-e','POSTGRES_PASSWORD='+secrets.token_urlsafe(24),
                        '-e','POSTGRES_USER=postgres','-e','POSTGRES_DB=aimee_store',
                        '-e','AIMEE_STORE_MIGRATOR_PASSWORD='+secrets.token_urlsafe(24),
                        '-e','AIMEE_STORE_RUNTIME_PASSWORD='+secrets.token_urlsafe(24),
                        '-e','AIMEE_POSTGRES_STORAGE=plain',os.environ['AIMEE_CUSTODY_POSTGRES_IMAGE']],check=True,stdout=subprocess.DEVNULL)
        try:
            import time
            for _ in range(120):
                if subprocess.run(['docker','exec',cls.container,'pg_isready','-h','127.0.0.1','-U','postgres','-d','aimee_store'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode == 0:
                    break
                time.sleep(1)
            else:
                raise RuntimeError('disposable PostgreSQL not ready')
            cls.command = f"docker exec -i {cls.container} sh -c 'PGPASSWORD=\"$POSTGRES_PASSWORD\" PGSSLMODE=require exec psql -h 127.0.0.1 -U postgres -d aimee_store \"$@\"' sh"
        except BaseException:
            cls.tearDownClass()
            raise

    @classmethod
    def tearDownClass(cls):
        subprocess.run(['docker','rm','-f',cls.container],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)

    def sql(self, sql):
        return custody.query(self.command, sql)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name)/'snapshot.jsonl'
        self.vault = VaultFixture()
        self.sql('''
DROP SCHEMA public CASCADE; CREATE SCHEMA public;
CREATE TABLE user_memories(id bigint PRIMARY KEY,key text NOT NULL,content text NOT NULL,source_session text DEFAULT '',
 record_revision bigint NOT NULL,lifecycle_state text NOT NULL,confidence numeric DEFAULT 0.1234567890123456789012345);
CREATE TABLE user_memory_versions(memory_id bigint REFERENCES user_memories(id),record_revision bigint,record jsonb NOT NULL CHECK(jsonb_typeof(record)='object'),PRIMARY KEY(memory_id,record_revision));
CREATE TABLE user_memory_correction_proposals(proposal_id text PRIMARY KEY,target_id bigint REFERENCES user_memories(id),payload text NOT NULL,payload_digest text NOT NULL,state text DEFAULT 'pending');
INSERT INTO user_memories(id,key,content,record_revision,lifecycle_state) VALUES
 (9007199254740993,'active','active@example.invalid',7,'active'),(2,'retired','retired@example.invalid',9,'retired');
INSERT INTO user_memory_versions VALUES(9007199254740993,6,'{"id":9007199254740993,"record_revision":6,"confidence":0.1234567890123456789012345,"content":"history@example.invalid","lifecycle_state":"active"}');
INSERT INTO user_memory_correction_proposals(proposal_id,target_id,payload,payload_digest) VALUES
 ('proposal-1',2,' { "content" : "proposal\\u0040example.invalid", "target_revision" : 9007199254740993 } ',encode(sha256(convert_to(' { "content" : "proposal\\u0040example.invalid", "target_revision" : 9007199254740993 } ','UTF8')),'hex'));
CREATE FUNCTION immutable() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'immutable'; END $$;
CREATE TRIGGER history_guard BEFORE UPDATE ON user_memory_versions FOR EACH ROW EXECUTE FUNCTION immutable();
CREATE TRIGGER proposal_guard BEFORE UPDATE ON user_memory_correction_proposals FOR EACH ROW EXECUTE FUNCTION immutable();
CREATE TRIGGER retired_guard BEFORE UPDATE ON user_memories FOR EACH ROW EXECUTE FUNCTION immutable();
ALTER TABLE user_memories ENABLE ALWAYS TRIGGER retired_guard;
CREATE TRIGGER unused_guard BEFORE UPDATE ON user_memories FOR EACH ROW EXECUTE FUNCTION immutable();
ALTER TABLE user_memories DISABLE TRIGGER unused_guard;
''')
        self.records = [dict(id=9007199254740993,record_revision=7,key='active',content='active@example.invalid')]
        self.path.write_text(json.dumps(self.records[0])+'\n')

    def tearDown(self):
        self.temp.cleanup()

    def modes(self):
        return self.sql("SELECT json_build_object('name',tgname,'mode',tgenabled) FROM pg_trigger WHERE NOT tgisinternal ORDER BY tgname;")

    def state(self):
        return self.sql("SELECT json_build_object('table','current','rows',json_agg(t ORDER BY id)) FROM user_memories t UNION ALL SELECT json_build_object('table','versions','rows',json_agg(t)) FROM user_memory_versions t UNION ALL SELECT json_build_object('table','proposals','rows',json_agg(t)) FROM user_memory_correction_proposals t;")

    def test_protects_retired_versions_proposals_and_snapshot_without_changing_identity(self):
        before, modes = self.state(), self.modes()
        original_query = custody.query
        submitted = []
        def capture(command, sql):
            if sql.startswith('BEGIN;'):
                submitted.append(sql)
            return original_query(command, sql)
        with mock.patch.object(custody, 'query', side_effect=capture):
            records, changed = custody.protect_source(self.command,'server',self.vault,self.records,self.path)
        self.assertGreaterEqual(changed,4)
        after = self.state()
        witnesses = self.sql("SELECT json_build_object('ref',original_vault_ref,'sha256',original_sha256) FROM aimee_legacy_memory_custody;")
        self.assertEqual(len(witnesses),changed)
        for witness in witnesses:
            self.assertEqual(hashlib.sha256(self.vault.values[witness['ref']].encode()).hexdigest(),witness['sha256'])
        safe = json.dumps(after)
        for ref, value in self.vault.values.items():
            self.assertNotIn(value,safe)
            self.assertNotIn(value,self.path.read_text())
            self.assertTrue(all(value not in sql for sql in submitted))
            safe = safe.replace(ref,value)
        self.assertTrue(any('\\u0040' in value for value in self.vault.values.values()))
        recovered = json.loads(safe)
        # Protected JSON keeps semantic fields; exact original spelling and
        # its signed digest are recovered independently through the witness.
        for state in (before, recovered):
            for entry in state:
                if entry['table'] == 'proposals':
                    for proposal in entry['rows']:
                        proposal['payload'] = json.loads(proposal['payload'])
        self.assertEqual(recovered,before)
        self.assertTrue(self.sql("SELECT json_build_object('matches',w.original_sha256=p.payload_digest) FROM aimee_legacy_memory_custody w JOIN user_memory_correction_proposals p ON w.source_key->>'proposal_id'=p.proposal_id WHERE w.source_table='user_memory_correction_proposals' AND w.source_column='payload';")[0]['matches'])
        self.assertEqual(self.sql("SELECT json_build_object('decimal',record->>'confidence') FROM user_memory_versions;")[0]['decimal'],'0.1234567890123456789012345')
        self.assertFalse(self.sql("SELECT json_build_object('allowed',has_table_privilege('aimee_store_runtime','aimee_legacy_memory_custody','INSERT'));")[0]['allowed'])
        self.assertEqual([m for m in self.modes() if m['name'] != 'aimee_legacy_memory_custody_immutable'],modes)
        with self.assertRaises(ValueError):
            self.sql("UPDATE aimee_legacy_memory_custody SET protected_sha256=repeat('0',64);")
        self.assertEqual(self.path.stat().st_mode & 0o777,0o600)
        again, changes = custody.protect_source(self.command,'server',self.vault,records,self.path)
        self.assertEqual(changes,0)
        self.assertEqual(again,records)
        self.assertEqual(self.sql("SELECT json_build_object('count',count(*)) FROM aimee_legacy_memory_custody;")[0]['count'],len(witnesses))
        with self.assertRaises(ValueError):
            self.sql("UPDATE user_memory_versions SET record='{}';")

    def test_shared_retained_rejections_and_proposals_keep_private_domain_untouched(self):
        private = self.state()
        self.sql("""
CREATE TABLE memories(id bigint PRIMARY KEY,key text,content text,use_cases text,source_session text,archive_reason text,record_revision bigint,lifecycle_state text);
CREATE TABLE memory_rejection_tombstones(id bigint PRIMARY KEY,memory_key text,memory_content text,reason text,active integer);
CREATE TABLE memory_correction_proposals(proposal_id text PRIMARY KEY,payload text,payload_digest text,state text);
CREATE TABLE fact_graph_changes(id bigint PRIMARY KEY,diff_detail text,before_version bigint,after_version bigint,before_state text DEFAULT '',after_state text DEFAULT '');
CREATE TABLE knowledge_corrections(correction_id text PRIMARY KEY,subject_kind text,before_value text,after_value text);
CREATE TABLE knowledge_review_decisions(decision_id text PRIMARY KEY,source_queue text,requested_value text,evidence_snapshot text);
CREATE TABLE memory_evidence_events(event_id text PRIMARY KEY,object_kind text,reason text);
INSERT INTO memories VALUES(44,'shared','active@example.invalid','','','',5,'active'),(45,'retired','retired@example.invalid','','','reason',6,'retired');
INSERT INTO memory_rejection_tombstones VALUES(1,'retired','retired@example.invalid','reason',1);
INSERT INTO memory_correction_proposals VALUES('kb-draft','{"content":"proposal@example.invalid"}','original-digest','rejected');
INSERT INTO fact_graph_changes(id,diff_detail,before_version,after_version) VALUES(7,'{"content":"history@example.invalid","record_revision":9007199254740993}',9007199254740993,9007199254740994);
INSERT INTO knowledge_corrections VALUES('c1','memory','history@example.invalid','active@example.invalid'),('unrelated','document','untouched unrelated document','untouched');
INSERT INTO knowledge_review_decisions VALUES('review1','conflict','proposal@example.invalid','{"content":"history@example.invalid","revision":9007199254740993}');
INSERT INTO memory_evidence_events VALUES('event1','memory','retired@example.invalid');
""")
        record = dict(id=44,key='shared',content='active@example.invalid',record_revision=5)
        self.path.write_text(json.dumps(record)+'\n')
        def shared():
            return self.sql("SELECT json_build_object('table','memories','rows',json_agg(t ORDER BY id)) FROM memories t UNION ALL SELECT json_build_object('table','rejections','rows',json_agg(t)) FROM memory_rejection_tombstones t UNION ALL SELECT json_build_object('table','proposals','rows',json_agg(t)) FROM memory_correction_proposals t UNION ALL SELECT json_build_object('table','changes','rows',json_agg(t)) FROM fact_graph_changes t;")
        before = shared()
        records, changed = custody.protect_source(self.command,'kb',self.vault,[record],self.path)
        self.assertGreaterEqual(changed,5)
        raw = json.dumps(shared())
        for ref,value in self.vault.values.items():
            self.assertNotIn(value,raw)
            raw = raw.replace(ref,value)
        self.assertEqual(json.loads(raw),before)
        self.assertEqual(self.state(),private)
        for table in ('knowledge_corrections','knowledge_review_decisions','memory_evidence_events'):
            exported = json.dumps(self.sql('SELECT json_build_object(\'rows\',json_agg(t)) FROM '+table+' t;'))
            for value in self.vault.values.values():
                self.assertNotIn(value,exported)
        self.assertIn('untouched unrelated document',json.dumps(self.sql("SELECT json_build_object('value',before_value) FROM knowledge_corrections WHERE correction_id='unrelated';")))
        self.assertEqual(custody.protect_source(self.command,'kb',self.vault,records,self.path)[1],0)

    def test_import_rechecks_eligibility_and_authenticated_source_metadata(self):
        query = "SELECT json_build_object('id',id,'record_revision',record_revision,'key',key,'content',content,'valid_until','2999-01-01T00:00:00Z') FROM user_memories WHERE lifecycle_state='active';"
        before = self.state()
        for changed in [dict(self.records[0],content='forged'),dict(self.records[0],id=2),dict(self.records[0],record_revision=6)]:
            with self.assertRaises(ValueError):
                custody.protect_source(self.command,'server',self.vault,[changed],self.path,export_sql=query)
        self.assertEqual(self.state(),before)
        safe, _ = custody.protect_source(self.command,'server',self.vault,self.records,self.path,export_sql=query)
        self.assertEqual(safe[0]['valid_until'],'2999-01-01T00:00:00Z')

    def test_failed_update_rolls_back_all_payloads_and_trigger_modes(self):
        before,modes = self.state(),self.modes()
        self.sql("ALTER TABLE user_memories ADD CONSTRAINT fixture_failure CHECK(content NOT LIKE '%[[vault%');")
        with self.assertRaises(ValueError):
            custody.protect_source(self.command,'server',self.vault,self.records,self.path)
        self.assertEqual(self.state(),before)
        self.assertEqual(self.modes(),modes)
        self.assertIn('active@example.invalid',self.path.read_text())

    def test_wrong_source_or_revision_never_changes_retained_data(self):
        before,modes = self.state(),self.modes()
        for record in [dict(self.records[0],id=3),dict(self.records[0],record_revision=6)]:
            with self.assertRaises(ValueError):
                custody.protect_source(self.command,'server',self.vault,[record],self.path)
        self.assertEqual(self.state(),before)
        self.assertEqual(self.modes(),modes)

    def test_source_race_is_detected_under_transaction_lock(self):
        before,modes = self.state(),self.modes()
        original_query = custody.query
        def changed(command, sql):
            if sql.startswith('BEGIN;'):
                original_query(command,"INSERT INTO user_memories(id,key,content,record_revision,lifecycle_state) VALUES(3,'new','retired@example.invalid',1,'retired');")
            return original_query(command,sql)
        with mock.patch.object(custody,'query',side_effect=changed):
            with self.assertRaises(ValueError):
                custody.protect_source(self.command,'server',self.vault,self.records,self.path)
        self.assertEqual(self.modes(),modes)
        self.assertIn('active@example.invalid',self.path.read_text())
        self.assertEqual(self.state()[0]['rows'][:2],sorted(before[0]['rows']+[dict(before[0]['rows'][0],id=3,key='new',content='retired@example.invalid',record_revision=1,lifecycle_state='retired')],key=lambda r:r['id'])[:2])


if __name__ == '__main__':
    unittest.main()
