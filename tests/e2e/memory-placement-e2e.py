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

    def hygiene_preview(self):
        scope = self.prefix + '-hygiene'
        content = 'duplicate candidate ' + scope
        self.sql(f"""INSERT INTO memories(key,content,scope_type,scope_value)
            VALUES('{scope}-a','{content}','project','{scope}'),
                  ('{scope}-b','{content}','project','{scope}'),
                  ('{scope}-c','unique candidate','project','{scope}'),
                  ('{scope}-hidden','{content}','project','{scope}-hidden'),
                  ('{scope}-expired','{content}','project','{scope}');
            UPDATE memories SET valid_until=(CURRENT_TIMESTAMP-interval '1 day')::text
            WHERE key='{scope}-expired'""")
        request = dict(dry_run=True, scope=dict(type='project', value=scope),
                       max_rows=64, max_content_bytes=16384)
        snapshot = f"SELECT id,key,content,lifecycle_state,valid_from,valid_until FROM memories WHERE scope_value LIKE '{scope}%' ORDER BY id"
        before = self.sql(snapshot)
        try:
            result = self.good('bounded hygiene HTTP preview', self.call('hygiene', request))
            findings = result.get('findings', [])
            targets = [v for f in findings for v in f.get('expected_versions', [])]
            ids = self.sql(f"SELECT id FROM memories WHERE key IN ('{scope}-a','{scope}-b') ORDER BY id").splitlines()
            self.check('hygiene keeps exact scope and eligible duplicate revisions',
                len(findings) == 1 and sorted(v.get('record_id') for v in targets) == sorted(ids)
                and all(v.get('record_revision') and v.get('owner_id') for v in targets)
                and result.get('rows_compared') == 3 and result.get('partial') is False, result)
            self.check('hygiene labels candidate-only findings without leaking content',
                all(f.get('uncertainty') == 'candidate_only' for f in findings)
                and content not in json.dumps(result) and result.get('canonical_writes') == 0
                and result.get('proposal_writes') == 0, result)
            repeated = self.good('hygiene repeat preview', self.call('hygiene', request))
            self.check('hygiene findings are stable on repeated read', repeated.get('findings') == findings)
            limited = self.good('hygiene bounded partial preview', self.call('hygiene', dict(request, max_rows=1)))
            self.check('hygiene row limit cannot claim clean full coverage', limited.get('partial') is True
                and limited.get('unvisited') == 'remaining_eligible_content_unknown'
                and limited.get('rows_compared') == 1 and limited.get('findings') == [], limited)
            limited = self.good('hygiene bounded content preview', self.call('hygiene', dict(request, max_content_bytes=1)))
            self.check('hygiene byte limit reports unvisited content', limited.get('partial') is True
                and limited.get('rows_compared') == 0 and limited.get('findings') == [], limited)
            for name, override in [('apply', dict(dry_run=False)), ('mutation', dict(operation='delete')),
                    ('sql', dict(sql='DELETE FROM memories')), ('all', dict(include_all=True)),
                    ('unbounded', dict(max_rows=0)), ('private', dict(store='user'))]:
                code, rejected = self.call('hygiene', dict(request, **override))
                self.check('hygiene refuses ' + name, code == 400 and rejected.get('kind') == 'invalid_argument', [code, rejected])
            self.check('hygiene leaves canonical fixtures unchanged', before == self.sql(snapshot))
        finally:
            self.sql(f"DELETE FROM memories WHERE scope_value IN ('{scope}','{scope}-hidden')")

    def protected_recall(self):
        # Seed the existing rules owner directly in this disposable fixture.
        # This checks packing, not authorization to promote a rule to hard.
        prefix = self.prefix + '-required-'
        self.sql(f"""INSERT INTO rules(polarity,title,description,weight,directive_type,created_at,updated_at)
            SELECT 'negative','{prefix}'||n,'Never exceed 10 EUR before 2026-10-01. Keep identifier '||n,
            1,'hard',pg_now_text(),pg_now_text() FROM generate_series(1,20) n""")
        try:
            for start in (False, True):
                body = self.good('complete hard-rule HTTP recall ' + str(start),
                    self.call('recall', dict(store='kb', scope='all', task_hint=self.prefix, limit_tokens=8192, session_start=start)))
                rules = body.get('recall', {}).get('always_on_rules', [])
                self.check('HTTP retains beyond former hard-rule cap ' + str(start),
                    len([r for r in rules if r['title'].startswith(prefix)]) == 20)
            cli = self.cli('recall', '--query', self.prefix, '--store', 'kb', '--scope', 'all', '--limit-tokens', '8192')
            self.check('CLI retains the complete hard-rule set', len([
                r for r in cli.get('recall', {}).get('always_on_rules', []) if r['title'].startswith(prefix)]) == 20)
            # A small hard rule survives oversized optional evidence at a budget
            # that fits the complete rule set but cannot fit the optional row.
            fixture = self.good('optional oversized recall fixture', self.call('store', dict(
                store='kb', key=self.prefix+'-packing', content='optional evidence ' * 500)))
            body = self.good('hard rules survive optional trimming', self.call('recall', dict(
                store='kb', scope='all', task_hint=self.prefix+'-packing', limit_tokens=1600)))
            bundle = body.get('recall', {})
            self.check('packing keeps complete rules and omits oversized optional row',
                len([r for r in bundle.get('always_on_rules', []) if r['title'].startswith(prefix)]) == 20 and
                all(r.get('id') != fixture['id'] for r in bundle.get('active_context', [])))
            code, body = self.call('recall', dict(store='kb', scope='all', task_hint=self.prefix, limit_tokens=64))
            self.check('HTTP exposes protected overflow without partial recall', code == 413 and
                body.get('kind') == 'protected_context_overflow' and 'recall' not in body, [code, body])
            for start in (False, True):
                mcp = self.mcp_document('MCP protected overflow ' + str(start), 'memory_recall',
                    dict(store='kb', scope='all', task_hint=self.prefix, limit_tokens=64, session_start=start))
                self.check('MCP preserves protected refusal before guidance ' + str(start),
                    mcp.get('kind') == 'protected_context_overflow' and 'recall' not in mcp)
            self.sql(f"UPDATE rules SET description=repeat('界',20000) WHERE title='{prefix}20'")
            code, body = self.call('recall', dict(store='kb', scope='all', task_hint=self.prefix, limit_tokens=8192))
            self.check('HTTP refuses oversized last rule instead of truncating', code == 413 and
                body.get('kind') == 'protected_context_overflow' and 'recall' not in body, [code, body])
        finally:
            self.sql(f"DELETE FROM rules WHERE title LIKE '{prefix}%'")
        self.good('recall recovers after protected overflow', self.call('recall', dict(store='kb', scope='all', task_hint=self.prefix)))

    def derived_parent_eligibility(self):
        # SQL seeds only this disposable fixture; every observation traverses
        # authenticated MCP, the C host/bus and the shared Go owner.
        key = self.prefix + '-derived-validity'
        text = 'eligible derived episode ' + self.prefix
        target = 'eligible-derived-target-' + self.prefix
        # The indexer owns episodes named after the parent key. Use a distinct
        # authored episode key so a background refresh cannot replace this fixture.
        mid = int(self.sql(f"""INSERT INTO memories(tier,kind,key,content,scope_type,scope_value)
            VALUES('L2','fact','{key}-parent','derived source','project','{key}') RETURNING id""").splitlines()[0])
        self.sql(f"""INSERT INTO memory_episodes(memory_id,episode_key,episode_text)
            VALUES({mid},'{key}','{text}');
            INSERT INTO memory_relations(memory_id,src_entity,relation,dst_entity,fact_text)
            VALUES({mid},'{key}','owns','{target}','derived fact')""")
        tools = [
            ('get_episode', dict(episode_key=key), text, 'memory episode not found'),
            ('search_graph', dict(query=key), target, 'No graph relations found'),
            # The authored relation survives refresh; generated mention rows do not.
            ('get_entity', dict(entity=key), 'Relations: 1', 'memory profile not found'),
            ('get_entity_edges', dict(entity=key), target, 'No edges found'),
        ]
        try:
            for state, update, visible in [
                ('current', "valid_from='',valid_until=''", True),
                ('future', "valid_from=(CURRENT_TIMESTAMP+interval '1 day')::text,valid_until=''", False),
                ('expired', "valid_from='',valid_until=(CURRENT_TIMESTAMP-interval '1 day')::text", False),
                ('suppressed', "valid_until='',activation_suppressed=1", False),
                ('retired', "activation_suppressed=0,lifecycle_state='retired'", False),
                ('restored', "lifecycle_state='active',valid_from='',valid_until=''", True),
            ]:
                self.sql(f'UPDATE memories SET {update} WHERE id={mid}')
                for tool, args, found, absent in tools:
                    code, body = self.mcp(tool, dict(args, project=key))
                    passed = code == 200 and (found in body if visible else absent in body and found not in body)
                    self.check('derived parent ' + state + ' ' + tool, passed,
                        None if passed else [code, body])
        finally:
            self.sql(f'DELETE FROM memories WHERE id={mid}')

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
            dict(verb='update', store='user', id=str(old_id), content='wrong-owner private correction',
                 expected_version=version, idempotency_key=self.prefix + '-private-retry'))
        self.check('MCP private placement refuses a shared owner version', private.get('reason') == 'expected_version_conflict')
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

    def shared_keyed_deletion(self):
        key = self.prefix + '-shared-deletion'
        human = self.good('shared destructive deletion parent', self.call('store', dict(store='kb', key=key, content='explicit user deletion fixture')))
        mid = human['id']
        # Wait for fixture preparation, not for a failed mutation to succeed.
        # Indexed children previously made the deletion's evidence trigger fail.
        deadline = time.monotonic() + 60
        indexed = False
        while time.monotonic() < deadline:
            indexed = self.sql(f"""SELECT (EXISTS(SELECT 1 FROM kb_async_jobs WHERE kind='memory_index'
                AND document_id={int(mid)} AND status='done') AND
                EXISTS(SELECT 1 FROM memory_units WHERE memory_id={int(mid)}) AND
                EXISTS(SELECT 1 FROM memory_summaries WHERE memory_id={int(mid)}))::text""") == 'true'
            if indexed:
                break
            time.sleep(0.2)
        self.check('shared deletion fixture indexed before mutation', indexed)
        if not indexed:
            raise RuntimeError('shared deletion fixture indexing did not complete')
        version = self.good('shared deletion expected version', self.call('get', dict(store='kb', id=mid, include_version=True)))['memory']['version']
        request = dict(store='kb', id=str(mid), expected_version=version, idempotency_key=key+'-retry')
        refused = self.mcp_document('model cannot elevate shared deletion', 'mutate', dict(verb='forget', authority='user', **request))
        self.check('shared deletion precondition grants no user authority', refused.get('kind') == 'review_required')
        stale = dict(version, record_revision='9223372036854775807')
        code, conflict = self.call('delete', dict(request, expected_version=stale))
        self.check('shared deletion rejects a stale target', code == 409 and conflict.get('reason') == 'expected_version_conflict')
        self.sql("ALTER TABLE memory_mutation_receipts ADD CONSTRAINT shared_delete_failure CHECK(operation<>'destroyed') NOT VALID")
        try:
            before = self.shared_changes(mid)
            code, failed = self.call('delete', request)
            self.check('shared destruction refuses a receipt write failure', code >= 500 and failed.get('status') == 'error')
            current = self.good('failed shared destruction preserves its parent', self.call('get', dict(store='kb', id=mid, include_version=True)))['memory']
            self.check('shared destruction failure rolls back revision and invalidation', current['version'] == version and self.shared_changes(mid) == before)
        finally:
            self.sql('ALTER TABLE memory_mutation_receipts DROP CONSTRAINT shared_delete_failure')
        first = self.good('shared keyed destruction commits', self.call('delete', request))
        receipt = first['mutation_receipt']
        self.check('shared destruction receipt invents no current record version', first.get('destroyed') is True and receipt['schema_version'] == 2 and
            receipt['outcome'] == 'destroyed' and receipt['target_version'] == version and 'version' not in receipt and receipt['replayed'] is False)
        self.check('shared destruction is physically applied and audited as irreversible', self.sql(f"SELECT count(*) FROM memories WHERE id={int(mid)}") == '0' and
            self.sql(f"SELECT reversible FROM fact_graph_commits WHERE commit_id='{receipt['commit_id']}'") == '0')
        replay = self.good('shared destruction HTTP retry', self.call('delete', request))
        self.check('shared destruction retry preserves its committed identity', replay['mutation_receipt']['replayed'] is True and replay['mutation_receipt']['commit_id'] == receipt['commit_id'])
        code, conflict = self.call('delete', dict(request, expected_version=stale))
        self.check('shared destruction key rejects a changed request', code == 409 and conflict.get('reason') == 'idempotency_conflict')
        self.shared_destroy_retry = (request, receipt)
        model_key = key+'-model'
        refused = self.mcp_document('MCP shared store without project context', 'mutate', dict(
            verb='store', store='kb', key=model_key, content='must not persist without context'))
        self.check('shared store rejects the missing-context marker before writing',
            refused.get('kind') == 'invalid_argument' and refused.get('reason') == 'active_context_missing' and
            self.sql(f"SELECT count(*) FROM memories WHERE key='{model_key}'") == '0')
        project = self.prefix+'-retirement-project'
        code, created = self.mcp('mutate', dict(verb='store', store='kb', project=project, key=model_key, content='model retirement fixture'))
        self.check('MCP creates shared retirement parent', code == 200 and 'stored memory id=' in created)
        model_id = int(self.sql(f"SELECT id FROM memories WHERE key='{model_key}' AND lifecycle_state='active'"))
        version = self.mcp_document('MCP shared retirement expected version', 'memory_get', dict(store='kb', project=project, id=str(model_id), include_version=True))['memory']['version']
        args = dict(verb='forget', store='kb', project=project, id=str(model_id), expected_version=version, idempotency_key=key+'-model-retry')
        first = self.mcp_document('MCP keyed shared retirement', 'mutate', args)
        receipt = first['mutation_receipt']
        self.check('MCP shared retirement receipt binds both versions', receipt['schema_version'] == 2 and receipt['outcome'] == 'retired' and
            receipt['target_version'] == version and receipt['version']['record_id'] == str(model_id) and receipt['version'] != version and receipt['replayed'] is False)
        committed = self.shared_changes(model_id)
        replay = self.mcp_document('MCP shared retirement retry', 'mutate', args)
        self.check('MCP shared retirement replay repeats no mutation or host audit', replay['mutation_receipt']['commit_id'] == receipt['commit_id'] and
            replay['mutation_receipt']['replayed'] is True and 'audit_id' not in replay and self.shared_changes(model_id) == committed)
        self.shared_retire_retry = (args, receipt)

    def shared_deletion_after_restart(self):
        request, receipt = self.shared_destroy_retry
        replay = self.good('shared destruction receipt after KB restart', self.call('delete', request))
        self.check('shared destruction restart retry retains one commit', replay['mutation_receipt']['replayed'] is True and replay['mutation_receipt']['commit_id'] == receipt['commit_id'])
        mid = int(request['id'])
        self.sql(f"INSERT INTO memories(id,key,content,tier,kind,scope_type,scope_value,provenance_category) VALUES({mid},'{self.prefix}-resurrected','restored hidden record','L2','fact','project','hidden-restoration','user_stated')")
        code, unavailable = self.call('delete', request)
        self.check('shared destruction retry cannot certify a hidden restored ID as absent', code == 409 and unavailable.get('reason') == 'idempotent_result_unavailable' and 'mutation_receipt' not in unavailable)
        self.sql(f'DELETE FROM memories WHERE id={mid}')
        args, receipt = self.shared_retire_retry
        replay = self.mcp_document('MCP shared retirement after KB restart', 'mutate', args)
        self.check('shared retirement restart retry retains one commit', replay.get('mutation_receipt', {}).get('replayed') is True and replay['mutation_receipt']['commit_id'] == receipt['commit_id'])
        mid = int(args['id'])
        self.sql(f"UPDATE memories SET lifecycle_state='active',activation_suppressed=0,valid_until=NULL WHERE id={mid}")
        unavailable = self.mcp_document('MCP retirement of reactivated result', 'mutate', args)
        self.check('shared retirement retry cannot repeat after reactivation', unavailable.get('reason') == 'idempotent_result_unavailable' and 'mutation_receipt' not in unavailable)
        self.sql(f'DELETE FROM memories WHERE id={mid}')
        unavailable = self.mcp_document('MCP retirement of erased result', 'mutate', args)
        self.check('shared retirement key survives erasure', unavailable.get('reason') == 'idempotent_result_unavailable' and
            self.sql(f"SELECT count(*) FROM memory_mutation_receipts WHERE result_id={mid} AND operation='retired'") == '1')

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
                    refused.get('kind') == 'review_required' and 'mutation_receipt' not in refused and
                    refused.get('proposal', {}).get('state') == 'pending')
                # Server HTTP exposes supersede; update is an MCP/KB action.
                if verb == 'supersede':
                    code, failure = self.call(verb, dict(args, idempotency_key=key + '-canonical'))
                    self.check('HTTP supersede admitted correction reaches blocked audit',
                        code >= 500 and failure.get('kind') == 'unavailable')
                args['expected_version'] = dict(version, record_revision='9223372036854775807')
                args['idempotency_key'] = key + '-stale'
                stale = self.mcp_document('MCP ' + verb + ' stale admission before audit',
                    'mutate', dict(args, verb=verb))
                self.check('MCP ' + verb + ' rejects stale version before canonical audit',
                    stale.get('reason') == 'expected_version_conflict')
        finally:
            self.sql('ALTER TABLE fact_graph_commits DROP CONSTRAINT e2e_admission_audit_failure')
        self.check('refused corrections preserve original revision and invalidations',
            self.shared_changes(old_id) == before)
        hashes = ','.join("'" + key + "'" for key in keys)
        self.check('draft outcomes retain their retry references', self.sql(
            f"SELECT count(*) FROM memory_mutation_receipts WHERE key_hash IN ({hashes}) AND proposal_id IS NOT NULL") == '2')

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

    def personal_versions(self):
        key = self.prefix + '-history'
        created = self.good('personal history fixture', self.call('store',
            dict(key=key, content='private original revision')))
        mid = created['id']
        observed = self.good('personal HTTP versioned get', self.call('get',
            dict(id=mid, include_version=True)))['memory']
        version = observed['version']
        self.check('personal version uses owner and exact decimal identifiers',
            version['schema_version'] == 1 and version['record_id'] == str(mid) and
            version['record_revision'] == '1' and len(version['owner_id']) == 36)
        self.good('personal same-key correction', self.call('store',
            dict(key=key, content='private second revision')))
        history = self.good('personal explicit historical get', self.call('get',
            dict(id=mid, at_version=version)))['memory']
        self.check('same-key correction retains labelled original revision',
            history['content'] == 'private original revision' and history.get('historical') is True and
            history.get('version') == version)
        code, stale = self.call('supersede', dict(old_id=mid, new_content='stale edit', expected_version=version))
        self.check('personal HTTP correction rejects stale version', code == 409 and
            stale.get('reason') == 'expected_version_conflict')
        current = self.mcp_document('personal MCP versioned get', 'memory_get',
            dict(id=str(mid), include_version=True))['memory']
        correction = dict(verb='update', id=str(mid), content='private third revision',
            expected_version=current['version'])
        refused = self.mcp_document('personal MCP authoritative correction refusal', 'mutate', correction)
        self.check('personal MCP cannot replace HTTP-authored content', refused.get('kind') == 'review_required')
        changed = self.good('personal HTTP authorized versioned correction', self.call('supersede',
            dict(old_id=mid, new_content='private third revision', expected_version=current['version'])))
        self.check('personal authorized correction returns committed revision', changed.get('status') == 'ok' and
            changed.get('version', {}).get('record_revision') == '3')
        self.check('private HTTP captures verified user authorship',
            changed.get('authorship', {}).get('category') == 'user_stated' and
            bool(changed.get('authorship', {}).get('principal')))
        model = self.mcp_document('personal MCP model fixture', 'mutate', dict(verb='store',
            key=key+'-model', content='private model original', confidence=1,
            authority='user', provenance_category='user_stated', author_principal='forged'))
        model_id = model['id']
        model_record = self.mcp_document('personal model version', 'memory_get',
            dict(id=str(model_id), include_version=True))['memory']
        self.check('private MCP cannot forge user provenance or certainty',
            model_record.get('authorship', {}).get('category') == 'agent_message' and
            model_record.get('authorship', {}).get('principal') != 'forged' and model_record['confidence'] == 0.8)
        model_changed = self.mcp_document('personal MCP model correction', 'mutate', dict(verb='update',
            id=str(model_id), content='private model corrected', expected_version=model_record['version']))
        self.check('private MCP can correct model content with version admission',
            model_changed.get('status') == 'ok' and
            model_changed.get('records', [{}])[0].get('version', {}).get('record_revision') == '2')
        model_history = self.mcp_document('personal MCP model history', 'memory_get',
            dict(id=str(model_id), at_version=model_record['version']))['memory']
        self.check('private model history retains original authorship',
            model_history['content'] == 'private model original' and model_history.get('authorship') == model_record.get('authorship'))
        protected_before = self.personal_changes(mid)
        for verb, fields in (('store', dict(key=key, content='forged replacement')),
                             ('forget', dict(id=str(mid)))):
            refused = self.mcp_document('private MCP authoritative '+verb+' refusal', 'mutate', dict(verb=verb, authority='user', **fields))
            self.check('private MCP '+verb+' cannot mutate user content', refused.get('kind') == 'review_required')
        self.check('private authority refusals leave canonical revision unchanged',
            self.personal_changes(mid) == protected_before)
        stale = self.mcp_document('personal MCP stale retry', 'mutate', correction)
        self.check('personal MCP rejects stale correction retry', stale.get('reason') == 'expected_version_conflict')
        historical = self.mcp_document('personal MCP historical get', 'memory_get',
            dict(id=str(mid), at_version=current['version']))['memory']
        self.check('MCP history preserves exact second revision', historical['content'] == 'private second revision' and
            historical.get('historical') is True and historical.get('version') == current['version'])
        wrong = dict(version, owner_id='00000000-0000-0000-0000-000000000001')
        code, unavailable = self.call('get', dict(id=mid, at_version=wrong))
        self.check('personal history rejects another owner', code == 404 and unavailable.get('kind') == 'not_found')
        self.check('private runtime cannot rewrite retained history', self.personal_sql(
            "SELECT (NOT has_table_privilege('aimee_store_runtime','user_memory_versions','INSERT') "
            "AND NOT has_table_privilege('aimee_store_runtime','user_memory_versions','UPDATE') "
            "AND NOT has_table_privilege('aimee_store_runtime','user_memory_versions','DELETE'))::text") == 'true')
        self.check('private runtime cannot bypass retirement with physical erase', self.personal_sql(
            "SELECT (NOT has_table_privilege('aimee_store_runtime','user_memories','DELETE,TRUNCATE'))::text") == 'true')
        self.check('private runtime cannot rewrite invalidation progress', self.personal_sql(
            "SELECT (NOT has_table_privilege('aimee_store_runtime','user_memory_collection_generation','INSERT,UPDATE,DELETE,TRUNCATE') "
            "AND NOT has_table_privilege('aimee_store_runtime','user_memory_invalidation_outbox','INSERT,UPDATE,DELETE,TRUNCATE'))::text") == 'true')
        self.check('private runtime cannot invoke protected trigger helpers', self.personal_sql(
            "SELECT (NOT has_function_privilege('aimee_store_runtime','user_memory_retain_version()','EXECUTE') "
            "AND NOT has_function_privilege('aimee_store_runtime','user_memory_capture_change()','EXECUTE') "
            "AND NOT has_function_privilege('aimee_store_runtime','user_memory_assign_revision()','EXECUTE'))::text") == 'true')
        self.docker('restart', self.args.server)
        current = self.good('personal versions survive restart', self.wait('get', dict(id=mid, include_version=True)))['memory']
        history = self.good('retained private history survives restart', self.call('get', dict(id=mid, at_version=version)))['memory']
        self.check('restart retains current and original private versions', current['content'] == 'private third revision' and
            current['version']['record_revision'] == '3' and history['content'] == 'private original revision')
        before = self.personal_changes(mid)
        self.personal_sql(f'ALTER TABLE user_memory_versions ADD CONSTRAINT e2e_history_failure CHECK(memory_id<>{int(mid)}) NOT VALID')
        try:
            code, failure = self.call('supersede', dict(old_id=mid, new_content='failed history edit', expected_version=current['version']))
            self.check('history persistence failure refuses correction', code >= 500 and failure.get('status') == 'error')
        finally:
            self.personal_sql('ALTER TABLE user_memory_versions DROP CONSTRAINT e2e_history_failure')
        got = self.good('personal version after history failure', self.call('get', dict(id=mid, include_version=True)))['memory']
        self.check('history failure rolls back content version and invalidation', got == current and self.personal_changes(mid) == before)
        self.personal_sql(f'DELETE FROM user_memories WHERE id={int(mid)}')
        self.check('parent erasure removes private history payloads', self.personal_sql(
            f'SELECT count(*) FROM user_memory_versions WHERE memory_id={int(mid)}') == '0')
        code, erased = self.call('get', dict(id=mid, at_version=version))
        self.check('erased private revision cannot be retrieved', code == 404 and erased.get('kind') == 'not_found')

    def personal_correction_review(self):
        key = self.prefix + '-private-review'
        created = self.good('private review parent', self.call('store', dict(key=key, content='private human assertion')))
        mid = created['id']
        original = self.good('private review original version', self.call('get', dict(id=mid, include_version=True)))['memory']
        before = self.personal_changes(mid)
        correction = dict(verb='update', store='user', id=str(mid), content='private model draft', expected_version=original['version'])
        proposed = self.mcp_document('private MCP correction draft', 'mutate', correction)
        proposal = proposed['proposal']
        self.check('private refusal links a non-serving model draft', proposed.get('kind') == 'review_required' and
            proposal['state'] == 'pending' and proposal.get('origin_authority') == 'model' and 'draft' not in proposal)
        current = self.good('private parent after draft', self.call('get', dict(id=mid, include_version=True)))['memory']
        self.check('private draft leaves serving version and journal unchanged', current == original and self.personal_changes(mid) == before)
        retry = self.mcp_document('private repeated draft', 'mutate', correction)['proposal']
        self.check('private proposal retry preserves original draft identity', retry['proposal_id'] == proposal['proposal_id'])
        inspected = self.good('private exact draft inspection', self.call('correction_proposals',
            dict(store='user', proposal_id=proposal['proposal_id'])))['proposals']
        self.check('private inspection returns screened model payload', len(inspected) == 1 and
            inspected[0]['draft']['content'] == correction['content'] and inspected[0]['draft']['confidence'] == 0.8)
        review = dict(store='user', proposal_id=proposal['proposal_id'], payload_digest=proposal['payload_digest'],
            expected_version=proposal['target_version'], action='approve')
        code, refused = self.call('review_correction', dict(review, payload_digest='0'*64))
        self.check('private review refuses a different draft digest', code == 409 and refused.get('kind') == 'conflict')
        self.personal_sql("ALTER TABLE user_memory_correction_proposals ADD CONSTRAINT e2e_private_review_failure "
            f"CHECK(proposal_id<>'{proposal['proposal_id']}'::uuid OR state='pending') NOT VALID")
        try:
            code, failure = self.call('review_correction', review)
            self.check('private review refuses late decision persistence failure', code >= 500 and failure.get('status') == 'error')
            current = self.good('private parent after failed review', self.call('get', dict(id=mid, include_version=True)))['memory']
            self.check('failed private review rolls back content history and journal', current == original and self.personal_changes(mid) == before and
                self.personal_sql(f'SELECT count(*) FROM user_memory_versions WHERE memory_id={int(mid)}') == '0')
        finally:
            self.personal_sql('ALTER TABLE user_memory_correction_proposals DROP CONSTRAINT e2e_private_review_failure')
        approved = self.good('private exact draft approval', self.call('review_correction', review))['proposal']
        current = self.good('private reviewed memory', self.call('get', dict(id=mid, include_version=True)))['memory']
        self.check('private approval preserves model origin and records the reviewer', approved['state'] == 'approved' and
            current['version'] == approved['result_version'] and current['content'] == correction['content'] and
            current['confidence'] == 0.8 and current['authorship']['category'] == 'reviewed_model' and
            current['authorship']['principal'] == proposal['proposer'] and
            current['authorship']['reviewer'] == approved['reviewer'] and current['authorship']['proposal_id'] == proposal['proposal_id'])
        history = self.good('private history after reviewed correction', self.call('get', dict(id=mid, at_version=original['version'])))['memory']
        self.check('reviewed private correction retains human source revision', history['content'] == original['content'] and
            history['authorship'] == original['authorship'])
        committed = self.personal_changes(mid)
        replay = self.good('private approved decision retry', self.call('review_correction', review))['proposal']
        self.check('private decision retry has no second canonical effect', replay.get('replayed') is True and
            replay['decision_id'] == approved['decision_id'] and self.personal_changes(mid) == committed)
        another = self.mcp_document('private reviewed content still requires review', 'mutate',
            dict(correction, content='private rejected draft', expected_version=current['version']))['proposal']
        rejection = dict(store='user', proposal_id=another['proposal_id'], payload_digest=another['payload_digest'],
            expected_version=another['target_version'], action='reject')
        rejected = self.good('private correction rejection', self.call('review_correction', rejection))['proposal']
        again = self.mcp_document('private rejected suggestion retry', 'mutate',
            dict(correction, content='private rejected draft', expected_version=current['version']))['proposal']
        self.check('private rejected draft cannot reopen or alter recall', rejected['state'] == 'rejected' and
            again['proposal_id'] == another['proposal_id'] and again['state'] == 'rejected' and self.personal_changes(mid) == committed)
        self.docker('restart', self.args.server)
        recovered = self.good('private review recovery after restart', self.wait('get', dict(id=mid, include_version=True)))['memory']
        replay = self.good('private decision survives restart', self.call('review_correction', review))['proposal']
        self.check('restart preserves reviewed version author and decision', recovered == current and
            replay.get('replayed') is True and replay['decision_id'] == approved['decision_id'])
        self.good('private reviewed memory retirement', self.call('delete', dict(id=mid)))
        code, unavailable = self.call('review_correction', review)
        self.check('private review retry cannot release a retired result', code == 409 and unavailable.get('reason') == 'idempotent_result_unavailable')
        self.personal_sql(f'DELETE FROM user_memories WHERE id={int(mid)}')
        self.check('private parent erasure removes correction drafts', self.personal_sql(
            f'SELECT count(*) FROM user_memory_correction_proposals WHERE target_id={int(mid)}') == '0')
        code, unavailable = self.call('review_correction', review)
        self.check('private erased proposal cannot be reviewed again', code == 404 and unavailable.get('kind') == 'not_found')

    def personal_keyed_corrections(self):
        key = self.prefix + '-private-keyed'
        created = self.good('private keyed correction parent', self.call('store', dict(key=key, content='private original')))
        mid = created['id']
        original = self.good('private keyed original version', self.call('get', dict(id=mid, include_version=True)))['memory']
        correction = dict(old_id=mid, new_content='private corrected', expected_version=original['version'],
            idempotency_key=key + '-retry')
        before = self.personal_changes(mid)
        self.personal_sql('ALTER TABLE user_memory_mutation_receipts ADD CONSTRAINT e2e_private_retry_failure CHECK(false) NOT VALID')
        try:
            code, failed = self.call('supersede', correction)
            self.check('private keyed correction refuses receipt failure', code >= 500 and failed.get('status') == 'error')
            current = self.good('private keyed parent after failure', self.call('get', dict(id=mid, include_version=True)))['memory']
            self.check('private receipt failure rolls back content and journal', current == original and self.personal_changes(mid) == before)
        finally:
            self.personal_sql('ALTER TABLE user_memory_mutation_receipts DROP CONSTRAINT e2e_private_retry_failure')
        first = self.good('private keyed correction commits', self.call('supersede', correction))
        receipt = first['mutation_receipt']
        self.check('private receipt binds stable ID and new version', receipt['replayed'] is False and
            receipt['version']['record_id'] == str(mid) and receipt['version'] == first['version'] and
            first['version'] != original['version'])
        committed = self.personal_changes(mid)
        replay = self.good('private keyed HTTP retry', self.call('supersede', correction))
        self.check('private HTTP retry preserves one commit and canonical effect', replay['mutation_receipt']['replayed'] is True and
            replay['mutation_receipt']['commit_id'] == receipt['commit_id'] and self.personal_changes(mid) == committed)
        code, conflict = self.call('supersede', dict(correction, new_content='different private correction'))
        self.check('private HTTP retry rejects changed payload', code == 409 and conflict.get('reason') == 'idempotency_conflict')
        self.docker('restart', self.args.server)
        self.good('private keyed result after restart', self.wait('get', dict(id=mid)))
        replay = self.good('private keyed retry after restart', self.call('supersede', correction))
        self.check('private retry receipt survives Server restart', replay['mutation_receipt']['replayed'] is True and
            replay['mutation_receipt']['commit_id'] == receipt['commit_id'] and self.personal_changes(mid) == committed)
        self.good('private keyed result retirement', self.call('delete', dict(id=mid)))
        code, hidden = self.call('supersede', correction)
        self.check('private retry cannot release retired content', code == 409 and hidden.get('reason') == 'idempotent_result_unavailable' and 'mutation_receipt' not in hidden)
        self.personal_sql(f'DELETE FROM user_memories WHERE id={int(mid)}')
        code, hidden = self.call('supersede', correction)
        self.check('private erasure does not free a committed retry key', code == 409 and hidden.get('reason') == 'idempotent_result_unavailable' and
            self.personal_sql(f'SELECT count(*) FROM user_memory_mutation_receipts WHERE target_id={int(mid)}') == '1')
        model = self.mcp_document('private keyed model fixture', 'mutate', dict(verb='store', key=key + '-model', content='model original'))
        model_id = model['id']
        version = self.good('private model retry version', self.call('get', dict(id=model_id, include_version=True)))['memory']['version']
        args = dict(verb='update', store='user', id=str(model_id), content='model corrected', expected_version=version, idempotency_key=key + '-model-retry')
        first = self.mcp_document('private keyed MCP correction', 'mutate', args)
        replay = self.mcp_document('private keyed MCP retry', 'mutate', args)
        self.check('private MCP preserves durable receipt and capped model authority', first['mutation_receipt']['replayed'] is False and
            replay['mutation_receipt']['replayed'] is True and first['mutation_receipt']['commit_id'] == replay['mutation_receipt']['commit_id'] and
            replay['records'][0]['authorship']['category'] == 'agent_message' and replay['records'][0]['confidence'] == 0.8)
        human = self.good('private keyed proposal parent', self.call('store', dict(key=key + '-human', content='human assertion')))
        human_id = human['id']
        version = self.good('private keyed proposal version', self.call('get', dict(id=human_id, include_version=True)))['memory']['version']
        args = dict(verb='update', store='user', id=str(human_id), content='keyed model proposal', expected_version=version, idempotency_key=key + '-proposal')
        first = self.mcp_document('private keyed model proposal', 'mutate', args)
        replay = self.mcp_document('private keyed model proposal retry', 'mutate', args)
        self.check('private proposal retry retains non-serving identity', first.get('kind') == 'review_required' and
            replay['proposal']['replayed'] is True and replay['proposal']['proposal_id'] == first['proposal']['proposal_id'] and 'draft' not in replay['proposal'])
        conflict = self.mcp_document('private keyed changed draft', 'mutate', dict(args, content='different keyed model proposal'))
        self.check('private proposal retry rejects changed draft', conflict.get('reason') == 'idempotency_conflict')
        p = first['proposal']
        self.good('private keyed proposal rejection', self.call('review_correction', dict(store='user', proposal_id=p['proposal_id'],
            payload_digest=p['payload_digest'], expected_version=p['target_version'], action='reject')))
        replay = self.mcp_document('private keyed rejected proposal retry', 'mutate', args)
        self.check('private keyed rejected proposal cannot reopen', replay['proposal']['state'] == 'rejected' and replay['proposal']['replayed'] is True)
        self.personal_sql(f'DELETE FROM user_memories WHERE id={int(human_id)}')
        replay = self.mcp_document('private keyed erased proposal retry', 'mutate', args)
        self.check('private erased proposal key cannot repeat a mutation', replay.get('reason') == 'idempotent_result_unavailable' and 'proposal' not in replay)

    def personal_keyed_retirement(self):
        key = self.prefix + '-retirement'
        mid = self.good('private retirement parent', self.call('store', dict(key=key, content='retained original')))['id']
        original = self.good('private retirement expected version', self.call('get', dict(id=mid, include_version=True)))['memory']
        request = dict(id=str(mid), expected_version=original['version'], idempotency_key=key + '-retry')
        before = self.personal_changes(mid)
        self.personal_sql("ALTER TABLE user_memory_mutation_receipts ADD CONSTRAINT e2e_retirement_failure CHECK(operation<>'delete') NOT VALID")
        try:
            code, failed = self.call('delete', request)
            self.check('private retirement refuses receipt failure', code >= 500 and failed.get('status') == 'error')
            current = self.good('private retirement rollback parent', self.call('get', dict(id=mid, include_version=True)))['memory']
            self.check('private retirement rolls back content and invalidation', current == original and self.personal_changes(mid) == before)
        finally:
            self.personal_sql('ALTER TABLE user_memory_mutation_receipts DROP CONSTRAINT e2e_retirement_failure')
        first = self.good('private conditional retirement commits', self.call('delete', request))
        receipt = first['mutation_receipt']
        self.check('private retirement receipt binds a non-destructive transition', first.get('deleted') is True and
            first.get('destroyed') is False and receipt['replayed'] is False and
            receipt['version']['record_id'] == str(mid) and receipt['version'] != original['version'] and 'memory' not in first)
        code, missing = self.call('get', dict(id=mid))
        self.check('private conditional retirement removes current recall', code == 404 and missing.get('kind') == 'not_found')
        committed = self.personal_changes(mid)
        replay = self.good('private retirement HTTP retry', self.call('delete', request))
        self.check('private retirement retry preserves one commit and invalidation', replay['mutation_receipt']['replayed'] is True and
            replay['mutation_receipt']['commit_id'] == receipt['commit_id'] and self.personal_changes(mid) == committed)
        code, conflict = self.call('delete', dict(request, expected_version=receipt['version']))
        self.check('private retirement rejects changed retry version', code == 409 and conflict.get('reason') == 'idempotency_conflict')
        self.docker('restart', self.args.server)
        replay = self.good('private retirement retry after Server restart', self.wait('delete', request))
        self.check('private retirement receipt survives restart without another effect', replay['mutation_receipt']['replayed'] is True and
            replay['mutation_receipt']['commit_id'] == receipt['commit_id'] and self.personal_changes(mid) == committed)
        self.good('private retired history stays governed', self.call('get', dict(id=mid, at_version=original['version'])))
        self.good('private authorized reactivation', self.call('store', dict(key=key, content='reactivated assertion')))
        code, unavailable = self.call('delete', request)
        self.check('private retirement retry cannot retire reactivated content', code == 409 and unavailable.get('reason') == 'idempotent_result_unavailable')
        self.personal_sql(f'DELETE FROM user_memories WHERE id={int(mid)}')
        code, unavailable = self.call('delete', request)
        self.check('private erasure retains retirement retry identity', code == 409 and unavailable.get('reason') == 'idempotent_result_unavailable' and
            self.personal_sql(f'SELECT count(*) FROM user_memory_mutation_receipts WHERE target_id={int(mid)}') == '1')
        model_id = self.mcp_document('private model retirement parent', 'mutate', dict(verb='store', key=key+'-model', content='model assertion'))['id']
        version = self.good('private model retirement version', self.call('get', dict(id=model_id, include_version=True)))['memory']['version']
        args = dict(verb='forget', store='user', id=str(model_id), expected_version=version, idempotency_key=key+'-model-retry')
        first = self.mcp_document('private keyed MCP retirement', 'mutate', args)
        replay = self.mcp_document('private keyed MCP retirement retry', 'mutate', args)
        self.check('private MCP retirement returns one non-destructive receipt', first.get('destroyed') is False and
            first['mutation_receipt']['replayed'] is False and replay['mutation_receipt']['replayed'] is True and
            first['mutation_receipt']['commit_id'] == replay['mutation_receipt']['commit_id'])
        human_id = self.good('private protected retirement parent', self.call('store', dict(key=key+'-human', content='human assertion')))['id']
        version = self.good('private protected retirement version', self.call('get', dict(id=human_id, include_version=True)))['memory']['version']
        refused = self.mcp_document('model conditional retirement of user assertion', 'mutate', dict(verb='forget', id=str(human_id),
            expected_version=version, idempotency_key=key+'-forged', authority='user'))
        self.check('model retirement cannot claim user authority', refused.get('kind') == 'review_required')
        self.good('private protected retirement cleanup', self.call('delete', dict(id=human_id)))

    def keyed_creation(self, store):
        key = self.prefix + '-create-' + store
        context = dict(store=store)
        if store == 'kb':
            context['project'] = key
        request = dict(context, key=key, content='durable creation fixture', idempotency_key=key+'-retry')
        row = self.good(store+' keyed HTTP creation', self.call('store', request))
        receipt = row.get('mutation_receipt', {})
        self.check(store+' creation receipt binds exact result', receipt.get('schema_version') == 2 and
            receipt.get('outcome') == 'stored' and receipt.get('replayed') is False and
            receipt.get('version', {}).get('record_id') == str(row['id']) and bool(receipt.get('commit_id')))
        changes = self.personal_changes if store == 'user' else self.shared_changes
        before = changes(row['id'])
        replay = self.good(store+' keyed HTTP creation replay', self.call('store', request))
        self.check(store+' replay preserves commit and invalidation', replay.get('id') == row['id'] and
            replay.get('mutation_receipt') == dict(receipt, replayed=True) and changes(row['id']) == before)
        code, conflict = self.call('store', dict(request, content='changed request'))
        self.check(store+' changed creation retry conflicts', code >= 400 and conflict.get('reason') == 'idempotency_conflict')
        self.docker('restart', self.args.server if store == 'user' else self.args.kb)
        self.good(store+' creation result survives restart', self.wait('get', dict(context, id=row['id'])))
        replay = self.good(store+' creation replay survives restart', self.call('store', request))
        self.check(store+' restart preserves creation commit', replay.get('mutation_receipt') == dict(receipt, replayed=True))
        # MCP is model authority; it must retain the same retry identity without
        # promoting authorship even when the request carries an authority field.
        args = dict(context, verb='store', key=key+'-model', content='model creation fixture',
                    confidence=1.0, authority='user', idempotency_key=key+'-model-retry')
        model = self.mcp_document(store+' keyed MCP creation', 'mutate', args)
        model_receipt = model.get('mutation_receipt', {})
        self.check(store+' MCP creation has a durable receipt', model.get('status') == 'ok' and
            model_receipt.get('schema_version') == 2 and model_receipt.get('outcome') == 'stored' and
            str(model_receipt.get('version', {}).get('record_id', '')).isdigit())
        repeated = self.mcp_document(store+' MCP creation replay', 'mutate', args)
        self.check(store+' MCP retry identifies original commit', repeated.get('mutation_receipt') == dict(model_receipt, replayed=True))
        observed = self.good(store+' model creation read', self.call('get', dict(context, id=model_receipt['version']['record_id'])))
        self.check(store+' keyed MCP creation retains confidence ceiling', observed.get('memory', {}).get('confidence') == 0.8)
        # Human-created parent still requires a review proposal on model store.
        args.update(key=key, content='model replacement proposal', idempotency_key=key+'-proposal')
        proposal = self.mcp_document(store+' creation proposal', 'mutate', args)
        again = self.mcp_document(store+' creation proposal replay', 'mutate', args)
        self.check(store+' creation retry preserves review admission', proposal.get('kind') == 'review_required' and
            bool(proposal.get('proposal', {}).get('proposal_id')) and
            again.get('proposal', {}).get('proposal_id') == proposal.get('proposal', {}).get('proposal_id') and
            again.get('proposal', {}).get('replayed') is True)
        self.good(store+' keyed creation retirement', self.call('delete', dict(context, id=row['id'])))
        code, unavailable = self.call('store', request)
        self.check(store+' retired creation cannot be recreated by retry', code >= 400 and
            unavailable.get('reason') == 'idempotent_result_unavailable' and unavailable.get('mutation_receipt') is None)

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
        self.personal_versions()
        self.personal_correction_review()
        self.personal_keyed_corrections()
        self.personal_keyed_retirement()
        self.keyed_creation('user')
        return all(c['passed'] for c in self.checks)

    def run(self):
        self.hygiene_preview()
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
            code, body = self.call('hygiene', dict(dry_run=True, scope=dict(type='project', value=self.prefix)))
            self.check('hygiene KB outage cannot report a clean scan', code >= 500
                and body.get('kind') == 'unavailable' and 'findings' not in body, [code, body])
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
        self.derived_parent_eligibility()
        self.shared_mcp_corrections()
        journal_id, journal = self.shared_journal()
        self.shared_keyed_deletion()
        self.keyed_creation('kb')
        self.docker('restart', self.args.kb)
        self.good('shared replacement survives KB restart', self.wait('get', dict(store='kb', id=journal_id)))
        self.shared_deletion_after_restart()
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
        self.protected_recall()
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
