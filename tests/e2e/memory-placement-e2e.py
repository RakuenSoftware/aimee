#!/usr/bin/env python3
"""Memory release gate against an explicitly supplied disposable Docker stack.

Creates fixtures, restarts services, and stops each dependency temporarily.
Run after the Docker smoke bootstrap; never point this at an installation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import uuid

# The host bounds module dispatch at 60 seconds; observe its failure envelope.
HTTP = '''import http.client,socket,json,sys
class C(http.client.HTTPConnection):
 def connect(self):
  self.sock=socket.socket(socket.AF_UNIX);self.sock.settimeout(70);self.sock.connect('/var/lib/aimee/aimee-http.sock')
a=json.load(sys.stdin);c=C('localhost',timeout=70)
c.request(a['method'],a['path'],json.dumps(a['body']),{'Content-Type':'application/json'})
r=c.getresponse();print(json.dumps([r.status,json.loads(r.read())]))
'''

class Gate:
    def __init__(self, args):
        self.args = args
        self.prefix = 'placement-e2e-' + uuid.uuid4().hex[:10]
        self.checks = []

    def docker(self, *args, input=None):
        return subprocess.check_output(['docker', *args], input=input, text=True, stderr=subprocess.PIPE, timeout=90).strip()

    def call(self, op, body=None, method='POST'):
        path = op if op.startswith('/') else '/v1/memory/' + op
        return json.loads(self.docker('exec', '-i', self.args.server, 'python3', '-c', HTTP,
            input=json.dumps(dict(method=method, path=path, body=body or {}))))

    def cli(self, *args):
        return json.loads(self.docker('exec', '-u', '1000', '-e',
            'AIMEE_API_ENDPOINT=unix:/var/lib/aimee/aimee-http.sock',
            self.args.server, 'aimee', '--json', 'memory', *map(str, args)))

    def sql(self, query):
        if getattr(self.args, 'kb_store_db', None):
            return self.docker('exec', self.args.kb_store_db, 'psql', '-U', 'postgres',
                '-d', 'aimee_store', '-X', '-At', '-v', 'ON_ERROR_STOP=1', '-c', query)
        return self.docker('exec', self.args.kb, 'psql', '-h', '/var/lib/aimee/run',
                           '-d', 'aimee_shared', '-X', '-At', '-v', 'ON_ERROR_STOP=1', '-c', query)

    def digest(self):
        rows = self.sql("SELECT id,key,content FROM memories ORDER BY id")
        return hashlib.sha256(rows.encode()).hexdigest()

    def personal_sql(self, query):
        return self.docker('exec', self.args.store_db, 'psql', '-U', 'postgres',
                           '-d', 'aimee_store', '-X', '-At', '-v', 'ON_ERROR_STOP=1', '-c', query)

    def personal_changes(self, mid):
        return json.loads(self.personal_sql(f"""SELECT json_build_object(
            'owner_id',g.owner_id,'generation',g.generation,'revision',m.record_revision,
            'events',(SELECT json_agg(json_build_object('generation',e.generation,
               'revision',e.record_revision,'operation',e.operation) ORDER BY e.generation)
               FROM user_memory_invalidation_outbox e WHERE e.memory_id=m.id))
            FROM user_memory_collection_generation g,user_memories m WHERE g.id=1 AND m.id={int(mid)}"""))

    def shared_changes(self, mid):
        return json.loads(self.sql(f"""SELECT json_build_object(
            'owner_id',o.owner_id,'revision',m.record_revision,
            'events',(SELECT json_agg(json_build_object('generation',e.generation,
              'scope_type',e.scope_type,'scope_value',e.scope_value,
              'revision',e.record_revision,'operation',e.operation)
              ORDER BY e.scope_type,e.scope_value,e.generation)
              FROM memory_invalidation_outbox e WHERE e.memory_id=m.id))
            FROM memory_collection_owner o,memories m WHERE o.id=1 AND m.id={int(mid)}"""))

    def mcp_document(self, name, tool, arguments):
        code, raw = self.mcp(tool, arguments)
        documents = []
        def visit(value):
            if isinstance(value, dict):
                if value.get('type') == 'text' and isinstance(value.get('text'), str):
                    try:
                        documents.append(json.loads(value['text']))
                    except ValueError:
                        pass
                else:
                    for child in value.values():
                        visit(child)
            elif isinstance(value, list):
                for child in value:
                    visit(child)
        visit(json.loads(raw))
        self.check(name + ' owner envelope', code == 200 and len(documents) == 1 and isinstance(documents[0], dict))
        return documents[0] if len(documents) == 1 and isinstance(documents[0], dict) else {}

    def shared_mcp_corrections(self):
        key = self.prefix + '-mcp-correction'
        project = self.prefix + '-mcp-project'
        code, stored = self.mcp('mutate', dict(verb='store', store='kb', project=project,
            key=key, content='original model fixture', confidence=0.43))
        self.check('MCP creates model-authored shared correction fixture', code == 200 and 'stored memory id=' in stored)
        old_id = int(self.sql(f"SELECT id FROM memories WHERE key='{key}' AND lifecycle_state='active'"))
        observed = self.mcp_document('MCP versioned get', 'memory_get',
            dict(store='kb', project=project, id=str(old_id), include_version=True))
        version = observed['memory']['version']
        self.check('MCP versioned get preserves exact target', version['record_id'] == str(old_id))
        private = self.mcp_document('MCP private correction precondition', 'mutate',
            dict(verb='update', store='user', id=str(old_id), content='unsupported private correction',
                 expected_version=version, idempotency_key=self.prefix + '-private-retry'))
        self.check('MCP private placement refuses unsupported correction preconditions', private.get('kind') == 'unsupported_mode')
        correction = dict(verb='update', store='kb', project=project, id=str(old_id),
            content='corrected model fixture', authority='user', expected_version=version,
            idempotency_key=self.prefix + '-mcp-retry')
        updated = self.mcp_document('MCP keyed update', 'mutate', correction)
        receipt = updated['mutation_receipt']
        new_id = int(receipt['version']['record_id'])
        self.check('MCP update returns canonical commit receipt', updated.get('status') == 'ok' and
            receipt['replayed'] is False and new_id != old_id and updated.get('audit_id') == str(new_id))
        self.check('MCP update preserves confidence and model authority', self.sql(
            f"SELECT (confidence=0.43 AND provenance_category='agent_message')::text FROM memories WHERE id={new_id}") == 'true')
        replay = self.mcp_document('MCP keyed update retry', 'mutate', correction)
        self.check('MCP replay preserves commit without requesting another host audit',
            replay.get('mutation_receipt', {}).get('commit_id') == receipt['commit_id'] and
            replay.get('mutation_receipt', {}).get('replayed') is True and 'audit_id' not in replay)
        conflict = self.mcp_document('MCP changed retry payload', 'mutate', dict(correction, content='different'))
        self.check('MCP refuses changed idempotency payload', conflict.get('reason') == 'idempotency_conflict')
        unkeyed = dict(correction)
        del unkeyed['idempotency_key']
        stale = self.mcp_document('MCP stale update version', 'mutate', unkeyed)
        self.check('MCP forwards and enforces expected version', stale.get('reason') == 'expected_version_conflict')
        self.sql(f"UPDATE memories SET lifecycle_state='retired' WHERE id={new_id}")
        hidden = self.mcp_document('MCP retired update retry', 'mutate', correction)
        self.check('MCP retry never releases retired content', hidden.get('reason') == 'idempotent_result_unavailable' and
            'mutation_receipt' not in hidden and 'memory' not in hidden)
        self.check('MCP retries retain exactly one replacement', self.sql(
            f"SELECT count(*) FROM memories WHERE key='{key}' OR key LIKE '{key}#v%'") == '2')

    def shared_correction_admission(self, old_id, version):
        before = self.shared_changes(old_id)
        keys = []
        self.sql("ALTER TABLE fact_graph_commits ADD CONSTRAINT e2e_admission_audit_failure CHECK(operation NOT IN ('memory.supersede','memory.update')) NOT VALID")
        try:
            for verb in ('update', 'supersede'):
                key = self.prefix + '-admission-' + verb
                keys.append(hashlib.sha256(key.encode()).hexdigest())
                args = dict(store='kb', id=str(old_id), old_id=str(old_id),
                    content='refused model edit', new_content='refused model edit',
                    authority='user', expected_version=version, idempotency_key=key)
                refused = self.mcp_document('MCP ' + verb + ' admission before audit',
                    'mutate', dict(args, verb=verb))
                self.check('MCP ' + verb + ' requires review before canonical audit',
                    refused.get('kind') == 'review_required' and 'mutation_receipt' not in refused)
                code, failure = self.call(verb, args)
                self.check('HTTP ' + verb + ' admitted correction reaches blocked audit',
                    code >= 500 and failure.get('kind') == 'unavailable')
                args['expected_version'] = dict(version, record_revision='9223372036854775807')
                code, stale = self.call(verb, args)
                self.check('HTTP ' + verb + ' rejects stale version before canonical audit',
                    code == 409 and stale.get('reason') == 'expected_version_conflict')
        finally:
            self.sql('ALTER TABLE fact_graph_commits DROP CONSTRAINT e2e_admission_audit_failure')
        self.check('refused corrections preserve original revision and invalidations',
            self.shared_changes(old_id) == before)
        hashes = ','.join("'" + key + "'" for key in keys)
        self.check('refused corrections do not reserve retry keys', self.sql(
            f"SELECT count(*) FROM memory_mutation_receipts WHERE key_hash IN ({hashes})") == '0')

    def shared_journal(self):
        key = self.prefix + '-journal'
        original = 'shared journal original'
        stored = self.good('shared journal HTTP store', self.call('store', dict(store='kb', key=key, content=original)))
        old_id = int(stored['id'])
        observed = self.good('shared versioned HTTP get', self.call('get', dict(store='kb', id=old_id, include_version=True)))
        version = observed['memory']['version']
        self.check('shared version binds exact identity and owner', version['schema_version'] == 1 and
                   version['record_id'] == str(old_id) and isinstance(version['record_revision'], str))
        before = self.shared_changes(old_id)
        self.check('shared version binds current revision', version['owner_id'] == before['owner_id'] and
                   version['record_revision'] == str(before['revision']))
        self.check('shared HTTP store commits invalidation', bool(before['events']) and
                   before['events'][0]['operation'] == 'insert' and before['events'][-1]['revision'] == before['revision'])
        retry = self.good('shared journal identical retry', self.call('store', dict(store='kb', key=key, content=original)))
        self.check('shared retry preserves identity and journal', retry['id'] == old_id and self.shared_changes(old_id) == before)
        self.shared_correction_admission(old_id, version)
        self.sql(f"INSERT INTO memory_scopes(memory_id,scope_type,scope_value) VALUES ({old_id},'workspace','journal-one'),({old_id},'workspace','journal-two')")
        tagged = self.shared_changes(old_id)
        self.check('shared tag batch invalidates its parent once', tagged['revision'] == before['revision'] + 1 and
                   len(tagged['events']) == len(before['events']) + 1)
        code, stale = self.call('supersede', dict(store='kb', old_id=old_id, new_content='stale replacement', expected_version=version))
        self.check('shared HTTP rejects stale expected version', code == 409 and stale.get('reason') == 'expected_version_conflict')
        observed = self.good('shared refreshed versioned HTTP get', self.call('get', dict(store='kb', id=old_id, include_version=True)))
        version = observed['memory']['version']
        retry_key = self.prefix + '-journal-correction'
        self.sql(f"ALTER TABLE memory_scopes ADD CONSTRAINT e2e_scope_copy_failure CHECK(memory_id={old_id} OR scope_value<>'journal-one') NOT VALID")
        try:
            code, failure = self.call('supersede', dict(store='kb', old_id=old_id, new_content='must roll back', expected_version=version, idempotency_key=retry_key))
            self.check('shared HTTP replacement refuses failed tag copy', code >= 500 and failure.get('kind') == 'unavailable')
            retained = self.good('shared original after failed copy', self.call('get', dict(store='kb', id=old_id)))
            self.check('failed tag copy preserves original and invalidation',
                       retained.get('memory', {}).get('content') == original and self.shared_changes(old_id) == tagged)
        finally:
            self.sql('ALTER TABLE memory_scopes DROP CONSTRAINT e2e_scope_copy_failure')
        correction = dict(store='kb', old_id=old_id, new_content='shared journal corrected', expected_version=version, idempotency_key=retry_key)
        updated = self.good('shared HTTP version replacement', self.call('supersede', correction))
        receipt = updated['mutation_receipt']
        self.check('shared correction returns canonical commit receipt', receipt['schema_version'] == 1 and
                   receipt['replayed'] is False and bool(receipt['commit_id']))
        replay = self.good('shared HTTP idempotent correction replay', self.call('supersede', correction))
        self.check('shared correction replay preserves identity and commit', replay.get('id') == updated['id'] and
                   replay.get('mutation_receipt', {}).get('replayed') is True and
                   replay.get('mutation_receipt', {}).get('commit_id') == receipt['commit_id'])
        code, conflict = self.call('supersede', dict(correction, new_content='different payload'))
        self.check('shared HTTP refuses idempotency key reuse', code == 409 and conflict.get('reason') == 'idempotency_conflict')
        self.retry_correction = (correction, receipt)

        new_id = int(updated['id'])
        code, retry = self.call('supersede', dict(store='kb', old_id=old_id, new_content='shared journal corrected', expected_version=version))
        self.check('shared HTTP correction retry conflicts without duplicating', code == 409 and retry.get('reason') == 'expected_version_conflict')
        self.check('shared replacement creates a new identity', new_id != old_id)
        self.check('shared HTTP correction retains verified user authorship', self.sql(f"SELECT (m.provenance_category='user_stated' AND a.actor_role='user' AND a.authenticated=1)::text FROM memories m JOIN memory_fact_actors a ON a.memory_id=m.id WHERE m.id={new_id}") == 'true')
        copies = int(self.sql(f"SELECT count(*) FROM memory_scopes WHERE memory_id={new_id} AND scope_value IN ('journal-one','journal-two')"))
        self.check('shared replacement preserves visible tags', copies == 2)
        after = self.shared_changes(new_id)
        self.check('shared replacement and copied tags have durable revisions',
                   after['revision'] >= 2 and after['events'][0]['operation'] == 'insert' and
                   after['events'][-1]['revision'] == after['revision'])
        self.good('shared replacement get', self.call('get', dict(store='kb', id=new_id)))
        self.check('shared direct read leaves invalidations unchanged', self.shared_changes(new_id) == after)
        return new_id, after

    def assert_no_personal_canary(self):
        tables = self.sql("SELECT tablename FROM pg_tables WHERE schemaname='public' ORDER BY tablename").splitlines()
        queries = []
        for table in tables:
            ident = '"' + table.replace('"', '""') + '"'
            literal = "'" + table.replace("'", "''") + "'"
            queries.append(f"SELECT {literal} WHERE EXISTS(SELECT 1 FROM public.{ident} t "
                "WHERE row_to_json(t)::text LIKE '%user@local.invalid%' "
                "OR row_to_json(t)::text LIKE '%person@local.invalid%')")
        found = self.sql(' UNION ALL '.join(queries))
        self.check('personal email canaries absent from all KB tables', not found,
                   dict(table_count=len(tables), matching_tables=found.splitlines()))

    def check(self, name, passed, detail=None):
        self.checks.append(dict(name=name, passed=bool(passed), detail=detail))
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)

    def good(self, name, response):
        code, body = response
        passed = code == 200 and body.get('status') == 'ok'
        self.check(name, passed, None if passed else response)
        return body

    def wait(self, op, body=None, *, predicate=None, timeout=150):
        """Wait for a successful response and, optionally, the required result."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                response = self.call(op, body)
                if (response[0] == 200 and response[1].get('status') == 'ok'
                        and (predicate is None or predicate(response[1]))):
                    return response
            except (subprocess.SubprocessError, ValueError):
                pass
            time.sleep(1)
        raise RuntimeError('service did not recover: ' + op)

    def mcp(self, tool, arguments):
        code, body = self.call('/v1/mcp/call', dict(tool=tool, arguments=arguments))
        # The transport wraps the MCP content blocks in its result envelope.
        return code, json.dumps(body, ensure_ascii=False)

    def confidence_contract(self, stores):
        """Malformed confidence never becomes certainty or a storage outage."""
        for store in stores:
            for confidence in (-1, 1.01, False, None, 'invalid', [], {}):
                payload = dict(store=store, key=self.prefix+'-confidence',
                               content='Synthetic confidence fixture', confidence=confidence)
                code, body = self.call('store', payload)
                self.check(f'{store} HTTP rejects confidence {confidence!r}',
                           code == 400 and body.get('kind') == 'invalid_argument', [code, body])
                for verb in ('store', 'update', 'supersede'):
                    code, body = self.mcp('mutate', dict(payload, verb=verb, id=1))
                    self.check(f'{store} MCP {verb} rejects confidence {confidence!r}',
                               'confidence must be between 0 and 1' in body, [code, body])
            for confidence in (0, 0.25, 1):
                row = self.good(f'{store} stores confidence {confidence}', self.call('store',
                    dict(store=store, key=self.prefix+'-confidence-'+str(confidence),
                         content='Synthetic confidence boundary fixture', confidence=confidence)))
                if 'id' not in row:
                    continue
                code, body = self.call('get', dict(store=store, id=row['id']))
                self.check(f'{store} preserves confidence {confidence}',
                           code == 200 and body.get('memory', {}).get('confidence') == confidence,
                           [code, body])

    def recall_with_database_contention(self, content, mid):
        """Brief database contention must fit the data stage's bounded budget."""
        lock = subprocess.Popen(['docker', 'exec', self.args.store_db,
            'psql', '-U', 'postgres', '-d', 'aimee_store', '-X', '-At',
            '-v', 'ON_ERROR_STOP=1', '-c',
            'BEGIN; LOCK TABLE user_memories IN ACCESS EXCLUSIVE MODE; '
            'SELECT pg_sleep(1.5); COMMIT;'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                held = self.docker('exec', self.args.store_db, 'psql', '-U', 'postgres',
                    '-d', 'aimee_store', '-X', '-At', '-c',
                    "SELECT count(*) FROM pg_locks WHERE relation='user_memories'::regclass "
                    "AND mode='AccessExclusiveLock' AND granted")
                if held == '1':
                    break
                if lock.poll() is not None:
                    raise RuntimeError('database contention fixture ended before recall')
                time.sleep(0.01)
            else:
                raise RuntimeError('database contention fixture never acquired its lock')
            bundle = self.good('personal recall tolerates brief database contention',
                self.call('recall', dict(query=self.prefix, store='user')))
            self.check('delayed recall preserves personal text and scoped handle', any(
                r.get('text') == content and r.get('handle') == 'user:memory:' + str(mid)
                for r in bundle.get('recall', {}).get('active_context', [])))
        finally:
            lock.communicate(timeout=10)
        if lock.returncode:
            raise RuntimeError('database contention fixture failed')

    def run_local(self):
        """First-boot regression on a composition containing only Server and its store."""
        content = 'Personal local-only fixture person@local.invalid 🦊'
        row = self.good('KB-free local store', self.call('store', dict(key=self.prefix, content=content)))
        mid = row['id']
        stored_changes = self.personal_changes(mid)
        self.check('HTTP store commits personal invalidation', stored_changes.get('revision') == 1 and
                   stored_changes.get('events', [])[-1:] == [dict(
                       generation=stored_changes['generation'], revision=1, operation='insert')])
        retry = self.good('personal store retry', self.call('store', dict(key=self.prefix, content=content)))
        self.check('identical HTTP upsert preserves identity and invalidation progress',
                   retry['id'] == mid and self.personal_changes(mid) == stored_changes)
        for explicit in ({}, {'store': 'user'}):
            bundle = self.good('KB-free recall ' + str(explicit),
                self.call('recall', dict(query=self.prefix, **explicit)))
            self.check('recall returns personal text and scoped handle ' + str(explicit), any(
                r.get('text') == content and r.get('handle') == 'user:memory:' + str(mid)
                for r in bundle.get('recall', {}).get('active_context', [])))
        self.recall_with_database_contention(content, mid)
        self.good('KB-free session recall without hint', self.call('recall', dict(session_start=True)))
        cli = self.cli('recall', '--query', self.prefix, '--store', 'user')
        self.check('KB-free CLI recall', cli.get('store') == 'user' and any(
            r.get('text') == content and r.get('memory_id') == mid
            for r in cli.get('recall', {}).get('active_context', [])), cli)
        code, mcp = self.mcp('memory_recall', dict(task_hint=self.prefix))
        self.check('KB-free MCP recall', code == 200 and 'Personal local-only fixture' in mcp)
        code, invalid = self.call('recall', dict(query=self.prefix, store='invalid'))
        self.check('recall rejects invalid store', code == 400, [code, invalid])
        read_changes = self.personal_changes(mid)
        self.check('HTTP CLI MCP recall leaves personal invalidations unchanged',
                   read_changes['revision'] == stored_changes['revision'] and
                   read_changes['events'] == stored_changes['events'])
        self.personal_sql(f'ALTER TABLE user_memory_invalidation_outbox ADD CONSTRAINT e2e_outbox_failure CHECK(memory_id<>{int(mid)}) NOT VALID')
        try:
            code, failure = self.call('supersede', dict(old_id=mid, new_content='must roll back'))
            self.check('HTTP mutation refuses failed invalidation commit', code >= 500 and failure.get('status') == 'error')
            got = self.good('personal content after failed invalidation', self.call('get', dict(id=mid)))
            self.check('failed invalidation preserves canonical content and progress',
                       got.get('memory', {}).get('content') == content and
                       self.personal_changes(mid) == read_changes)
        finally:
            self.personal_sql('ALTER TABLE user_memory_invalidation_outbox DROP CONSTRAINT e2e_outbox_failure')
        self.docker('restart', self.args.server)
        self.good('KB-free recall survives restart', self.wait('recall', dict(query=self.prefix)))
        self.check('personal invalidations survive application restart', self.personal_changes(mid) == read_changes)
        self.docker('stop', self.args.store_db)
        try:
            code, failure = self.call('recall', dict(query=self.prefix))
            self.check('KB-free recall reports unavailable local store', code >= 500 and failure.get('status') == 'error', [code, failure])
        finally:
            self.docker('start', self.args.store_db)
        self.good('KB-free recall recovers', self.wait('recall', dict(query=self.prefix)))
        self.check('personal invalidations survive storage restart', self.personal_changes(mid) == read_changes)
        self.good('KB-free local retirement', self.call('delete', dict(id=mid)))
        retired_changes = self.personal_changes(mid)
        self.check('HTTP retirement commits a new personal invalidation',
                   retired_changes['owner_id'] == stored_changes['owner_id'] and
                   retired_changes['revision'] == 2 and retired_changes['events'][-1]['revision'] == 2 and
                   len(retired_changes['events']) == len(stored_changes['events']) + 1)
        bundle = self.good('KB-free recall after retirement', self.call('recall', dict(query=self.prefix)))
        self.check('retired personal record excluded from recall', all(
            r.get('memory_id') != mid and r.get('handle') != 'user:memory:' + str(mid)
            for r in bundle.get('recall', {}).get('active_context', [])))
        self.confidence_contract(('user',))
        return all(c['passed'] for c in self.checks)

    def run(self):
        before = self.digest()
        content = 'Personal fixture user@local.invalid 🦊 ' + 'long note αβ ' * 500
        written = self.good('local store', self.call('store', dict(key=self.prefix, content=content)))
        mid = written['id']
        self.check('local store leaves KB unchanged', self.digest() == before)
        # Independent sequences deliberately overlap. Preserve any existing KB
        # row at this ID, or seed a benign shared record to create the collision.
        self.sql(f"INSERT INTO memories(id,key,content) VALUES({mid},'{self.prefix}-kb','shared collision fixture') ON CONFLICT(id) DO NOTHING")
        self.sql("SELECT setval(pg_get_serial_sequence('memories','id'),GREATEST(1,(SELECT max(id) FROM memories)),true)")
        shared = json.loads(self.sql(f"SELECT to_json(content) FROM memories WHERE id={mid}"))
        before = self.digest()
        got = self.good('local get', self.call('get', dict(id=mid)))
        self.check('get returns exact local content for colliding ID', got.get('memory', {}).get('content') == content)
        got = self.good('explicit KB get', self.call('get', dict(id=mid, store='kb', scope='all')))
        self.check('explicit KB get selects the other record', got.get('memory', {}).get('content') == shared)
        mcp_key = self.prefix + '-mcp'
        mcp_content = 'Personal MCP fixture person@local.invalid'
        code, reply = self.call('/v1/mcp/call', dict(tool='mutate', arguments=dict(verb='store', key=mcp_key, content=mcp_content)))
        result = json.loads(reply['content'][0]['text'])
        self.check('MCP store defaults to user', code == 200 and result.get('status') == 'ok' and result.get('store') == 'user', None if result.get('status') == 'ok' else reply)
        mcp_id = result['id']
        self.check('MCP personal content stays local', self.cli('get', mcp_id).get('memory', {}).get('content') == mcp_content and self.digest() == before)
        self.call('/v1/mcp/call', dict(tool='mutate', arguments=dict(verb='update', id=mcp_id, content='updated MCP fixture')))
        self.check('MCP update uses local ID', self.cli('get', mcp_id).get('memory', {}).get('content') == 'updated MCP fixture')
        self.call('/v1/mcp/call', dict(tool='mutate', arguments=dict(verb='forget', id=mcp_id)))
        code, reply = self.call('get', dict(id=mcp_id))
        self.check('MCP forget retires local ID', code >= 400 and reply.get('kind') == 'not_found' and self.digest() == before)
        self.check('CLI default get selects user', self.cli('get', mid).get('memory', {}).get('content') == content)
        self.check('CLI explicit get selects KB', self.cli('get', mid, '--store', 'kb', '--scope', 'all').get('memory', {}).get('content') == shared)
        cli_row = self.cli('store', self.prefix + '-cli', 'local CLI fixture')
        self.check('CLI store identifies local scope', cli_row.get('store') == 'user')
        self.cli('supersede', cli_row['id'], 'updated CLI fixture')
        self.check('CLI supersede updates local record', self.cli('get', cli_row['id']).get('memory', {}).get('content') == 'updated CLI fixture')
        self.cli('delete', cli_row['id'])
        self.check('CLI mutations leave KB unchanged', self.digest() == before)
        self.good('local stats', self.call('stats', method='GET'))
        for invalid in ('both', '', 1):
            code, body = self.call('store', dict(store=invalid, key=self.prefix+'-invalid', content='private invalid selector'))
            self.check('invalid store rejected '+repr(invalid), code >= 400 and body.get('status') == 'error')
        self.check('invalid selectors leave KB unchanged', self.digest() == before)
        for op, body, field in [('list', {}, 'memories'), ('review', {}, 'memories'),
                                ('search', {'keywords': [self.prefix]}, 'facts')]:
            result = self.good('local ' + op, self.call(op, body))
            self.check(op + ' returns local fixture', any(r.get('id') == mid and r.get('content') == content
                                                         for r in result.get(field, [])))
        for args, expected in [(dict(id=mid), content), (dict(handle='memory:' + str(mid), scope='all'), shared)]:
            code, result = self.mcp('memory_get', args)
            # Content is nested as a JSON string within the MCP text block.
            self.check('MCP get preserves selected store ' + str(args), code == 200 and json.dumps(expected, ensure_ascii=False)[1:-1] in result)
        self.good('local preference fixture', self.call('store', dict(key='preference-' + self.prefix,
            kind='preference', tier='L2', content='Private preference person@local.invalid')))
        shared_recall = self.good('explicit shared recall', self.call('recall', dict(store='kb', scope='all', query=self.prefix)))
        self.check('explicit shared recall excludes personal preferences', 'person@local.invalid' not in json.dumps(shared_recall))
        self.good('local supersede', self.call('supersede', dict(old_id=mid, new_content='corrected local fixture')))
        self.check('local mutation leaves colliding KB record unchanged', self.digest() == before)
        got = self.good('get corrected local memory', self.call('get', dict(id=mid)))
        self.check('local replacement content', got.get('memory', {}).get('content') == 'corrected local fixture')
        self.docker('restart', self.args.server)
        got = self.good('local memory survives restart', self.wait('get', dict(id=mid)))
        self.check('restart preserves corrected value', got.get('memory', {}).get('content') == 'corrected local fixture')
        before = self.digest()
        self.docker('stop', self.args.store_db)
        try:
            for op, body in [('list', {}), ('get', {'id': mid}), ('recall', {'query': self.prefix}),
                             ('store', {'key': self.prefix + '-outage', 'content': 'private outage fixture'}),
                             ('supersede', {'old_id': mid, 'new_content': 'must not reach KB'}), ('delete', {'id': mid})]:
                code, body = self.call(op, body)
                self.check('local dependency outage fails ' + op, code >= 500 and body.get('status') == 'error', [code, body])
            self.check('local outage never writes KB', self.digest() == before)
        finally:
            self.docker('start', self.args.store_db)
        self.good('local dependency recovers', self.wait('get', dict(id=mid)))
        self.docker('stop', self.args.kb)
        try:
            self.good('local get works with KB offline', self.call('get', dict(id=mid)))
            recall = self.good('local recall works with KB offline', self.call('recall', dict(query=self.prefix)))
            self.check('local recall retains scoped ID and prompt text', any(
                r.get('memory_id') == mid and r.get('text') == 'corrected local fixture'
                and r.get('handle') == 'user:memory:' + str(mid)
                for r in recall.get('recall', {}).get('active_context', [])))
            self.good('local session recall needs no hint or KB', self.call('recall', dict(session_start=True)))
            recalled = self.cli('recall', '--query', self.prefix, '--store', 'user')
            self.check('CLI recall works with KB offline', recalled.get('store') == 'user' and any(
                r.get('text') == 'corrected local fixture' and r.get('memory_id') == mid
                for r in recalled.get('recall', {}).get('active_context', [])), recalled)
            code, recalled = self.mcp('memory_recall', dict(task_hint=self.prefix, store='user'))
            self.check('MCP recall works with KB offline', code == 200 and 'corrected local fixture' in recalled)
            self.good('local store works with KB offline', self.call('store', dict(key=self.prefix + '-offline', content='private offline fixture')))
            code, body = self.call('list', {'store': 'kb'})
            self.check('explicit KB outage is an HTTP failure', code >= 500 and body.get('status') == 'error', [code, body])
        finally:
            self.docker('start', self.args.kb)
        self.good('KB recovers', self.wait('get', dict(id=mid, store='kb', scope='all')))
        # Pause only the KB memory process: daemon health can stay green while
        # the data stage is unavailable. An empty successful list is a defect.
        pid = int(self.docker('exec', self.args.kb, 'python3', '-c',
            "from pathlib import Path; import os,signal; "
            "pids=[int(p.name) for p in Path('/proc').iterdir() if p.name.isdigit() and "
            "(p/'cmdline').exists() and (p/'cmdline').read_bytes().split(b'\\0')[0].endswith(b'/aimee-module-memory')]; "
            "assert len(pids)==1,pids; os.kill(pids[0],signal.SIGSTOP); print(pids[0])"))
        try:
            for op in ('list', 'get'):
                code, body = self.call(op, dict(store='kb', id=mid, scope='all'))
                self.check('KB memory process outage fails '+op, code >= 500 and body.get('status') == 'error', [code, body])
            self.good('user store works during KB memory process outage', self.call('get', dict(id=mid)))
        finally:
            # A paused process can lose its bus lease. Let the existing image
            # supervisor replace it so recovery follows the production path.
            self.docker('exec', self.args.kb, 'python3', '-c',
                'import os,signal,sys; p=int(sys.argv[1]); os.kill(p,signal.SIGCONT); os.kill(p,signal.SIGTERM)', str(pid))
        self.good('KB memory process recovers', self.wait('get', dict(id=mid, store='kb', scope='all')))
        self.good('local retirement', self.call('delete', dict(id=mid)))
        code, body = self.call('get', dict(id=mid))
        self.check('retired local ID never resolves to KB collision', code >= 400 and body.get('kind') == 'not_found', [code, body])
        self.check('local retirement leaves KB content unchanged', json.loads(self.sql(f'SELECT to_json(content) FROM memories WHERE id={mid}')) == shared)
        self.shared_mcp_corrections()
        journal_id, journal = self.shared_journal()
        self.docker('restart', self.args.kb)
        self.good('shared replacement survives KB restart', self.wait('get', dict(store='kb', id=journal_id)))
        restarted = self.shared_changes(journal_id)
        # Background indexing may add the primary scope tag or normalize derived
        # fields after restart. Those are real governed mutations: require the
        # old events to survive exactly, while allowing newer committed events.
        self.check('shared journal owner and events survive KB restart',
                   restarted['owner_id'] == journal['owner_id'] and
                   restarted['revision'] >= journal['revision'] and
                   all(event in restarted['events'] for event in journal['events']) and
                   max(event['revision'] for event in restarted['events']) == restarted['revision'])
        correction, receipt = self.retry_correction
        code, replay = self.call('supersede', correction)
        # Background normalization can change the result revision after restart.
        # Replay may return the original commit only while that version remains
        # eligible; otherwise it must refuse without repeating the correction.
        self.check('shared correction receipt survives KB restart',
                   (code == 200 and replay.get('mutation_receipt', {}).get('replayed') is True and
                    replay.get('mutation_receipt', {}).get('commit_id') == receipt['commit_id']) or
                   (code == 409 and replay.get('reason') == 'idempotent_result_unavailable'))
        self.check('shared restarted correction never duplicates result',
                   self.sql(f"SELECT count(*) FROM memories WHERE key='{self.prefix}-journal' OR key LIKE '{self.prefix}-journal#v%'") == '2')
        self.sql(f"UPDATE memories SET lifecycle_state='retired' WHERE id={journal_id}")
        code, unavailable = self.call('supersede', correction)
        self.check('shared HTTP retry refuses retired result without cached content', code == 409 and
                   unavailable.get('reason') == 'idempotent_result_unavailable' and 'memory' not in unavailable and
                   'mutation_receipt' not in unavailable)
        long_shared = 'shared release fixture ' + 'αβ🦊 ' * 1000
        row = self.good('explicit long KB store', self.call('store', dict(store='kb', key=self.prefix + '-long', content=long_shared)))
        got = self.good('explicit long KB get', self.call('get', dict(store='kb', id=row['id'])))
        self.check('KB get preserves full long Unicode content', got.get('memory', {}).get('content') == long_shared)
        # Current lookup must hide retired records; explicit historical lookup
        # must still return their complete retained content and validity.
        self.sql(f"UPDATE memories SET lifecycle_state='retired', valid_from='2026-01-01T00:00:00Z', valid_until='2026-06-01T00:00:00Z' WHERE id={row['id']}")
        code, body = self.call('get', dict(store='kb', id=row['id']))
        self.check('ordinary KB get hides retired record', code >= 400 and body.get('kind') == 'not_found', [code, body])
        for as_of, expected_valid in [('2026-03-01T00:00:00Z', True), ('2026-07-01T00:00:00Z', False)]:
            got = self.good('historical retired KB get ' + as_of,
                self.call('get', dict(store='kb', id=row['id'], as_of=as_of)))
            self.check('historical KB get preserves content and validity ' + as_of,
                got.get('memory', {}).get('content') == long_shared and
                got.get('as_of') == as_of and got.get('valid_at') is expected_valid)
        for valid_at, expected_valid in [('2026-01-01T00:00:00Z', True),
                                         ('2026-03-01T00:00:00Z', True),
                                         ('2026-06-01T00:00:00Z', False),
                                         ('2025-12-31T23:59:59Z', False)]:
            code, got = self.call('get', dict(store='kb', id=row['id'], read_policy=dict(
                schema_version=1, mode='historical', valid_at=valid_at)))
            if expected_valid:
                self.check('versioned historical HTTP read ' + valid_at,
                    code == 200 and got.get('memory', {}).get('content') == long_shared and
                    got.get('read', {}).get('valid_at') == valid_at, [code, got.get('read')])
            else:
                self.check('versioned historical HTTP excludes ' + valid_at,
                    code == 404 and got.get('kind') == 'not_found' and 'memory' not in got, [code, got])
        for store in ('user', 'kb'):
            for policy, kind in [
                    (dict(schema_version=2, mode='current'), 'unsupported_version'),
                    (dict(schema_version=1, mode='current', believed_at='2026-01-01'), 'unsupported_mode')]:
                code, got = self.call('get', dict(store=store, id=mid, read_policy=policy))
                self.check('explicit HTTP read refusal ' + store + ' ' + kind,
                    code == 400 and got.get('kind') == kind and 'memory' not in got, [code, got])
        current = self.good('versioned personal current fixture', self.call('store', dict(
            store='user', key=self.prefix + '-current-policy', content='personal current policy fixture')))
        got = self.good('versioned personal current HTTP read', self.call('get', dict(
            store='user', id=current['id'], read_policy=dict(schema_version=1, mode='current'))))
        self.check('personal current HTTP reports applied policy', got.get('read', {}).get('mode') == 'current')
        code, got = self.call('get', dict(store='user', id=current['id'], read_policy=dict(
            schema_version=1, mode='historical', valid_at='2026-03-01')))
        self.check('personal historical HTTP refuses unsupported reconstruction',
            code == 400 and got.get('kind') == 'unsupported_mode' and 'memory' not in got, [code, got])
        if self.args.upgrade_fixture:
            rows = json.loads(Path(self.args.upgrade_fixture).read_text())
            for row in rows:
                got = self.good('0.4.1 upgrade get ' + row['key'], self.call('get', dict(store='kb', id=row['id'], scope='all')))
                self.check('0.4.1 upgrade preserves exact content ' + row['key'], got.get('memory', {}).get('content') == row['content'])
            listing = self.good('0.4.1 upgrade list', self.call('list', dict(store='kb', scope='all', limit=64)))
            self.check('0.4.1 rows remain discoverable', all(any(r['id'] == old['id'] for r in listing.get('memories', [])) for old in rows))
        self.assert_no_personal_canary()
        self.confidence_contract(('user', 'kb'))
        return all(c['passed'] for c in self.checks)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('server', 'store-db', 'output'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--kb-store-db', help='Standardized PostgreSQL container owned by KB')
    parser.add_argument('--kb', help='Shared container; omit to test a KB-free local composition')
    parser.add_argument('--upgrade-fixture')
    args = parser.parse_args()
    gate = Gate(args)
    try:
        passed = gate.run() if args.kb else gate.run_local()
    finally:
        Path(args.output).write_text(json.dumps(gate.checks, indent=2, ensure_ascii=False) + '\n')
    raise SystemExit(0 if passed else 1)
