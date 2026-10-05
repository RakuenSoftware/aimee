#!/usr/bin/env python3
"""Exercise published-image Cognee wiring, canonical authority and derived erasure."""
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
    fixture = load('contract', 'scripts/validation/memory/run-cognee-contract.py')
    model = ThreadingHTTPServer(('127.0.0.1', 0), fixture.ModelFixture)
    threading.Thread(target=model.serve_forever, daemon=True).start()
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        api_port = reservation.getsockname()[1]
    api_url = 'http://127.0.0.1:' + str(api_port)

    class Proxy(BaseHTTPRequestHandler):
        blocked = False

        def log_message(self, *args):
            pass

        def request(self):
            if self.blocked:
                self.send_response(503)
                self.end_headers()
                self.wfile.write(b'{"detail":"injected disposable provider outage"}')
                return
            body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
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
                '      AIMEE_MEMORY_BACKEND_URL: ' + json.dumps(endpoint) + '\n')
            matrix.command('python3', str(ROOT / 'scripts/compose-vault-init.py'),
                *stack.compose_args(), 'up', env=stack.env)
            binary = 'aimee-' + role
            matrix.command('docker', 'compose', *stack.compose_args(), 'run', '--rm', '--no-deps', '-T',
                '--entrypoint', '/usr/sbin/runuser', binary, '-u', 'aimee', '--', binary,
                '--bootstrap-vault-stdin', env=stack.env, data='AIMEE_MEMORY_BACKEND_TOKEN=' + token + '\0')
            stack.start()
            check(role + ' with Cognee reaches published-image readiness', True)
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
        erasure = dict(subject='unretained-disposable-subject', request_id='published-cognee-' + uuid.uuid4().hex)
        Proxy.blocked = True
        _, refused = call('/v1/kb/erase-subject', erasure)
        check('provider transport outage blocks completion evidence',
              refused.get('coverage_complete') is not True and refused.get('status') == 'error')
        Proxy.blocked = False
        _, completed = call('/v1/kb/erase-subject', erasure)
        check('same erasure request retries to verified completion', completed.get('coverage_complete') is True)
        check('completion verifies both nodes derived datasets absent', not datasets())
        _, repeated = call('/v1/kb/erase-subject', erasure)
        check('completion retry is idempotent', repeated.get('coverage_complete') is True)
        for store, request in records.items():
            status, result = call('get', request)
            check(store + ' canonical record survives conservative derived reset', status == 200 and
                  store + ' ' + marker in json.dumps(result))
            status, result = call('search', dict(request, keywords=['needle'], limit=10))
            check(store + ' retained canonical data reindexes after reset', status == 200 and
                  store + ' ' + marker in json.dumps(result))
        author = gate.personal_sql('SELECT author_principal FROM user_memories WHERE id=' + records['user']['id'])
        check('private fixture has a verified canonical author', bool(author))
        _, erased = call('/v1/kb/erase-subject', dict(subject=author, request_id='actual-author-' + uuid.uuid4().hex))
        check('actual author erasure verifies managed-store coverage', erased.get('coverage_complete') is True)
        check('actual private canonical row is erased', gate.personal_sql(
            'SELECT count(*) FROM user_memories WHERE id=' + records['user']['id']) == '0')
        boundary = 'disposable-provider-restore'
        body = (f'--{boundary}\r\nContent-Disposition: form-data; name="datasetName"\r\n\r\n{old_name}\r\n'
                f'--{boundary}\r\nContent-Disposition: form-data; name="data"; filename="restored.txt"\r\n'
                f'Content-Type: text/plain\r\n\r\nneedle erased historical fixture\r\n--{boundary}--\r\n').encode()
        provider_request('/api/v1/add', body, 'multipart/form-data; boundary=' + boundary)
        check('fixture restores an erased derived dataset', any(row['name'] == old_name for row in datasets()))
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
        check('supervised memory restart removes restored erased derived state',
              not any(row['name'] == old_name for row in datasets()))
        check('restart cannot restore the erased canonical row', gate.personal_sql(
            'SELECT count(*) FROM user_memories WHERE id=' + records['user']['id']) == '0')
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
