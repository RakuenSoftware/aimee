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
c.request('GET' if a['body'] is None else 'POST',a['path'],None if a['body'] is None else json.dumps(a['body']),h)
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
        if limit == 0:
            check('Zero byte typed projection emits no wrappers or retained IDs',
                  rendered == b'' and result.get('retained_items') == [] and
                  result.get('context_sufficiency') == 'insufficient')
        if limit == 400:
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
    except (RuntimeError, subprocess.SubprocessError, ValueError, OSError) as error:
        checks.append(dict(name='topology completed', passed=False, error=str(error)))
        print('FAIL ' + str(error), flush=True)
    finally:
        (args.output / 'topology.json').write_text(json.dumps(checks, indent=2) + '\n')
        if not args.keep:
            for stack in reversed(stacks):
                stack.compose('down', '--volumes', '--remove-orphans')
        else:
            print('Retained disposable projects: ' + ', '.join(stack.project for stack in stacks))
    return 0 if checks and all(row['passed'] for row in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
