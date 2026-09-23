#!/usr/bin/env python3
"""Fresh, isolated Docker release topologies using the shipped compositions.

T1: shared KB. T2: standalone Server enrolled into a separately deployed KB.
T3: KB-free Server. Images must already exist locally. All created containers,
networks and volumes belong to a random project and are removed unless --keep.
Credentials remain in memory and test output contains verdicts only.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import secrets
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]


def command(*argv, env=None, data=None, timeout=300):
    result = subprocess.run(argv, cwd=ROOT, env=env, input=data, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    if result.returncode:
        # Never echo input, environment, command arguments or container logs:
        # an enrollment token or bootstrap password may be present there.
        categories = ('no space left on device', 'port is already allocated',
                      'permission denied', 'no such image', 'failed to mount',
                      'invalid mount', 'invalid spec', 'address already in use',
                      'not found', 'unhealthy', 'operation not permitted')
        reason = next((value for value in categories if value in result.stderr.lower()), 'unclassified')
        # Command shape and fixed categories reveal the failing phase without
        # emitting potentially credential-bearing arguments or Docker output.
        operation = next((value for value in argv[1:]
                          if value in ('up', 'config', 'inspect', 'exec', 'run', 'restart', 'down')), '')
        raise RuntimeError(f'{Path(argv[0]).name} {operation} failed with exit {result.returncode} ({reason})')
    return result.stdout.strip()


class Stack:
    def __init__(self, role, env, output):
        self.role = role
        self.project = 'aimee-e2e-' + role + '-' + uuid.uuid4().hex[:10]
        self.env = dict(env)
        self.env['AIMEE_KB_API_BEARER_TOKEN'] = 'scope:service:aimee-server:' + secrets.token_hex(32)
        self.service_identity = 'scope:service:aimee-server:' + secrets.token_hex(32)
        self.env['AIMEE_KB_HOST'] = 'aimee-kb'
        for kind in ('ADMIN', 'MIGRATOR', 'RUNTIME'):
            self.env[f'AIMEE_STORE_{kind}_PASSWORD'] = secrets.token_hex(24)
        self.file = 'compose.kb.yaml' if role == 'kb' else 'compose.yaml'
        self.application = f'{self.project}-aimee-{role}-1'
        self.postgres = f'{self.project}-aimee-store-db-1'
        self.embedder = f'{self.project}-aimee-embedder-1'
        self.output = output
        # Tests reach applications over their private network or container
        # loopback. Do not bind host sockets: multiple ephemeral port-zero
        # mappings can collide on newer Docker daemons, especially mixed
        # loopback and wildcard bindings. Keep the production service intact.
        self.network_override = output.resolve() / (self.project + '-network.yaml')
        self.network_override.write_text(
            'services:\n  aimee-' + role + ':\n    ports: !reset []\n')

    def compose_args(self):
        encryption = ('-f', 'compose.kb.luks.yaml' if self.role == 'kb' else 'compose.luks.yaml') if self.env.get('AIMEE_POSTGRES_STORAGE') == 'luks' else ()
        return ('--env-file', '/dev/null', '-p', self.project, '-f', self.file,
                *encryption, '-f', str(self.network_override))

    def compose(self, *args):
        return command('docker', 'compose', *self.compose_args(), *args, env=self.env)

    def start(self):
        command('python3', str(ROOT / 'scripts/compose-vault-init.py'),
                *self.compose_args(), 'up', env=self.env)
        self.compose('up', '-d', '--no-build', '--pull', 'never')
        bindings = json.loads(command('docker', 'inspect', '--format',
                                      '{{json .HostConfig.PortBindings}}', self.application))
        if bindings:
            raise RuntimeError('isolated topology unexpectedly published a host port')
        entries = json.loads(command('docker', 'inspect', '--format', '{{json .Config.Env}}', self.application))
        actual = next((entry.split('=', 1)[1] for entry in entries
                       if entry.startswith('AIMEE_PROVIDER_CONTEXT_LIMITS=')), '')
        if not actual or json.loads(actual) != json.loads(self.env['AIMEE_PROVIDER_CONTEXT_LIMITS']):
            raise RuntimeError('candidate application did not receive the provider byte ceiling')
        host = json.loads(command('docker', 'inspect', '--format', '{{json .HostConfig}}', self.postgres))
        if self.env.get('AIMEE_POSTGRES_STORAGE', 'plain') == 'plain':
            if any(host.get(key) for key in ('Privileged', 'CapAdd', 'Devices', 'DeviceCgroupRules')):
                raise RuntimeError('ordinary PostgreSQL unexpectedly requires extra host privileges or devices')
        deadline = time.monotonic() + 240
        while time.monotonic() < deadline:
            status = command('docker', 'inspect', '--format', '{{.State.Health.Status}}', self.application)
            if status == 'healthy':
                return
            time.sleep(2)
        raise RuntimeError(self.role + ' did not become healthy')

    def kb_request(self, path, body=None, authenticated=True):
        code = '''import http.client,json,sys
a=json.load(sys.stdin); c=http.client.HTTPConnection('127.0.0.1',8741,timeout=70)
h={'Content-Type':'application/json'}
if a['token']: h['Authorization']='Bearer '+a['token']
c.request('GET' if a['body'] is None else 'POST',a['path'],None if a['body'] is None else (a['body'] if isinstance(a['body'],str) else json.dumps(a['body'])),h)
r=c.getresponse(); print(json.dumps([r.status,json.loads(r.read())]))
'''
        return json.loads(command('docker', 'exec', '-i', self.application, 'python3', '-c', code,
            data=json.dumps(dict(path=path, body=body,
                token=self.env['AIMEE_KB_API_BEARER_TOKEN'] if authenticated else ''))))


def application_metadata_is_private(stack):
    entries = json.loads(command('docker', 'inspect', '--format', '{{json .Config.Env}}', stack.application))
    names = [entry.partition('=')[0] for entry in entries]
    forbidden = {'AIMEE_STORE_URL', 'AIMEE_STORE_MIGRATION_URL', 'AIMEE_DB2_URL', 'AIMEE_KB_CONN'}
    return not any(name in forbidden or name.endswith(('_PASSWORD', '_API_KEY', '_TOKEN', '_PRIVATE_KEY')) for name in names)


def preview_source_version_gate(kb, check):
    """Exact canonical and summary versions through the authenticated Go owner."""
    key = 'preview-source-' + uuid.uuid4().hex
    summary_id = 9007199254740993 + uuid.uuid4().int % 1000000000
    def sql(query):
        return command('docker', 'exec', kb.postgres, 'psql', '-U', 'postgres',
                       '-d', 'aimee_store', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-c', query)
    fixture = json.loads(sql(f"""BEGIN;
        INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES
          ('L2','fact','{key}-headline','canonical content','project','{key}'),
          ('L2','fact','{key}-fallback','fallback content','project','{key}');
        INSERT INTO memory_summaries(id,memory_id,scope,summary)
          SELECT {summary_id},id,'headline','exact headline' FROM memories WHERE key='{key}-headline';
        SELECT json_build_object('owner_id',(SELECT owner_id::text FROM memory_collection_owner WHERE id=1),
          'parent_id',id::text,'parent_revision',record_revision::text) FROM memories WHERE key='{key}-headline';
        COMMIT"""))
    def previews():
        return kb.kb_request('/v1/actions/memory.diagnose_scoped',
                             dict(query=key, project=key, scope_context=True, format='ingress', limit=5))
    def revalidate(refs, project=key):
        return kb.kb_request('/v1/actions/memory.revalidate_sources',
            dict(scope_context=True, project=project, include_all=False,
                 revalidation=dict(schema_version=1, check_id=uuid.uuid4().hex, sources=refs)))
    def matches(result):
        p = result.get('memory_projection', {})
        rows = result.get('memories', [])
        rendered = ''.join(f"  - memory:{r['id']} {r['key']} [{r['tier']}/{r['kind']} score={r['score_text']} headline_missing={str(not bool(r['headline'])).lower()}]\n"
                           f"    > {r['headline'] or r['content']}\n" for r in rows)
        identity = dict(schema_version=1, projection_digest=p.get('projection_digest'), retained_items=p.get('retained_items'))
        digest = hashlib.sha256(json.dumps(identity, ensure_ascii=False, separators=(',', ':')).encode()).hexdigest()
        return p.get('projection_digest') == 'sha256:' + hashlib.sha256(rendered.encode()).hexdigest() and p.get('rendered_bytes') == len(rendered.encode()) and p.get('selection_digest') == 'sha256:' + digest
    try:
        code, before = previews()
        refs = before.get('memory_projection', {}).get('retained_items', [])
        summary = next((r.get('source_version', {}) for r in refs if r.get('stable_id') == str(summary_id)), {})
        check('Preview commits exact rendered bytes and selected source versions', code == 200 and len(refs) == 2 and matches(before))
        check('Preview preserves large summary ID and canonical parent revision', summary.get('record_kind') == 'memory_summary' and
              summary.get('version') == dict(schema_version=1, owner_id=fixture['owner_id'], record_id=str(summary_id), record_revision='1') and
              summary.get('memory_parents') == [dict(schema_version=1, owner_id=fixture['owner_id'], record_id=fixture['parent_id'], record_revision=fixture['parent_revision'])])
        check('Preview fallback binds its canonical source', any(r.get('source_version', {}).get('record_kind') == 'memory_record' for r in refs))
        code, result = revalidate(refs)
        check('Preview final source check accepts current sources', code == 200 and result.get('eligible') is True)
        code, result = revalidate(refs, key+'-hidden')
        check('Preview final source check refuses hidden sources', code == 200 and result.get('eligible') is False)
        sql(f"UPDATE memory_summaries SET summary=summary,record_revision=999 WHERE id={summary_id}")
        code, result = revalidate(refs)
        check('Preview no-op refresh cannot forge a source revision', code == 200 and result.get('eligible') is True)
        sql(f"UPDATE memory_summaries SET summary='revised headline' WHERE id={summary_id}")
        code, result = revalidate(refs)
        check('Preview final source check refuses independently edited summary', code == 200 and result.get('eligible') is False)
        code, after = previews()
        fresh_refs = after.get('memory_projection', {}).get('retained_items', [])
        fresh_summary = next((r.get('source_version', {}) for r in fresh_refs if r.get('stable_id') == str(summary_id)), {})
        check('Preview refresh binds revised summary without inventing parent change', code == 200 and matches(after) and
              fresh_summary.get('version', {}).get('record_revision') == '2' and fresh_summary.get('memory_parents') == summary.get('memory_parents'))
        code, result = revalidate(fresh_refs)
        check('Preview final source check accepts refreshed summary', code == 200 and result.get('eligible') is True)
        sql(f"UPDATE memories SET content='revised fallback' WHERE key='{key}-fallback'")
        code, result = revalidate(fresh_refs)
        check('Preview final source check refuses changed canonical fallback', code == 200 and result.get('eligible') is False)
    finally:
        sql(f"DELETE FROM memories WHERE key IN ('{key}-headline','{key}-fallback')")


def linked_relation_input_gate(kb, check):
    """Verify copied-input and link fences through authenticated graph endpoints."""
    key = 'linked-input-' + uuid.uuid4().hex
    def sql(query):
        return command('docker', 'exec', kb.postgres, 'psql', '-U', 'postgres',
                       '-d', 'aimee_store', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-c', query)
    fixture = json.loads(sql(f"""BEGIN;
        INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES
          ('L2','fact','{key}-parent','parent','project','{key}'),
          ('L2','fact','{key}-target','linked source','project','{key}');
        INSERT INTO memory_links(source_id,target_id,relation)
          SELECT p.id,t.id,'related_to' FROM memories p,memories t
          WHERE p.key='{key}-parent' AND t.key='{key}-target';
        INSERT INTO memory_entities(memory_id,entity)
          SELECT id,'{key}' FROM memories WHERE key='{key}-parent';
        INSERT INTO memory_relations(memory_id,src_entity,relation,dst_entity,fact_text)
          SELECT id,'{key}','related_to','linked-target','{key} copied linked source'
          FROM memories WHERE key='{key}-parent';
        INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
          SELECT 'relation',r.id,'memory-index-v1',r.memory_id::text FROM memory_relations r
          WHERE r.src_entity='{key}';
        INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
          SELECT 'relation',r.id,'memory-relation-input-v2',
            (jsonb_build_object('record_id',m.id::text,'record_revision',m.record_revision::text) ||
             CASE WHEN m.key='{key}-target' THEN jsonb_build_object('link_id',l.id::text) ELSE '{{}}'::jsonb END)::text
          FROM memory_relations r JOIN memory_links l ON l.source_id=r.memory_id
          JOIN memories m ON m.id IN(l.source_id,l.target_id) WHERE r.src_entity='{key}';
        UPDATE kb_async_jobs SET status='done' WHERE kind='memory_index' AND document_id IN
          (SELECT id FROM memories WHERE key IN ('{key}-parent','{key}-target'));
        SELECT json_build_object('relation_id',r.id,'target_id',l.target_id,'link_id',l.id)
          FROM memory_relations r JOIN memory_links l ON l.source_id=r.memory_id WHERE r.src_entity='{key}';
        COMMIT"""))
    def reads(label, expected):
        for verb, field in [('search_graph','relations'), ('entity_edges','edges'), ('entity_profile','profile')]:
            code, result = kb.kb_request('/v1/actions/memory.'+verb,
                dict(query=key+' copied linked source', entity=key, project=key, scope_context=True, limit=1))
            value = result.get(field, {} if field == 'profile' else [])
            count = value.get('relation_count', -1) if field == 'profile' else len(value)
            check('Linked input '+label+' on '+verb, code == 200 and count == expected)
    try:
        reads('is initially visible', 1)
        sql(f"UPDATE memories SET valid_until='2000-01-01' WHERE id={fixture['target_id']}")
        reads('expiry withholds copied text', 0)
        sql(f"UPDATE memories SET valid_until='' WHERE id={fixture['target_id']}")
        reads('restoration cannot reuse old observation', 0)
        sql(f"""UPDATE memory_lineage SET source_ref=jsonb_set(source_ref::jsonb,'{{record_revision}}',
            to_jsonb((SELECT record_revision::text FROM memories WHERE id={fixture['target_id']})))::text
            WHERE object_type='relation' AND object_id={fixture['relation_id']}
            AND source_kind='memory-relation-input-v2' AND source_ref::jsonb->>'record_id'='{fixture['target_id']}'""")
        reads('fresh observed input is visible', 1)
        sql(f"DELETE FROM memory_links WHERE id={fixture['link_id']}")
        reads('deleted link withholds copied relationship', 0)
    finally:
        sql(f"""DELETE FROM memory_lineage WHERE object_type='relation' AND object_id={fixture['relation_id']};
            DELETE FROM memories WHERE key IN ('{key}-parent','{key}-target')""")


def typed_source_version_gate(kb, check):
    """Observe real assertion versions through the authenticated KB/Go path."""
    key = 'typed-version-' + uuid.uuid4().hex
    def sql(query):
        return command('docker', 'exec', kb.postgres, 'psql', '-U', 'postgres',
                       '-d', 'aimee_store', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-c', query)
    fixture = json.loads(sql(f"""BEGIN;
        INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status)
        VALUES('{key}','assert','test:typed-version','system',100,'open');
        INSERT INTO entity_edges(source,relation,target,edge_class,assertion_kind,lifecycle_state,
          confidence_class,confidence,authority_rank,commit_id)
        VALUES('{key}','naming_convention','fixture','semantic','world_fact','persistent','A',.9,80,'{key}');
        INSERT INTO fact_graph_changes(commit_id,assertion_id,action,existed_before,existed_after,
          after_lifecycle,after_confidence,after_authority_rank,after_version)
        SELECT '{key}',id,'assert',0,1,lifecycle_state,confidence,authority_rank,version
        FROM entity_edges WHERE commit_id='{key}';
        WITH parent AS (INSERT INTO memories(tier,kind,key,content,scope_type,scope_value)
          VALUES('L2','fact','{key}-parent','supporting source','project','{key}') RETURNING id)
        INSERT INTO fact_evidence(assertion_id,source_kind,source_id,stance)
          SELECT e.id,'memory','memory:'||p.id::text,'supports' FROM entity_edges e,parent p WHERE e.commit_id='{key}';
        SELECT json_build_object('owner_id',(SELECT owner_id::text FROM memory_collection_owner WHERE id=1),
          'record_id',id::text,'record_revision',version::text,
          'parent_id',(SELECT id::text FROM memories WHERE key='{key}-parent'),
          'parent_revision',(SELECT record_revision::text FROM memories WHERE key='{key}-parent'))
          FROM entity_edges WHERE commit_id='{key}';
        COMMIT"""))
    payload = dict(query=key, project=key, scope_context=True,
                   enable_observations=False, enable_approved_procedures=False)
    def call(extra=None):
        return kb.kb_request('/v1/actions/memory.assemble_typed_context', dict(payload, **(extra or {})))
    def selected(result):
        return next((r for r in result.get('retained_items', []) if r['stable_id'] == fixture['record_id']), {})
    def selected_text(result):
        return next((r.get('rendered') for r in result.get('channels', {}).get('current_assertions', {}).get('items', [])
                     if r.get('stable_id') == fixture['record_id']), None)
    def revalidate(ref, project=key, authenticated=True):
        request = dict(scope_context=True, project=project, include_all=False,
                       revalidation=dict(schema_version=1, check_id=uuid.uuid4().hex, sources=[ref]))
        return kb.kb_request('/v1/actions/memory.revalidate_sources', request, authenticated=authenticated)
    def matches(result):
        identity = dict(schema_version=1, projection_digest=result.get('projection_digest'),
                        retained_items=result.get('retained_items'))
        raw = json.dumps(identity, ensure_ascii=False, separators=(',', ':')).encode()
        return result.get('selection_digest') == 'sha256:' + hashlib.sha256(raw).hexdigest()
    try:
        code, before = call()
        source = selected(before).get('source_version', {})
        expected = dict(schema_version=1, **{k:fixture[k] for k in ('owner_id','record_id','record_revision')})
        check('Typed assertion binds the exact owner record revision', code == 200 and
              source.get('record_kind') == 'semantic_assertion' and source.get('version') == expected and
              before.get('source_version_state') == 'record_versions_observed')
        check('Typed selection digest includes source revision evidence', matches(before))
        expected_parent = dict(schema_version=1, owner_id=fixture['owner_id'],
                               record_id=fixture['parent_id'], record_revision=fixture['parent_revision'])
        check('Typed assertion binds its direct memory parent revision',
              source.get('memory_parents') == [expected_parent] and source.get('memory_parent_state') == 'observed')
        requirements = dict(schema_version=1, task_revision='deployment-fixture:1', query_mode='current_state',
                            obligations=[dict(subject=key, relation='naming_convention')])
        code, covered = call(dict(evidence_requirements=requirements))
        coverage = covered.get('evidence_coverage', {})
        roles = coverage.get('roles', [])
        check('Typed coverage evaluates explicit current-state roles over retained evidence', code == 200 and
              coverage.get('status') == 'complete' and len(roles) == 1 and
              roles[0].get('retained_ids') == [fixture['record_id']] and
              coverage.get('selection_digest') == covered.get('selection_digest') and
              coverage.get('release_state') == 'not_revalidated')
        code, dropped = call(dict(evidence_requirements=requirements,
                                  context_limits=dict(schema_version=1, max_context_bytes=0)))
        roles = dropped.get('evidence_coverage', {}).get('roles', [])
        check('Typed coverage marks a packed-away role budget_dropped', code == 200 and
              dropped.get('context_sufficiency') == 'insufficient' and len(roles) == 1 and
              roles[0].get('status') == 'budget_dropped' and roles[0].get('retained_ids') == [])
        unrelated = dict(requirements, obligations=[dict(subject=key+'-unrelated', relation='naming_convention')])
        code, absent = call(dict(evidence_requirements=unrelated))
        check('Unrelated retrieved evidence cannot fill a current-state obligation', code == 200 and
              absent.get('context_sufficiency') == 'insufficient')
        recovery_budget = dict(max_rounds=1, max_new_items=1, max_tokens=256,
                               max_elapsed_ms=200, max_cost_microunits=0)
        code, proposed = call(dict(evidence_requirements=dict(unrelated, recovery_budget=recovery_budget)))
        plan = proposed.get('evidence_recovery', {})
        check('Missing role recovery remains a bounded host-admitted proposal', code == 200 and
              proposed.get('context_sufficiency') == 'insufficient' and
              plan.get('authority') == 'proposal_only' and plan.get('state') == 'awaiting_host_admission' and
              plan.get('proposed_budget') == recovery_budget and len(plan.get('actions', [])) == 1 and
              plan.get('selection_digest') == proposed.get('selection_digest'))
        code, blocked = call(dict(evidence_requirements=dict(requirements, recovery_budget=recovery_budget),
                                  context_limits=dict(schema_version=1, max_context_bytes=0)))
        plan = blocked.get('evidence_recovery', {})
        check('Recovery cannot retry evidence removed by packing', code == 200 and
              plan.get('state') == 'blocked' and plan.get('actions') == [] and
              any(gap.get('reason') == 'packing_budget_requires_host_revision' for gap in plan.get('remaining_gaps', [])))
        code, unknown = call(dict(evidence_requirements=dict(requirements, query_mode='timeline')))
        check('Unsupported evidence query mode remains unknown', code == 200 and unknown.get('context_sufficiency') == 'unknown')
        code, hidden_coverage = call(dict(evidence_requirements=requirements, project=key+'-hidden'))
        check('Hidden assertion cannot establish evidence coverage', code == 200 and
              hidden_coverage.get('context_sufficiency') != 'complete' and
              all(not role.get('retained_ids') for role in hidden_coverage.get('evidence_coverage', {}).get('roles', [])))
        code, again = call()
        # Other visible candidates and their retrieval traces can change while
        # background indexing progresses. This check concerns the exact fixture
        # source; whole-projection stability requires identical whole inputs.
        check('Unchanged typed assertion retains its source binding', code == 200 and
              selected(again) == selected(before) and matches(again))
        code, verified = revalidate(selected(before))
        check('Final source check accepts unchanged assertion and parent', code == 200 and
              verified.get('status') == 'ok' and verified.get('eligible') is True)
        code, hidden = revalidate(selected(before), project=key+'-hidden')
        check('Final source check refuses a newly hidden parent', code == 200 and hidden.get('eligible') is False)
        code, anonymous = revalidate(selected(before), authenticated=False)
        check('Final source check requires authenticated transport', code in (401, 403) and anonymous.get('eligible') is not True)
        sql(f"UPDATE memories SET content='changed supporting source' WHERE id={int(fixture['parent_id'])}")
        code, parent_changed = call()
        expected_parent['record_revision'] = str(int(fixture['parent_revision'])+1)
        check('Changed memory parent invalidates binding with identical assertion bytes', code == 200 and
              selected(parent_changed).get('source_version', {}).get('memory_parents') == [expected_parent] and
              selected_text(before) is not None and selected_text(parent_changed) == selected_text(before) and
              parent_changed.get('selection_digest') != before.get('selection_digest') and matches(parent_changed))
        code, stale = revalidate(selected(before))
        check('Final source check refuses parent changed after selection', code == 200 and stale.get('eligible') is False)
        code, current = revalidate(selected(parent_changed))
        check('Final source check accepts refreshed parent binding', code == 200 and current.get('eligible') is True)
        mid = int(fixture['record_id'])
        sql(f"""BEGIN; UPDATE entity_edges SET version=version+1 WHERE id={mid};
            INSERT INTO fact_graph_changes(commit_id,assertion_id,action,existed_before,existed_after,
              before_version,after_version,after_lifecycle,after_confidence,after_authority_rank)
            SELECT '{key}',id,'revise',1,1,version-1,version,lifecycle_state,confidence,authority_rank
            FROM entity_edges WHERE id={mid}; COMMIT""")
        code, changed = call()
        new_source = selected(changed).get('source_version', {}).get('version', {})
        check('Changed assertion revision cannot reuse the prior selection binding', code == 200 and
              new_source.get('record_revision') == str(int(fixture['record_revision'])+1) and
              new_source.get('owner_id') == fixture['owner_id'] and matches(changed) and
              changed.get('selection_digest') != before.get('selection_digest'))
        code, empty = call(dict(context_limits=dict(schema_version=1, max_context_bytes=0)))
        check('Omitted typed assertion claims no retained source revision', code == 200 and
              empty.get('retained_items') == [] and empty.get('source_version_state') == 'unavailable')
        # The legacy plain-text channel keeps the same wording, but now carries
        # source versions alongside it for assembly and later release decisions.
        def plain_facts():
            return kb.kb_request('/v1/actions/memory.facts', dict(query=key, project=key, scope_context=True))
        code, facts = plain_facts()
        fact_projection = facts.get('fact_projection', {})
        fact_source = selected(fact_projection).get('source_version', {})
        check('Plain facts bind exact assertion and parent revisions', code == 200 and
              fact_source.get('record_kind') == 'semantic_assertion' and
              fact_source.get('version') == new_source and fact_source.get('memory_parents') == [expected_parent])
        check('Plain facts digest binds rendered bytes and source selection', matches(fact_projection) and
              fact_projection.get('projection_digest') == 'sha256:' + hashlib.sha256(facts.get('facts', '').encode()).hexdigest() and
              fact_projection.get('rendered_bytes') == len(facts.get('facts', '').encode()))
        code, facts_again = plain_facts()
        check('Unchanged plain facts preserve selection identity', code == 200 and
              facts_again.get('fact_projection') == fact_projection)
        sql(f"UPDATE memories SET content='plain fact supporting source changed' WHERE id={int(fixture['parent_id'])}")
        code, facts_parent_changed = plain_facts()
        expected_parent['record_revision'] = str(int(expected_parent['record_revision'])+1)
        changed_facts_projection = facts_parent_changed.get('fact_projection', {})
        check('Parent revision changes plain fact binding with identical text', code == 200 and
              facts_parent_changed.get('facts') == facts.get('facts') and
              selected(changed_facts_projection).get('source_version', {}).get('memory_parents') == [expected_parent] and
              changed_facts_projection.get('selection_digest') != fact_projection.get('selection_digest') and matches(changed_facts_projection))
        # This authored episode has a different key from the parent, so indexing
        # cannot replace it with the generated parent-key episode.
        episode_id = sql(f"""INSERT INTO memory_episodes(memory_id,episode_key,episode_text,source_session)
            VALUES({int(fixture['parent_id'])},'{key}-episode','{key} episode','fixture') RETURNING id::text""").strip()
        payload.update(query=key+'-episode', enable_semantic_assertions=False, enable_episodes=True)
        def episode_source(result):
            return next((r.get('source_version', {}) for r in result.get('retained_items', [])
                         if r.get('channel') == 'episodes' and r.get('stable_id') == episode_id), {})
        code, episode = call()
        expected_episode = dict(schema_version=1, owner_id=fixture['owner_id'],
                                record_id=episode_id, record_revision='1')
        check('Typed episode binds its own and parent revisions', code == 200 and
              episode_source(episode) == dict(record_kind='memory_episode', version=expected_episode,
                  memory_parents=[expected_parent], memory_parent_state='observed') and matches(episode))
        sql(f"UPDATE memory_episodes SET episode_text=episode_text,record_revision=900 WHERE id={int(episode_id)}")
        code, episode_noop = call()
        check('Typed episode no-op preserves revision and selection', code == 200 and
              episode_noop.get('selection_digest') == episode.get('selection_digest'))
        sql(f"UPDATE memory_episodes SET source_session='edited provenance' WHERE id={int(episode_id)}")
        code, episode_edit = call()
        expected_episode['record_revision'] = '2'
        check('Episode provenance changes independently of its parent', code == 200 and
              episode_source(episode_edit).get('version') == expected_episode and
              episode_source(episode_edit).get('memory_parents') == [expected_parent] and
              episode_edit.get('selection_digest') != episode.get('selection_digest') and matches(episode_edit))
        sql(f"UPDATE memories SET content='episode supporting source changed' WHERE id={int(fixture['parent_id'])}")
        code, episode_parent_edit = call()
        expected_parent['record_revision'] = str(int(expected_parent['record_revision'])+1)
        check('Parent revision binds identical rendered episode bytes', code == 200 and
              episode_source(episode_parent_edit).get('version') == expected_episode and
              episode_source(episode_parent_edit).get('memory_parents') == [expected_parent] and
              episode_parent_edit.get('rendered_context') == episode_edit.get('rendered_context') and
              episode_parent_edit.get('selection_digest') != episode_edit.get('selection_digest') and matches(episode_parent_edit))
        code, episode_empty = call(dict(context_limits=dict(schema_version=1, max_context_bytes=0)))
        check('Omitted typed episode claims no retained source revision', code == 200 and
              episode_empty.get('retained_items') == [] and episode_empty.get('source_version_state') == 'unavailable')
    finally:
        sql(f"""BEGIN; UPDATE entity_edges SET lifecycle_state='invalidated',version=version+1 WHERE commit_id='{key}';
            INSERT INTO fact_graph_changes(commit_id,assertion_id,action,existed_before,existed_after,
              before_version,after_version,after_lifecycle,after_confidence,after_authority_rank)
            SELECT '{key}',id,'retire',1,1,version-1,version,lifecycle_state,confidence,authority_rank
            FROM entity_edges WHERE commit_id='{key}';
            UPDATE memories SET lifecycle_state='retired' WHERE key='{key}-parent'; COMMIT""")


def typed_context_budget_gate(kb, check):
    """Exact projection bytes through the authenticated KB action and Go owner."""
    payload = dict(query='typed-budget-fixture', project='typed-budget-' + uuid.uuid4().hex,
        scope_context=True, enable_semantic_assertions=False, enable_observations=False,
        enable_approved_procedures=False, enable_working_context=True,
        recent_turns=['small constraint LIMIT_7', '界"\\\n' * 80])
    def call(limits=None):
        body = dict(payload)
        if limits is not None:
            body['context_limits'] = limits
        return kb.kb_request('/v1/actions/memory.assemble_typed_context', body)
    code, baseline = call()
    check('Typed projection retains both untrusted caller turns', code == 200 and
          baseline.get('status') == 'ok' and len(baseline.get('retained_items', [])) == 2)
    def selection_matches(result):
        identity = dict(schema_version=1, projection_digest=result.get('projection_digest'),
                        retained_items=result.get('retained_items'))
        encoded = json.dumps(identity, ensure_ascii=False, separators=(',', ':')).encode()
        return result.get('selection_digest') == 'sha256:' + hashlib.sha256(encoded).hexdigest()
    check('Typed projection identity binds rendered bytes and retained IDs', selection_matches(baseline))
    exact = len(baseline['rendered_context'].encode())
    for limit in (0, 1, 400, exact - 1, exact):
        code, result = call(dict(schema_version=1, max_context_bytes=limit))
        rendered = result.get('rendered_context', '').encode()
        a = result.get('context_accounting', {})
        check('Typed projection obeys byte limit ' + str(limit), code == 200 and
              result.get('status') == 'ok' and len(rendered) <= limit and
              a.get('max_context_bytes') == limit and a.get('rendered_bytes') == len(rendered))
        check('Typed projection binds exact accounting at limit ' + str(limit),
              a.get('boundary') == 'typed_memory_projection' and a.get('count_state') == 'exact' and
              a.get('unit') == 'utf8_bytes' and a.get('token_count_state') == 'unavailable' and
              a.get('digest') == 'sha256:' + hashlib.sha256(rendered).hexdigest())
        check('Typed projection binds retained IDs at limit ' + str(limit), selection_matches(result))
        if limit == 0:
            check('Zero byte typed projection emits no wrappers or retained IDs',
                  rendered == b'' and result.get('retained_items') == [] and
                  result.get('context_sufficiency') == 'insufficient')
        if limit == 400:
            partial_projection = result
            check('Typed byte packing preserves the earlier small item',
                  [r['stable_id'] for r in result['retained_items']] == ['turn:0'] and
                  b'LIMIT_7' in rendered)
        if limit == exact:
            check('Typed projection retains exact-fit serialized bytes',
                  result['rendered_context'] == baseline['rendered_context'])
    for field in ('max_context_tokens', 'max_request_tokens', 'reserved_response_tokens', 'reserved_tool_tokens'):
        code, result = call(dict(schema_version=1, **{field:0}))
        check('Typed projection refuses unavailable ' + field,
              code == 200 and result.get('kind') == 'unsupported_mode')

    malformed = {
        'null limits': 'null',
        'null byte cap': '{"schema_version":1,"max_context_bytes":null}',
        'null token cap': '{"schema_version":1,"max_context_tokens":null}',
        'duplicate byte cap': '{"schema_version":1,"max_context_bytes":0,"max_context_bytes":700}',
        'escaped duplicate cap': r'{"schema_version":1,"max_context_bytes":0,"max_context_byt\u0065s":700}',
        'case alias': '{"schema_version":1,"MAX_CONTEXT_BYTES":0}',
    }
    for name, limits in malformed.items():
        # Raw JSON preserves duplicates across the actual native HTTP adapter.
        raw = json.dumps(payload)[:-1] + ',"context_limits":' + limits + '}'
        code, result = kb.kb_request('/v1/actions/memory.assemble_typed_context', raw)
        check('Typed projection rejects ' + name + ' without serving context',
              (code >= 400 or result.get('status') == 'error') and
              not result.get('rendered_context') and not result.get('retained_items'))
    code, after = call()
    check('Typed projection remains usable after malformed limit refusals',
          code == 200 and after.get('status') == 'ok' and
          after.get('rendered_context') == baseline['rendered_context'] and
          after.get('selection_digest') == baseline['selection_digest'])


    # The native C-host/Go test covers actual integrity-gated emission. Here the
    # real authenticated writer/reader persists the projection-reference contract.
    turn = 'typed-evidence-' + uuid.uuid4().hex
    def refs_for(projection):
        return [dict(type='memory_projection_item',
                     ref='typed:v1:' + projection['selection_digest'] + ':' + r['channel'] + ':' + r['stable_id'])
                for r in projection['retained_items']]
    def merge(turn_id, refs):
        return kb.kb_request('/v1/actions/evidence.merge_retrieval_event',
                            dict(turn_id=turn_id, role='Recall', query_fingerprint='typed-fixture', surfaced_refs=refs))
    def trace(turn_id):
        return kb.kb_request('/v1/actions/evidence.trace_retrieval_event', dict(turn_id=turn_id))
    refs = refs_for(baseline)
    code, first = merge(turn, refs)
    check('Typed assembly references commit through authenticated evidence writer',
          code == 200 and first.get('status') == 'ok' and bool(first.get('retrieval_event_id')))
    code, found = trace(turn)
    check('Typed evidence preserves exact references without invented source versions',
          code == 200 and found.get('trace_status') == 'ok' and found.get('event', {}).get('surfaced_refs') == refs)
    code, again = merge(turn, refs)
    check('Repeated typed evidence merge preserves event identity', code == 200 and
          again.get('retrieval_event_id') == first['retrieval_event_id'])
    code, found = trace(turn)
    check('Repeated typed evidence merge does not duplicate references',
          code == 200 and found.get('event', {}).get('surfaced_refs') == refs)
    partial_refs = refs_for(partial_projection)
    code, changed = merge(turn, partial_refs)
    check('Repacked typed selection joins the same turn event', code == 200 and
          changed.get('retrieval_event_id') == first['retrieval_event_id'])
    code, found = trace(turn)
    check('Distinct accepted projection identities remain distinguishable', code == 200 and
          found.get('event', {}).get('surfaced_refs') == refs + partial_refs)
    code, empty = merge(turn + '-empty', [])
    check('Empty typed evidence does not create a bare event', code == 200 and
          empty.get('status') == 'ok' and empty.get('retrieval_event_id') == '')
    code, found = trace(turn + '-empty')
    check('Empty typed evidence trace remains explicitly unavailable',
          code == 200 and found.get('trace_status') == 'evidence_unavailable')
    large_refs = [dict(type='memory_projection_item', ref=refs[0]['ref'] + ':fixture:' + str(i)) for i in range(100)]
    code, large = merge(turn + '-large', large_refs)
    check('Large typed evidence writer accepts the reference batch', code == 200 and
          large.get('status') == 'ok' and bool(large.get('retrieval_event_id')))
    code, found = trace(turn + '-large')
    check('Bounded trace refuses a truncated event instead of reporting success', code == 200 and
          found.get('trace_status') == 'evidence_unavailable' and 'event' not in found and 'event_raw' not in found)
    code, replay = merge(turn + '-large', large_refs)
    check('Bounded-read refusal preserves subsequent event merge retries', code == 200 and
          replay.get('retrieval_event_id') == large['retrieval_event_id'])


def evidence_exact_identity_gate(kb, check):
    """Exact identity through authenticated evidence writers and public readers."""
    turn = 'exact-evidence-' + uuid.uuid4().hex
    expected = [42, '9007199254740992', '9007199254740993', '9223372036854775807']
    def action(name, payload):
        return kb.kb_request('/v1/actions/' + name, payload)
    code, written = action('evidence.emit_retrieval_event',
        dict(turn_id=turn, role='Recall', surfaced_ids=expected))
    check('Evidence writer accepts exact int64 source identities',
          code == 200 and written.get('status') == 'ok' and bool(written.get('retrieval_event_id')))
    code, trace = action('evidence.trace_retrieval_event', dict(turn_id=turn))
    event = trace.get('event', {})
    check('Evidence trace preserves adjacent large IDs and int64 maximum', code == 200 and
          event.get('surfaced_ids') == expected and
          [r.get('id') for r in event.get('surfaced_refs', [])] == expected and
          [r.get('id') for r in event.get('surfaced_items', [])] == expected)
    code, provenance = action('evidence.provenance_retrieval_event', dict(turn_id=turn))
    check('Evidence provenance retains exact source identity on lookup', code == 200 and
          [r.get('id') for r in provenance.get('sources', [])] == expected)
    code, outcome = action('memory.record_retrieval_outcome', dict(
        retrieval_event_id=written['retrieval_event_id'], rows=[
            dict(id=value, verdict='accepted', weight=0.8) for value in expected]))
    check('Memory outcome writer accepts exact source IDs', code == 200 and outcome.get('written') == 4)
    code, outcome = action('memory.record_retrieval_outcome', dict(
        retrieval_event_id=written['retrieval_event_id'], surfaced_row_id='9223372036854775807', verdict='accepted'))
    check('Single memory outcome accepts int64 maximum', code == 200 and outcome.get('written') == 1)
    invalid = [9007199254740992, 9007199254740993, 1.5, '9223372036854775808',
               '01', '9007199254740993\x00suffix']
    for i, value in enumerate(invalid):
        failed_turn = turn + '-invalid-' + str(i)
        code, result = action('evidence.emit_retrieval_event',
            dict(turn_id=failed_turn, role='Recall', surfaced_ids=[42, value]))
        check('Evidence rejects ambiguous or malformed ID case ' + str(i), result.get('status') == 'error')
        code, result = action('evidence.trace_retrieval_event', dict(turn_id=failed_turn))
        check('Invalid identity creates no partial event case ' + str(i), code == 200 and
              result.get('trace_status') == 'evidence_unavailable')
    code, outcome = action('memory.record_retrieval_outcome', dict(
        retrieval_event_id=written['retrieval_event_id'], surfaced_row_id=9007199254740993, verdict='accepted'))
    check('Outcome writer never attributes a rounded numeric ID', code == 200 and outcome.get('written') == 0)


def correction_review_gate(kb, server, placement, output):
    """Exercise the shipping KB actions and model MCP adapter on fresh stores."""
    from types import SimpleNamespace
    gate = placement.Gate(SimpleNamespace(server=server.application, kb=kb.application,
        kb_store_db=kb.postgres, store_db=server.postgres))
    scope = 'review-e2e-' + uuid.uuid4().hex
    def check(name, passed):
        gate.check(name, passed)
        if not passed:
            raise RuntimeError(name)
    def action(verb, values):
        payload = dict(project=scope, scope_context=True)
        payload.update(values)
        # Exercise the existing authenticated host-caller transport. A direct
        # service bearer intentionally supplies no human actor. The enrolled
        # Server certificate, rotating bearer and service identity authenticate
        # the host before it can assert this synthetic operator account.
        code = '''import http.client,json,os,ssl,sys,tempfile
a=json.load(sys.stdin)
i=json.load(open('/var/lib/aimee/kb-client-identity.json'))
with tempfile.TemporaryDirectory() as directory:
  for name in ('cert','key'):
    path=os.path.join(directory,name)
    with open(path,'w',opener=lambda p,f: os.open(p,f,0o600)) as out: out.write(i[name])
  context=ssl.create_default_context(cadata=i['ca'])
  context.load_cert_chain(os.path.join(directory,'cert'),os.path.join(directory,'key'))
  c=http.client.HTTPSConnection('aimee-kb',8745,context=context,timeout=70)
  h={'Content-Type':'application/json','Authorization':'Bearer '+a['bearer'],
     'X-Aimee-Service-Authorization':'Bearer '+a['service'],
     'X-Aimee-Caller-Subject':'review_operator'}
  c.request('POST',a['path'],json.dumps(a['body']),h)
  r=c.getresponse(); print(json.dumps([r.status,json.loads(r.read())]))
'''
        status, result = json.loads(command('docker', 'exec', '-i', server.application,
            'python3', '-c', code, data=json.dumps(dict(path='/v1/actions/memory.' + verb,
            body=payload, bearer=kb.env['AIMEE_KB_API_BEARER_TOKEN'], service=kb.service_identity))))
        if status != 200:
            raise RuntimeError('authenticated review action transport returned HTTP ' + str(status))
        return result
    try:
        created = action('store', dict(key='review-original', content='verified original',
            authority='user', tier='L2', confidence=0.95))
        check('review fixture has verified user authorship', created.get('status') == 'ok' and
            created.get('memory', {}).get('provenance_category') == 'user_stated')
        old_id = created['id']
        version = action('get', dict(id=old_id, include_version=True))['memory']['version']
        correction = dict(verb='update', store='kb', project=scope, id=str(old_id),
            content='reviewed model correction', authority='user', expected_version=version,
            idempotency_key=scope + '-retry')
        proposed = gate.mcp_document('MCP creates linked correction draft', 'mutate', correction)
        check('model draft preserves authoritative current memory', proposed.get('kind') == 'review_required' and
            proposed.get('proposal', {}).get('state') == 'pending' and
            action('get', dict(id=old_id))['memory']['content'] == 'verified original')
        proposal = proposed['proposal']
        pid, digest = proposal['proposal_id'], proposal['payload_digest']
        inspected = action('correction_proposals', dict(proposal_id=pid))['proposals']
        check('draft inspection preserves model authority and cap', len(inspected) == 1 and
            inspected[0]['origin_authority'] == 'model' and inspected[0]['draft']['confidence'] == 0.8)
        check('draft inspection obeys parent scope', action('correction_proposals',
            dict(proposal_id=pid, project='other-' + scope)).get('proposals') == [])
        command('docker', 'restart', kb.application)
        kb.start()
        # Container health checks KB locally. The enrolled Server also needs
        # to reconnect to the restarted owner before exercising a mutation.
        # Use the same end-to-end readiness check as the placement restart
        # gates; never retry a conflicting mutation until it appears to pass.
        restored = gate.good('enrolled Server reaches restarted memory owner', gate.wait(
            'get', dict(store='kb', id=old_id, project=scope, include_version=True)))['memory']
        check('Server observes the exact target version after KB restart', restored.get('version') == version)
        replay = gate.mcp_document('MCP draft retry after KB restart', 'mutate', correction)
        retry_ok = (replay.get('proposal', {}).get('proposal_id') == pid and
            replay['proposal'].get('replayed') is True)
        if not retry_ok:
            # Fixed categories only: do not expose draft text, IDs or tokens.
            print('Review retry diagnostic: ' + json.dumps(dict(
                proposal_present=isinstance(replay.get('proposal'), dict),
                same_proposal=replay.get('proposal', {}).get('proposal_id') == pid,
                replayed=replay.get('proposal', {}).get('replayed') is True,
                conflict=replay.get('kind') == 'conflict',
                forbidden=replay.get('kind') == 'forbidden',
                unavailable=replay.get('kind') in ('unavailable', 'upstream_error', 'capability_absent'),
                review_required=replay.get('kind') == 'review_required')), flush=True)
        check('draft retry survives owner restart', retry_ok)
        check('restart and derived primary scope preserve target version',
            action('get', dict(id=old_id, include_version=True))['memory']['version'] == version)
        review = dict(proposal_id=pid, payload_digest=digest, expected_version=version, action='approve')
        check('direct service bearer cannot claim human review authority', kb.kb_request(
            '/v1/actions/memory.review_correction', dict(review, project=scope, authority='user'))[1].get('kind') == 'forbidden')
        check('unauthenticated HTTP cannot review a draft', kb.kb_request(
            '/v1/actions/memory.review_correction', dict(review, project=scope), authenticated=False)[0] == 401)
        check('review binds exact draft digest', action('review_correction',
            dict(review, payload_digest='0' * 64)).get('kind') == 'conflict')
        accepted = action('review_correction', review)
        check('verified KB review approves the draft', accepted.get('status') == 'ok' and
            accepted.get('proposal', {}).get('state') == 'approved')
        approved = accepted['proposal']
        new_id = approved['result_version']['record_id']
        check('approval separates model authorship from reviewer', approved['origin_authority'] == 'model' and
            approved['revision_authority'] == 'model' and approved['review_authority'] == 'user' and
            bool(approved.get('reviewer')) and bool(approved.get('decision_id')))
        current = action('get', dict(id=new_id))['memory']
        check('approved model version keeps recomputed confidence', current['content'] == 'reviewed model correction' and
            current['confidence'] == 0.8 and current['provenance_category'] == 'reviewed_model')
        check('approved extraction remains model authored', gate.sql(
            f"SELECT (actor_role='model' AND authority_rank=10 AND authenticated=0)::text FROM memory_fact_actors WHERE memory_id={int(new_id)}") == 'true')
        repeated = action('review_correction', review).get('proposal', {})
        check('review replay keeps one canonical commit', repeated.get('replayed') is True and
            repeated.get('review_commit_id') == approved['review_commit_id'])
        followup = dict(verb='update', store='kb', project=scope, id=new_id, content='rejected model follow-up')
        pending = gate.mcp_document('MCP reviewed content requires another review', 'mutate', followup)
        check('reviewed model text cannot be silently overwritten', pending.get('kind') == 'review_required' and
            pending.get('proposal', {}).get('state') == 'pending')
        reject = pending['proposal']
        rejected = action('review_correction', dict(proposal_id=reject['proposal_id'],
            payload_digest=reject['payload_digest'], expected_version=reject['target_version'], action='reject'))
        check('verified reviewer rejects the exact follow-up', rejected.get('status') == 'ok' and
            rejected.get('proposal', {}).get('state') == 'rejected')
        duplicate = gate.mcp_document('MCP repeats rejected draft', 'mutate', followup)
        check('rejected draft cannot reopen through another model call',
            duplicate.get('proposal', {}).get('proposal_id') == reject['proposal_id'] and
            duplicate['proposal'].get('state') == 'rejected')
        check('review workflow retains exactly old and approved versions', gate.sql(
            f"SELECT count(*) FROM memories WHERE scope_type='project' AND scope_value='{scope}'") == '2')
        check('rejected draft never enters recall storage', gate.sql(
            f"SELECT count(*) FROM memories WHERE scope_value='{scope}' AND content='rejected model follow-up'") == '0')
        check('explicit erasure removes original proposal parent', action('delete', dict(id=old_id, authority='user')).get('status') == 'ok')
        check('parent erasure removes draft payload', action('correction_proposals', dict(proposal_id=pid)).get('proposals') == [])
        erased = gate.mcp_document('MCP erased proposal retry', 'mutate', correction)
        check('erasure does not free a committed proposal retry key', erased.get('reason') == 'idempotent_result_unavailable')
    finally:
        (output / 'correction-review.json').write_text(json.dumps(gate.checks, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--topology', choices=('T1', 'T2', 'T3'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--keep', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, AIMEE_RUNTIME_WEB_ENABLED='0', AIMEE_POSTGRES_VOLUME_MIB='512',
               COMPOSE_PROFILES='', EMBEDDER_MODEL='bekko-a25m',
               EMBEDDER_URL='https://aimee-embedder:8762', EMBEDDER_DIMS='384')
    # A missing operator cap silently skips the deployment-ceiling regressions.
    # Every release topology must exercise this boundary in the actual process.
    if not env.get('AIMEE_PROVIDER_CONTEXT_LIMITS'):
        env['AIMEE_PROVIDER_CONTEXT_LIMITS'] = json.dumps(dict(schema_version=1, max_request_bytes=32768))
    for name in ('AIMEE_APPLICATION_IMAGE', 'AIMEE_POSTGRES_IMAGE', 'AIMEE_EMBEDDER_IMAGE'):
        if not env.get(name):
            parser.error(name + ' must name the candidate image')
        command('docker', 'image', 'inspect', env[name])
    stacks, checks = [], []

    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)
        if not passed:
            raise RuntimeError(name)

    try:
        kb = None
        if args.topology in ('T1', 'T2'):
            kb = Stack('kb', env, args.output)
            stacks.append(kb)
            kb.start()
            check('KB application metadata contains no database or enrollment credential', application_metadata_is_private(kb))
            command('docker', 'exec', '-i', '-u', '1000', kb.application, 'aimee-kb',
                '--bootstrap-vault-stdin', data='AIMEE_KB_SERVICE_IDENTITY_TOKEN=' + kb.service_identity + '\0')
            command('docker', 'restart', kb.application)
            kb.start()
            code, body = kb.kb_request('/v1/health')
            check('KB boots with standardized PostgreSQL and local embedding', code == 200 and body.get('db2_ok') is True)
            code, body = kb.kb_request('/v1/actions/memory.store', dict(key='shared-e2e-' + uuid.uuid4().hex,
                content='Synthetic shared deployment fixture'))
            check('KB shared memory module stores a record', code == 200 and body.get('status') == 'ok')
            for confidence in (-1, 1.01, False, None, 'invalid', [], {}):
                code, body = kb.kb_request('/v1/actions/memory.store', dict(
                    key='invalid-confidence-e2e', content='Synthetic shared fixture',
                    confidence=confidence))
                check('Direct KB store rejects confidence ' + repr(confidence),
                      body.get('status') == 'error' and
                      'confidence must be between 0 and 1' in body.get('message', ''))
            code, body = kb.kb_request('/v1/search', dict(query='shared deployment fixture', scope='all', max_results=3))
            check('KB ranked search uses its local embedding service', code == 200 and isinstance(body.get('hits'), list))
            code, body = kb.kb_request('/v1/actions/memory.find_facts', dict(query='shared deployment fixture', limit=3, graph_code_fusion_state='on'))
            check('KB graph and memory retrieval survives the deepest worker path', code == 200 and isinstance(body.get('facts'), list))
            typed_context_budget_gate(kb, check)
            typed_source_version_gate(kb, check)
            linked_relation_input_gate(kb, check)
            preview_source_version_gate(kb, check)
            evidence_exact_identity_gate(kb, check)
        if args.topology in ('T2', 'T3'):
            server = Stack('server', env, args.output)
            stacks.append(server)
            server.start()
            check('Server boots without a KB connection', True)
            check('Server application metadata contains no database or enrollment credential', application_metadata_is_private(server))
            gate_script = ROOT / 'tests/e2e/memory-placement-e2e.py'
            common = ['--server', server.application, '--store-db', server.postgres]
            command('python3', str(gate_script), *common, '--output', str(args.output / 'local-memory.json'), timeout=900)
            check('KB-free personal memory regression gate', True)
            if kb:
                # The two projects retain separate stores, model identities and
                # Vaults. Only the optional KB application joins the API network.
                command('docker', 'network', 'connect', '--alias', 'aimee-kb',
                        server.project + '_default', kb.application)
                connection = command('docker', 'exec', '-u', '1000', kb.application,
                    'aimee-kb', 'enroll', '--host=aimee-kb', '--port=8745', '--scope=service:aimee-server')
                if not connection.startswith('aimee://') or '\n' in connection:
                    raise RuntimeError('KB enrollment did not return one connection string')
                # Exercise the same Vault-only config API used by Settings.
                spec = importlib.util.spec_from_file_location('memory_gate', gate_script)
                placement = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(placement)
                for key, value in [('kb_client_bearer_token', kb.env['AIMEE_KB_API_BEARER_TOKEN']),
                                   ('kb_service_identity_token', kb.service_identity),
                                   ('kb_connection_string', connection), ('kb_mode', 'remote')]:
                    reply = json.loads(command('docker', 'exec', '-i', server.application,
                        'python3', '-c', placement.HTTP, data=json.dumps(dict(method='POST',
                        path='/v1/config/set', body=dict(key=key, value=value)))))
                    if reply[0] != 200 or reply[1].get('status') != 'ok':
                        raise RuntimeError('KB connection config API failed for ' + key)
                    if key != 'kb_mode' and (reply[1].get('value') is not True or reply[1].get('secret') is not True):
                        raise RuntimeError('KB credential response was not redacted')
                command('docker', 'restart', server.application)
                server.start()
                correction_review_gate(kb, server, placement, args.output)
                check('Linked model correction drafts and authenticated decisions', True)
                command('python3', str(gate_script), *common, '--kb', kb.application,
                    '--kb-store-db', kb.postgres, '--output', str(args.output / 'shared-memory.json'), timeout=900)
                check('Enrolled optional KB, scope isolation, restart and outage regressions', True)
                command('python3', str(ROOT / 'tests/e2e/instance-identity-e2e.py'),
                    '--server', server.application, '--kb', kb.application,
                    '--image', env['AIMEE_APPLICATION_IMAGE'], '--output', str(args.output / 'identity.json'))
                check('One application image and immutable first-boot identities', True)
            else:
                command('python3', str(ROOT / 'tests/e2e/local-model-memory-e2e.py'), *common,
                    '--embedder', server.embedder, '--output', str(args.output / 'semantic-memory.json'), timeout=600)
                check('Real local semantic recall, outage recovery and retirement gate', True)
                command('python3', str(ROOT / 'tests/e2e/memory-exploratory-e2e.py'), *common,
                    '--output', str(args.output / 'exploratory-memory.json'), timeout=300)
                check('Concurrent memory, exact IDs and supervised owner recovery gate', True)
            command('python3', str(ROOT / 'tests/e2e/memory-provider-boundary-e2e.py'),
                '--server', server.application, '--output', str(args.output / 'provider-boundary.json'), timeout=600)
            check('Provider-bound memory, constraints, tools and continuation gate', True)
            command('python3', str(ROOT / 'tests/e2e/memory-native-async-e2e.py'),
                '--server', server.application, '--output', str(args.output / 'native-async.json'), timeout=600)
            check('Live asynchronous native memory refusal and recovery gate', True)
    except (RuntimeError, subprocess.SubprocessError, ValueError, OSError) as error:
        checks.append(dict(name='topology completed', passed=False, error=str(error)))
        print('FAIL ' + str(error), flush=True)
    finally:
        (args.output / 'topology.json').write_text(json.dumps(checks, indent=2) + '\n')
        # Export only this fixed diagnostic grammar. Full Docker output may
        # contain bootstrap credentials, request bodies or SQL driver details.
        failures = []
        pattern = re.compile(r'memory data failure operation="([a-z-]{1,64})" trace=([0-9]+) status=([0-9]+) class=(internal|deadline|cancelled|store_unavailable|result_capacity|transaction_closed|sqlstate_[0-9A-Z]{5})(?=\s|$)')
        for stack in stacks:
            try:
                logs = subprocess.run(['docker', 'logs', '--tail', '2000', stack.application],
                    capture_output=True, text=True, timeout=15)
                for operation, trace, status, kind in pattern.findall(logs.stdout + logs.stderr):
                    failures.append(dict(role=stack.role, operation=operation, trace=trace,
                                         status=int(status), error_class=kind))
            except (OSError, subprocess.SubprocessError):
                failures.append(dict(role=stack.role, diagnostics='unavailable'))
        (args.output / 'memory-failures.json').write_text(json.dumps(failures, indent=2) + '\n')
        if not args.keep:
            for stack in reversed(stacks):
                stack.compose('down', '--volumes', '--remove-orphans')
        else:
            print('Retained disposable projects: ' + ', '.join(stack.project for stack in stacks))
    return 0 if checks and all(row['passed'] for row in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
