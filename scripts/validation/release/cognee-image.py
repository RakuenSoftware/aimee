#!/usr/bin/env python3
"""Exercise image Cognee wiring, canonical authority, deletion and derived reset."""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import threading
import time
from types import SimpleNamespace
import urllib.error
import urllib.parse
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[3]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', type=Path, required=True)
    parser.add_argument('--attempt', default='cognee-image')
    parser.add_argument('--keep', action='store_true')
    args = parser.parse_args()
    if not args.attempt or Path(args.attempt).name != args.attempt or args.attempt in ('.', '..'):
        parser.error('attempt must be a single directory name')
    stage = args.stage.resolve()
    output = stage / 'evidence' / args.attempt
    output.mkdir(parents=True, exist_ok=True)
    private = stage / 'private' / args.attempt
    private.mkdir(parents=True, mode=0o700, exist_ok=True)
    os.chmod(private, 0o700)
    if (private / 'data').exists():
        parser.error('use a fresh Cognee stage; refusing stale provider state')
    matrix = load('matrix', 'tests/e2e/deployment-matrix.py')
    placement = load('placement', 'tests/e2e/memory-placement-e2e.py')
    # The production wrapper intentionally suppresses potentially secret-bearing
    # Compose output. Preserve failed bootstrap diagnostics in this disposable
    # fixture's private directory, without changing the production wrapper.
    bootstrap = load('vault_bootstrap', 'scripts/compose-vault-init.py')
    original_command = matrix.command

    def diagnostic_command(*argv, env=None, data=None, timeout=300):
        if len(argv) < 2 or Path(argv[1]).name != 'compose-vault-init.py':
            return original_command(*argv, env=env, data=data, timeout=timeout)

        def private_run(command, **kwargs):
            result = subprocess.run(command, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=120, **kwargs)
            if result.returncode:
                path = private / ('bootstrap-failure-' + uuid.uuid4().hex + '.log')
                fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, 'wb') as log:
                    log.write(result.stdout)
                    log.write(b'\n')
                    log.write(result.stderr)
                raise RuntimeError('Compose Vault bootstrap failed; diagnostic retained in private fixture directory')
            return result.stdout

        bootstrap.run = private_run
        bootstrap.main(list(argv[2:]))
        return ''

    matrix.command = diagnostic_command
    fixture = load('contract', 'scripts/validation/memory/run-cognee-contract.py')
    model = ThreadingHTTPServer(('127.0.0.1', 0), fixture.ModelFixture)
    threading.Thread(target=model.serve_forever, daemon=True).start()
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        api_port = reservation.getsockname()[1]
    api_url = 'http://127.0.0.1:' + str(api_port)

    class Proxy(BaseHTTPRequestHandler):
        blocked = False
        fault = None
        requests = 0

        def log_message(self, *args):
            pass

        def request(self):
            Proxy.requests += 1
            if self.blocked:
                self.send_response(503)
                self.end_headers()
                self.wfile.write(b'{"detail":"injected disposable provider outage"}')
                return
            body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
            fault = Proxy.fault
            if fault and self.command == fault['method'] and self.path.startswith(fault['path']):
                raw = fault.get('body', b'{}')
                self.send_response(fault.get('status', 503))
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
                return
            headers = {key: value for key, value in self.headers.items()
                       if key.lower() not in ('host', 'connection', 'content-length')}
            request = urllib.request.Request(api_url + self.path, method=self.command,
                headers=headers, data=body if self.command in ('POST', 'PUT') else None)
            try:
                response = urllib.request.urlopen(request, timeout=120)
            except urllib.error.HTTPError as error:
                response = error
            with response:
                data = response.read()
                self.send_response(response.status)
                self.send_header('Content-Type', response.headers.get('Content-Type', 'application/json'))
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        do_GET = request
        do_POST = request
        do_DELETE = request

    # Only the Docker host bridge receives provider traffic; no LAN listener.
    proxy = ThreadingHTTPServer(('172.17.0.1', 0), Proxy)
    threading.Thread(target=proxy.serve_forever, daemon=True).start()
    endpoint = 'http://172.17.0.1:' + str(proxy.server_port)
    model_url = 'http://127.0.0.1:' + str(model.server_port) + '/v1'
    env = {key: value for key, value in os.environ.items()
           if not any(part in key for part in ('API_KEY', 'TOKEN', 'PASSWORD', 'SECRET'))}
    env.update(DATA_ROOT_DIRECTORY=str(private / 'data'), SYSTEM_ROOT_DIRECTORY=str(private / 'system'),
        CACHE_ROOT_DIRECTORY=str(private / 'cache'), COGNEE_LOGS_DIR=str(private / 'logs'),
        COGNEE_REPOS_DIR=str(private / 'repos'), DEFAULT_USER_PASSWORD='contract-fixture-password',
        LLM_PROVIDER='custom', LLM_MODEL='openai/fixture', LLM_ENDPOINT=model_url, LLM_API_KEY='fixture',
        EMBEDDING_PROVIDER='openai', EMBEDDING_MODEL='text-embedding-3-small',
        EMBEDDING_ENDPOINT=model_url, EMBEDDING_API_KEY='fixture', EMBEDDING_DIMENSIONS='8',
        COGNEE_TRACING_ENABLED='false', TELEMETRY_DISABLED='true')
    checks, stacks, prefixes = [], [], []
    provider = None
    provider_log = (private / 'provider.log').open('w')

    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)
        if not passed:
            raise RuntimeError(name)

    def provider_request(path, data=None, content_type='application/json'):
        headers = {'Authorization': 'Bearer ' + token}
        if data is not None:
            headers['Content-Type'] = content_type
        request = urllib.request.Request(api_url + path, headers=headers, data=data)
        with urllib.request.urlopen(request, timeout=120) as response:
            raw = response.read()
            return json.loads(raw) if raw else None

    def datasets():
        return [row for row in provider_request('/api/v1/datasets')
                if any(row['name'].startswith(prefix) for prefix in prefixes)]

    try:
        provider = subprocess.Popen([str(stage / 'cognee-venv/bin/python'), '-m', 'uvicorn',
            'cognee.api.client:app', '--host', '127.0.0.1', '--port', str(api_port)],
            cwd=private, env=env, stdout=provider_log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 150
        while time.monotonic() < deadline:
            if provider.poll() is not None:
                raise RuntimeError('Cognee exited; private provider log retained')
            try:
                urllib.request.urlopen(api_url + '/openapi.json', timeout=2).close()
                break
            except OSError:
                time.sleep(.5)
        else:
            raise RuntimeError('Cognee API startup timed out')
        login = urllib.request.Request(api_url + '/api/v1/auth/login',
            data=urllib.parse.urlencode(dict(username='default_user@example.com',
                password='contract-fixture-password')).encode(),
            headers={'Content-Type': 'application/x-www-form-urlencoded'})
        with urllib.request.urlopen(login, timeout=30) as response:
            token = json.load(response)['access_token']
        check('real Cognee 1.6.2 authenticates disposable account', bool(token))
        app_env = dict(os.environ, AIMEE_RUNTIME_WEB_ENABLED='0', COMPOSE_PROFILES='',
            EMBEDDER_MODEL='bekko-a25m', EMBEDDER_DIMS='384',
            EMBEDDER_URL='https://aimee-embedder:8762')
        for role in ('kb', 'server'):
            stack = matrix.Stack(role, app_env, output)
            stacks.append(stack)
            stack.network_override.write_text('services:\n  aimee-' + role + ':\n    ports: !reset []\n'
                '    environment:\n      AIMEE_MEMORY_BACKEND: cognee\n'
                '      AIMEE_MEMORY_BACKEND_URL: ' + json.dumps(endpoint) + '\n' +
                ('      AIMEE_KB_HTTP_BIND: \'1\'\n' if role == 'kb' else ''))
            matrix.command('python3', str(ROOT / 'scripts/compose-vault-init.py'),
                *stack.compose_args(), 'up', env=stack.env)
            binary = 'aimee-' + role
            matrix.command('docker', 'compose', *stack.compose_args(), 'run', '--rm', '--no-deps', '-T',
                '--entrypoint', '/usr/sbin/runuser', binary, '-u', 'aimee', '--', binary,
                '--bootstrap-vault-stdin', env=stack.env, data='AIMEE_MEMORY_BACKEND_TOKEN=' + token + '\0')
            stack.start()
            check(role + ' with Cognee reaches candidate-image readiness', True)
            check(role + ' keeps credentials out of container metadata', matrix.application_metadata_is_private(stack))
            node = json.loads(matrix.command('docker', 'exec', stack.application, 'cat',
                '/var/lib/aimee/instance-identity.json'))['id']
            prefixes.append('aimee_' + hashlib.sha256(node.encode()).hexdigest()[:32] + '_')
        kb, server = stacks
        matrix.command('docker', 'exec', '-i', '-u', '1000', kb.application, 'aimee-kb',
            '--bootstrap-vault-stdin', data='AIMEE_KB_SERVICE_IDENTITY_TOKEN=' + kb.service_identity + '\0')
        kb.compose('restart', 'aimee-kb')
        kb.start()
        matrix.command('docker', 'network', 'connect', '--alias', 'aimee-kb',
            server.project + '_default', kb.application)
        connection = matrix.command('docker', 'exec', '-u', '1000', kb.application,
            'aimee-kb', 'enroll', '--host=aimee-kb', '--port=8745', '--scope=service:aimee-server')
        gate = placement.Gate(SimpleNamespace(server=server.application, store_db=server.postgres))
        for key, value in [('kb_client_bearer_token', kb.env['AIMEE_KB_API_BEARER_TOKEN']),
                          ('kb_service_identity_token', kb.service_identity),
                          ('kb_connection_string', connection), ('kb_mode', 'remote')]:
            status, result = gate.call('/v1/config/set', dict(key=key, value=value))
            check('optional KB config seals ' + key, status == 200 and result.get('status') == 'ok')
        server.compose('restart', 'aimee-server')
        server.start()

        def call(path, body):
            status, result = gate.call(path, body)
            if result.get('object') == 'op.run':
                run_id = result['id']
                deadline = time.monotonic() + 180
                while result.get('status') in ('queued', 'in_progress') and time.monotonic() < deadline:
                    time.sleep(.2)
                    status, result = gate.call('/v1/runs/' + run_id, method='GET')
                if result.get('status') not in ('completed', 'failed'):
                    raise RuntimeError('asynchronous fixture operation did not terminate')
                result = result.get('result', result)
            return status, result

        records = {}
        marker = 'needle-published-' + uuid.uuid4().hex[:10]
        for store in ('user', 'kb'):
            context = dict(store=store, project='published-cognee' if store == 'kb' else '')
            status, result = call('store', dict(context, key=store + '-' + marker,
                content='needle canonical ' + store + ' ' + marker, kind='fact'))
            check(store + ' canonical store succeeds', status == 200 and result.get('status') == 'ok')
            records[store] = dict(context, id=str(result['id']))
            status, result = call('search', dict(context, keywords=['needle'], limit=10))
            text = json.dumps(result)
            check(store + ' real Cognee retrieval uses canonical scoped records', status == 200 and
                  store + ' ' + marker in text and ('kb' if store == 'user' else 'user') + ' ' + marker not in text)
        catalog = datasets()
        check('both immutable node namespaces own real derived datasets',
              all(any(row['name'].startswith(prefix) for row in catalog) for prefix in prefixes))
        old_name = next(row['name'] for row in catalog if row['name'].startswith(prefixes[1]))
        def restored_search(request, marker_text):
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                status, result = call('search', request)
                if status == 200 and marker_text in json.dumps(result):
                    return True
                time.sleep(1)
            return False

        # Exercise every provider route through the actual admitted memory/egress pair.
        probe_query = dict(store='user', keywords=['needle'], limit=10)
        for label, method, route in [('catalog', 'GET', '/api/v1/datasets'),
                ('add', 'POST', '/api/v1/add'), ('cognify', 'POST', '/api/v1/cognify'),
                ('search', 'POST', '/api/v1/search')]:
            if label == 'add':
                status, added = call('store', dict(store='user', key='fault-add-' + marker,
                    content='needle added route fixture ' + marker, kind='fact'))
                check('new canonical record supplies the real add route', status == 200)
            Proxy.fault = dict(method=method, path=route)
            status, result = call('search', probe_query)
            check('real provider ' + label + ' failure refuses retrieval without native fallback',
                  result.get('status') == 'error' and not result.get('memories'))
            Proxy.fault = None
            check('real provider ' + label + ' recovers through the same runtime',
                  restored_search(probe_query, 'user ' + marker))
        for label, route, response in [('malformed catalog', '/api/v1/datasets', b'{'),
                ('invalid dataset identity', '/api/v1/datasets', b'[{"id":"../escape","name":"foreign"}]'),
                ('incomplete cognification', '/api/v1/cognify', b'{}'),
                ('foreign retrieval result', '/api/v1/search', b'[{"dataset_name":"foreign","search_result":[]}]')]:
            Proxy.fault = dict(method='GET' if route.endswith('datasets') else 'POST',
                path=route, status=200, body=response)
            status, result = call('search', probe_query)
            check(label + ' is refused by the real runtime', result.get('status') == 'error')
            Proxy.fault = None
            check(label + ' recovery preserves canonical retrieval',
                  restored_search(probe_query, 'user ' + marker))
        matrix.command('docker', 'exec', '-i', '-u', '1000', '-e', 'AIMEE_VAULT_ENV_OVERWRITE=1',
            server.application, 'aimee-server', '--bootstrap-vault-stdin',
            data='AIMEE_MEMORY_BACKEND_TOKEN=invalid-disposable-cognee-token\0')
        status, result = call('search', probe_query)
        check('real Cognee rejects a rotated invalid Vault bearer without native fallback',
              result.get('status') == 'error')
        status, result = call('get', records['user'])
        check('provider authentication failure preserves exact canonical reads', status == 200)
        matrix.command('docker', 'exec', '-i', '-u', '1000', '-e', 'AIMEE_VAULT_ENV_OVERWRITE=1',
            server.application, 'aimee-server', '--bootstrap-vault-stdin',
            data='AIMEE_MEMORY_BACKEND_TOKEN=' + token + '\0')
        check('real Cognee observes restored Vault bearer on the existing pipe',
              restored_search(probe_query, 'user ' + marker))
        status, correction = call('get', dict(records['user'], include_version=True))
        status, revised = call('supersede', dict(store='user', old_id=records['user']['id'],
            new_content='needle user corrected ' + marker, expected_version=correction['memory']['version']))
        check('Cognee selection retains the canonical versioned correction API', status == 200)
        check('real Cognee reindexes the corrected canonical revision',
              restored_search(probe_query, 'user corrected ' + marker))
        for store in ('user', 'kb'):
            status, recalled = call('recall', dict(store=store, task_hint='needle', limit_tokens=8192))
            check(store + ' recall traverses the selected real Cognee backend', status == 200 and
                ('user corrected ' + marker if store == 'user' else 'kb ' + marker) in json.dumps(recalled))
            cli = gate.cli('recall', '--query', 'needle', '--store', store)
            check(store + ' native CLI recall reaches real Cognee',
                ('user corrected ' + marker if store == 'user' else 'kb ' + marker) in json.dumps(cli))
            code_mcp, recalled_mcp = gate.mcp('memory_recall', dict(task_hint='needle', store=store))
            check(store + ' MCP recall reaches real Cognee with canonical scope', code_mcp == 200 and
                ('user corrected ' + marker if store == 'user' else 'kb ' + marker) in recalled_mcp)
        status, exported = call('/v1/native/primitive', dict(task_hint='needle', session_start=True, limit_tokens=8192))
        check('native memory export uses Cognee selection and exact canonical versions',
            status == 200 and any('user corrected ' + marker in row.get('content', '') and
                isinstance(row.get('version', {}).get('record_revision'), str) for row in exported.get('records', [])))
        user_prefix = prefixes[1]
        corrected_name = next(row['name'] for row in datasets() if row['name'].startswith(user_prefix)
            and '_' + records['user']['id'] + '_' in row['name'])
        check('real Cognee removes obsolete correction datasets', sum(
            row['name'].startswith(user_prefix) and '_' + records['user']['id'] + '_' in row['name']
            for row in datasets()) == 1)
        status, auxiliary = call('get', dict(store='user', id=str(added['id']), include_version=True))
        auxiliary_delete = dict(store='user', id=str(added['id']),
            expected_version=auxiliary['memory']['version'], idempotency_key='route-delete-' + uuid.uuid4().hex)
        Proxy.fault = dict(method='DELETE', path='/api/v1/datasets/')
        status, result = call('delete', auxiliary_delete)
        check('real provider delete failure refuses a canonical retirement receipt',
              result.get('status') == 'error' and 'mutation_receipt' not in result)
        Proxy.fault = None
        status, result = call('get', dict(store='user', id=str(added['id'])))
        check('failed real dataset deletion preserves the active canonical record', status == 200)
        status, result = call('delete', auxiliary_delete)
        check('same deletion succeeds after real dataset route recovery',
              status == 200 and result.get('deleted') is True)
        old_name = corrected_name
        deletions = {}
        for store, request in records.items():
            status, result = call('get', dict(request, include_version=True))
            check(store + ' exposes a canonical version for conditional deletion', status == 200)
            deletions[store] = dict(request, expected_version=result['memory']['version'],
                idempotency_key='published-cognee-delete-' + uuid.uuid4().hex)
        Proxy.blocked = True
        for store, request in deletions.items():
            status, result = call('delete', request)
            check(store + ' provider outage refuses destructive completion',
                  result.get('status') == 'error' and 'mutation_receipt' not in result)
        Proxy.blocked = False
        for store, request in deletions.items():
            status, result = call('get', records[store])
            check(store + ' outage preserves serving canonical record', status == 200)
            status, result = call('delete', request)
            check(store + ' same conditional request succeeds after provider recovery',
                  status == 200 and result.get('deleted') is True and 'mutation_receipt' in result)
            status, result = call('delete', request)
            check(store + ' deletion retry is idempotent', status == 200 and
                  result.get('mutation_receipt', {}).get('replayed') is True)
            status, result = call('get', records[store])
            check(store + ' deleted record leaves current canonical retrieval', status == 404)
        check('both nodes remove real deleted derived datasets', not datasets())
        check('shared user-authority deletion destroys canonical row',
              matrix.command('docker', 'exec', kb.postgres, 'psql', '-U', 'postgres',
              '-d', 'aimee_store', '-X', '-At', '-c', 'SELECT count(*) FROM memories WHERE id=' + records['kb']['id']) == '0')
        check('personal retirement leaves no active canonical row', gate.personal_sql(
            "SELECT count(*) FROM user_memories WHERE lifecycle_state='active' AND id=" + records['user']['id']) == '0')
        status, retained = call('store', dict(store='user', key='retained-' + marker,
            content='needle retained canonical ' + marker, kind='fact'))
        retained_request = dict(store='user', id=str(retained['id']))
        check('unrelated canonical record survives deletion', status == 200)
        status, result = call('search', dict(store='user', keywords=['needle'], limit=10))
        check('retained canonical record reindexes after deletion', status == 200 and
              'retained canonical ' + marker in json.dumps(result))
        boundary = 'disposable-provider-restore'
        body = (f'--{boundary}\r\nContent-Disposition: form-data; name="datasetName"\r\n\r\n{old_name}\r\n'
                f'--{boundary}\r\nContent-Disposition: form-data; name="data"; filename="restored.txt"\r\n'
                f'Content-Type: text/plain\r\n\r\nneedle retired historical fixture\r\n--{boundary}--\r\n').encode()
        provider_request('/api/v1/add', body, 'multipart/form-data; boundary=' + boundary)
        check('fixture restores derived data for a deleted record', any(row['name'] == old_name for row in datasets()))
        code = '''import os,signal
from pathlib import Path
pids=[]
for p in Path('/proc').iterdir():
 if p.name.isdigit():
  try:
   argv=(p/'cmdline').read_bytes().split(b'\\0')
   if Path(os.fsdecode(argv[0])).name=='aimee-module-memory': pids.append(int(p.name))
  except OSError: pass
if len(pids)!=1: raise SystemExit('expected one supervised memory child')
os.kill(pids[0],signal.SIGTERM)
'''
        matrix.command('docker', 'exec', server.application, 'python3', '-c', code)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline and any(row['name'] == old_name for row in datasets()):
            time.sleep(.5)
        check('supervised memory restart removes restored derived state for deleted records',
              not any(row['name'] == old_name for row in datasets()))
        check('restart cannot reactivate retired canonical row', gate.personal_sql(
            "SELECT count(*) FROM user_memories WHERE lifecycle_state='active' AND id=" + records['user']['id']) == '0')
        status, result = call('get', retained_request)
        check('derived reset preserves unrelated canonical record', status == 200 and
              'retained canonical ' + marker in json.dumps(result))
        status, result = call('search', dict(store='user', keywords=['needle'], limit=10))
        check('retained canonical record reindexes after derived reset', status == 200 and
              'retained canonical ' + marker in json.dumps(result))
        before = Proxy.requests
        status, listed = call('search', dict(store='user', keywords=[], limit=10))
        check('empty-query listing remains canonical and bypasses Cognee',
            status == 200 and 'retained canonical ' + marker in json.dumps(listed)
            and Proxy.requests == before)
        for store in ('user', 'kb'):
            status, eligible = call('store', dict(store=store, key='eligibility-' + store + '-' + marker,
                content='needle lifecycle ' + store + ' ' + marker, kind='fact'))
            check(store + ' creates a canonical eligibility fixture', status == 200)
            mid = str(eligible['id'])
            query = dict(store=store, keywords=['needle'], limit=10)
            text = 'lifecycle ' + store + ' ' + marker
            check(store + ' eligible fixture reaches real derived retrieval', restored_search(query, text))
            table = 'user_memories' if store == 'user' else 'memories'
            def fixture_sql(sql):
                if store == 'user':
                    return gate.personal_sql(sql)
                return matrix.command('docker', 'exec', kb.postgres, 'psql', '-U', 'postgres',
                    '-d', 'aimee_store', '-X', '-At', '-v', 'ON_ERROR_STOP=1', '-c', sql)
            cases = [('expired', "valid_until='2000-01-01T00:00:00Z'", 'valid_until=NULL' if store == 'user' else "valid_until=''"),
                ('retired', "lifecycle_state='retired'", "lifecycle_state='active'")]
            if store == 'kb':
                cases += [('suppressed', 'activation_suppressed=1', 'activation_suppressed=0'),
                    ('future', "valid_from='2999-01-01T00:00:00Z'", "valid_from=''")]
            prefix = prefixes[1 if store == 'user' else 0]
            for label, disable, restore in cases:
                fixture_sql('UPDATE ' + table + ' SET ' + disable + ' WHERE id=' + mid)
                status, absent = call('search', query)
                check(store + ' ' + label + ' record is excluded and its derived dataset removed',
                    status == 200 and text not in json.dumps(absent) and not any(
                        row['name'].startswith(prefix) and '_' + mid + '_' in row['name'] for row in datasets()))
                fixture_sql('UPDATE ' + table + ' SET ' + restore + ' WHERE id=' + mid)
                check(store + ' restored ' + label + ' record reindexes through real Cognee', restored_search(query, text))
            status, current = call('get', dict(store=store, id=mid, include_version=True))
            status, removed = call('delete', dict(store=store, id=mid, expected_version=current['memory']['version'],
                idempotency_key='eligibility-delete-' + uuid.uuid4().hex))
            check(store + ' synthetic eligibility target cleans up through the versioned API',
                status == 200 and removed.get('deleted') is True)
        capacity_prefix = 'capacity-' + marker + '-'
        gate.personal_sql("INSERT INTO user_memories(kind,key,content) SELECT 'fact','" + capacity_prefix +
            "'||n::text,'needle capacity fixture' FROM generate_series(1,257) AS n")
        try:
            before = Proxy.requests
            status, capacity = call('search', dict(store='user', keywords=['needle'], limit=10))
            check('257 canonical records refuse Cognee capacity without provider calls or fallback',
                capacity.get('status') == 'error' and not capacity.get('memories') and Proxy.requests == before)
            status, current = call('get', retained_request)
            check('Cognee capacity refusal preserves exact canonical reads', status == 200)
        finally:
            gate.personal_sql("DELETE FROM user_memories WHERE key LIKE '" + capacity_prefix + "%'")
        check('retrieval recovers after synthetic capacity fixture removal',
            restored_search(dict(store='user', keywords=['needle'], limit=10), 'retained canonical ' + marker))
        # An explicit operator-owned loopback bridge exercises the privileged
        # coordinator without widening the existing scoped service identity.
        operator_records = {}
        for store in ('user', 'kb'):
            status, created = call('store', dict(store=store, key='operator-' + store + '-' + marker,
                content='needle operator subject ' + store + ' ' + marker, kind='fact'))
            check(store + ' creates an authored operator erasure target', status == 200)
            operator_records[store] = str(created['id'])
            check(store + ' operator target reaches real derived retrieval', restored_search(
                dict(store=store, keywords=['needle'], limit=10), 'operator subject ' + store + ' ' + marker))
        private_author = gate.personal_sql('SELECT author_principal FROM user_memories WHERE id=' + operator_records['user'])
        # KB data-subject ownership is separate from the author of a shared fact.
        # Bind this synthetic target to the chosen subject before observing erasure.
        matrix.command('docker', 'exec', kb.postgres, 'psql', '-U', 'postgres',
            '-d', 'aimee_store', '-X', '-At', '-v', 'ON_ERROR_STOP=1', '-c',
            "UPDATE memories SET owner_principal='" + private_author.replace("'", "''") +
            "' WHERE id=" + operator_records['kb'])
        shared_author = matrix.command('docker', 'exec', kb.postgres, 'psql', '-U', 'postgres',
            '-d', 'aimee_store', '-X', '-At', '-c', 'SELECT owner_principal FROM memories WHERE id=' + operator_records['kb'])
        check('operator targets have verified private authorship and explicit shared subject ownership', bool(private_author) and bool(shared_author))
        unrelated_body = dict(store='user', key='other-actor-' + marker,
            content='needle unrelated operator canonical ' + marker, kind='fact')
        status, unrelated = json.loads(matrix.command('docker', 'exec', '-i', '-u', '1000',
            server.application, 'python3', '-c', placement.HTTP, data=json.dumps(dict(
                method='POST', path='/v1/memory/store', body=unrelated_body))))
        check('a distinct local principal authors an unrelated record', status == 200 and
            gate.personal_sql('SELECT author_principal FROM user_memories WHERE id=' + str(unrelated['id'])) != private_author)
        historical_name = next(row['name'] for row in datasets() if row['name'].startswith(prefixes[1])
            and '_' + operator_records['user'] + '_' in row['name'])
        owner_bearer = uuid.uuid4().hex + uuid.uuid4().hex
        matrix.command('docker', 'exec', '-i', '-u', '1000', '-e', 'AIMEE_VAULT_ENV_OVERWRITE=1',
            kb.application, 'aimee-kb', '--bootstrap-vault-stdin',
            data='AIMEE_KB_API_BEARER_TOKEN=' + owner_bearer + '\0')
        kb.compose('restart', 'aimee-kb')
        kb.start()
        for key, value in [('kb_mode', 'none'), ('kb_connection_string', ''),
                ('kb_service_identity_token', ''), ('kb_client_bearer_token', owner_bearer),
                ('kb_client_url', 'http://127.0.0.1:8747'), ('kb_mode', 'remote')]:
            status, result = gate.call('/v1/config/set', dict(key=key, value=value))
            check('operator transport config applies ' + key, status == 200 and result.get('status') == 'ok')
        matrix.command('docker', 'exec', '-u', '1000', server.application, 'python3', '-c',
            'from pathlib import Path\nfor p in Path("/var/lib/aimee").rglob("kb-client-identity.json"):\n p.rename(p.with_name("validation-retired-kb-client-identity.json"))')
        server.compose('restart', 'aimee-server')
        # This fixture's bridge binds only Server loopback and forwards inside
        # its disposable Docker network. No host/LAN listener is published.
        relay = '''import http.client
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
class Relay(BaseHTTPRequestHandler):
 def log_message(self,*args): pass
 def request(self):
  body=self.rfile.read(int(self.headers.get('Content-Length','0')))
  headers={k:v for k,v in self.headers.items() if k.lower() not in ('host','connection','content-length')}
  c=http.client.HTTPConnection('aimee-kb',8741,timeout=120)
  c.request(self.command,self.path,body,headers)
  r=c.getresponse();raw=r.read();self.send_response(r.status)
  self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)))
  self.end_headers();self.wfile.write(raw);c.close()
 do_GET=request
 do_POST=request
ThreadingHTTPServer(('127.0.0.1',8747),Relay).serve_forever()
'''
        matrix.command('docker', 'exec', '-d', '-u', '1000', server.application,
            'python3', '-c', relay)
        server.start()
        status, health = gate.call('/v1/kb/health', method='GET')
        check('explicit operator transport authenticates to KB', status == 200)
        erasure = dict(subject=private_author, request_id='operator-cognee-' + uuid.uuid4().hex)
        Proxy.blocked = True
        status, refused = call('/v1/kb/erase-subject', erasure)
        check('managed subject erasure cannot certify a Cognee provider outage',
            refused.get('status') == 'error' and refused.get('coverage_complete') is not True)
        Proxy.blocked = False
        status, completed = call('/v1/kb/erase-subject', erasure)
        check('same managed subject request retries to verified completion',
            status == 200 and completed.get('coverage_complete') is True)
        status, repeated = call('/v1/kb/erase-subject', erasure)
        check('managed subject completion retry is idempotent',
            status == 200 and repeated.get('coverage_complete') is True)
        if shared_author != private_author:
            status, completed = call('/v1/kb/erase-subject', dict(subject=shared_author,
                request_id='operator-shared-' + uuid.uuid4().hex))
            check('shared canonical author erasure also reaches verified completion',
                status == 200 and completed.get('coverage_complete') is True)
        check('managed erasure physically removes the personal target and history',
            gate.personal_sql('SELECT (SELECT count(*) FROM user_memories WHERE id=' + operator_records['user'] +
                ')+(SELECT count(*) FROM user_memory_versions WHERE memory_id=' + operator_records['user'] + ')') == '0')
        check('managed erasure physically removes the shared target', matrix.command(
            'docker', 'exec', kb.postgres, 'psql', '-U', 'postgres', '-d', 'aimee_store', '-X', '-At', '-c',
            'SELECT count(*) FROM memories WHERE id=' + operator_records['kb']) == '0')
        check('managed completion verifies both real derived namespaces absent', not datasets())
        status, current = call('get', dict(store='user', id=str(unrelated['id'])))
        check('managed erasure preserves the other principal canonical record',
            status == 200 and 'unrelated operator canonical ' + marker in json.dumps(current))
        boundary = 'operator-restored-provider-state'
        body = (f'--{boundary}\r\nContent-Disposition: form-data; name="datasetName"\r\n\r\n{historical_name}\r\n'
            f'--{boundary}\r\nContent-Disposition: form-data; name="data"; filename="restored.txt"\r\n'
            f'Content-Type: text/plain\r\n\r\nneedle erased operator fixture\r\n--{boundary}--\r\n').encode()
        provider_request('/api/v1/add', body, 'multipart/form-data; boundary=' + boundary)
        check('fixture restores an actually erased subject dataset', any(row['name'] == historical_name for row in datasets()))
        matrix.command('docker', 'exec', server.application, 'python3', '-c', code)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline and any(row['name'] == historical_name for row in datasets()):
            time.sleep(.5)
        check('memory restart removes restored subject-erased derived data',
            not any(row['name'] == historical_name for row in datasets()))
        check('memory restart cannot resurrect physically erased personal data',
            gate.personal_sql('SELECT count(*) FROM user_memories WHERE id=' + operator_records['user']) == '0')
        check('other principal reindexes after managed erasure replay', restored_search(
            dict(store='user', keywords=['needle'], limit=10), 'unrelated operator canonical ' + marker))
        check('real Cognee exercised local completion and embedding models', all(fixture.ModelFixture.calls.values()))
        (output / 'model-calls.json').write_text(json.dumps(fixture.ModelFixture.calls, indent=2) + '\n')
    except Exception as error:
        checks.append(dict(name='published Cognee gate completed', passed=False, error=type(error).__name__))
        print('FAIL Cognee image gate: ' + str(error), flush=True)
    finally:
        Proxy.blocked = False
        (output / 'cognee-image.json').write_text(json.dumps(checks, indent=2) + '\n')
        for stack in reversed(stacks):
            try:
                logs = subprocess.run(['docker', 'logs', stack.application], capture_output=True, text=True)
                (private / (stack.role + '.log')).write_text(logs.stdout + logs.stderr)
                if not args.keep:
                    stack.compose('down', '--volumes', '--remove-orphans')
            except Exception:
                print('Private test stack retained after cleanup failure', flush=True)
        if provider is not None:
            provider.terminate()
            try:
                provider.wait(timeout=15)
            except subprocess.TimeoutExpired:
                provider.kill()
                provider.wait()
        provider_log.close()
        proxy.shutdown()
        model.shutdown()
    return 0 if checks and all(row['passed'] for row in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
