#!/usr/bin/env python3
"""Qualify retained standalone code indexing and real SQL outage readiness.

Runs only against an owned aimee-e2e-server-* stack, or creates its own stack.
Fixture source changes and dependency restarts are confined to that project.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tests/e2e' / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--server')
    parser.add_argument('--store-db')
    parser.add_argument('--keep', action='store_true', help='retain the owned stack for isolated diagnosis')
    args = parser.parse_args()
    if bool(args.server) != bool(args.store_db):
        parser.error('supply both owned containers or neither')
    args.output.mkdir(parents=True, exist_ok=True)
    matrix = load('matrix', 'deployment-matrix.py')
    placement = load('placement', 'memory-placement-e2e.py')
    owned = None
    if not args.server:
        env = dict(os.environ, AIMEE_RUNTIME_WEB_ENABLED='0', COMPOSE_PROFILES='',
                   EMBEDDER_MODEL='bekko-a25m', EMBEDDER_DIMS='384',
                   EMBEDDER_URL='https://aimee-embedder:8762')
        owned = matrix.Stack('server', env, args.output)
        args.server, args.store_db = owned.application, owned.postgres
    checks, observations = [], []
    project = 'release-code-' + uuid.uuid4().hex[:10]
    root = '/var/lib/aimee-workspaces/environment/' + project
    first = '/* heliotrope_release_probe 界 🦊 */\nint release_engine(void) { return 42; }\n'
    second = '/* changed committed source 界 🦊 */\nint release_changed(void) { return 7; }\n'

    def check(name, passed, **details):
        checks.append(dict(name=name, passed=bool(passed), **details))
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)
        if not passed:
            raise RuntimeError(name)

    def api(path, body=None, method='POST'):
        return json.loads(matrix.command('docker', 'exec', '-i', '-u', '1000', args.server,
            'python3', '-c', placement.HTTP,
            data=json.dumps(dict(path=path, body=body or {}, method=method)), timeout=90))

    def eventually(name, fn, seconds=180):
        start = time.monotonic()
        while time.monotonic() - start < seconds:
            try:
                if fn():
                    check(name, True, seconds=round(time.monotonic() - start, 2))
                    return
            except (RuntimeError, KeyError, TypeError, ValueError):
                pass
            time.sleep(2)
        check(name, False, seconds=round(time.monotonic() - start, 2))

    def query(op, **values):
        status, body = api('/v1/index/' + op, dict(project=project, **values))
        if status != 200 or body.get('status') in ('error', 'unavailable'):
            return {}
        return body

    def fixture(code):
        # Paths are generated here, never supplied by a remote caller.
        matrix.command('docker', 'exec', '-i', '-u', '1000', args.server, 'python3', '-', data=code)

    def control_memory():
        return api('/v1/memory/get', dict(id=str(memory_id)))[1].get('memory')

    def historical_source_count():
        # Read-only SQL in the owned database; its own credential stays inside it.
        code = '''import os,subprocess
import json,sys
p=json.load(sys.stdin)
env=dict(os.environ,PGPASSWORD=os.environ['AIMEE_STORE_RUNTIME_PASSWORD'],PGSSLMODE='require')
r=subprocess.run(['psql','-h','127.0.0.1','-U','aimee_store_runtime','-d','aimee_store','-X','-At','-v','ON_ERROR_STOP=1','-c',"BEGIN READ ONLY; SELECT count(*) FROM server_code_file_versions WHERE project='"+p['project']+"' AND fingerprint=encode(sha256(convert_to('"+p['content'].replace("'","''")+"','UTF8')),'hex'); COMMIT;"],env=env,capture_output=True,text=True)
if r.returncode:raise SystemExit(1)
print(json.dumps([int(x) for x in r.stdout.splitlines() if x.isdigit()]))
'''
        values = json.loads(matrix.command('docker', 'exec', '-i', args.store_db, 'python3', '-c',
                            code, data=json.dumps(dict(project=project, content=first))))
        return values[0]

    try:
        if owned:
            owned.start()
        for container in (args.server, args.store_db):
            label = matrix.command('docker', 'inspect', '--format',
                    '{{index .Config.Labels "com.docker.compose.project"}}', container)
            check('owned disposable container ' + ('server' if container == args.server else 'postgres'),
                  label.startswith('aimee-e2e-server-'))
            if container == args.server:
                expected_label = label
            else:
                check('both containers belong to one project', label == expected_label)
        check('standalone optional KB remains disabled', api('/v1/ready', method='GET')[1].get('dependencies', {}).get('kb') == 'disabled')
        status, stored = api('/v1/memory/store', dict(key=project, content='Unrelated retained memory control 界 🦊', idempotency_key=uuid.uuid4().hex))
        check('independent memory control stored', status == 200 and stored.get('status') == 'ok')
        memory_id = stored['id']
        memory_before = control_memory()
        fixture('''from pathlib import Path
import subprocess
p=Path(''' + repr(root) + ''');p.mkdir(parents=True)
def git(*a):subprocess.run(['git','-C',str(p),*a],check=True,capture_output=True)
git('init','-q','-b','main');git('config','user.name','ReleaseFixture');git('config','user.email','release-fixture@example.invalid')
git('remote','add','origin','https://example.invalid/''' + project + '''.git')
(p/'engine.c').write_text(''' + repr(first) + ''')
(p/'caller.c').write_text(''' + repr('int release_engine(void);\nint release_caller(void) { return release_engine(); }\n') + ''')
git('add','.');git('commit','-qm','fixture');git('update-ref','refs/remotes/origin/main','HEAD');git('symbolic-ref','refs/remotes/origin/HEAD','refs/remotes/origin/main')
''')
        # Discover an already published clone on the first pass after startup.
        matrix.command('docker', 'restart', args.server)
        eventually('application returns ready', lambda: api('/v1/ready', method='GET')[1].get('ready'), 180)
        eventually('published clone automatically indexes a real definition',
                   lambda: any(h.get('file_path') == 'engine.c' for h in query('find', identifier='release_engine').get('hits', [])))
        check('project inventory lists the clone', any(p.get('name') == project for p in api('/v1/index/list', {})[1].get('projects', [])))
        check('caller lookup follows extracted calls', any(h.get('file_path') == 'caller.c' for h in query('find_callers', symbol='release_engine').get('hits', [])))
        check('structure carries extracted definitions', bool(query('structure', file_path='engine.c').get('definitions')))
        check('blast radius follows the caller', 'caller.c' in query('blast_radius', file_path='engine.c').get('dependents', []))
        span = query('span', file_path='engine.c', line_start=1, line_end=2)
        check('published span retains exact Unicode source', span.get('span', {}).get('content') == first)
        hybrid = query('hybrid', query='heliotrope_release_probe', graph_code_fusion_state='off')
        result = hybrid.get('results', [{}])[0].get('result', {})
        check('hybrid uses the actual embedding provider', result.get('dense_state') == 'query_executed')
        check('hybrid resolves source and instance graph policy', result.get('graph_code_fusion_state') == 'on' and any(h.get('file_path') == 'engine.c' for h in result.get('results', result.get('hits', []))))
        check('complete embedding generation is ready', bool(result.get('index_generations')) and all(g.get('readiness') == 'ready' for g in result['index_generations']))
        investigation = query('investigate', query='heliotrope_release_probe')
        entries = investigation.get('results', [{}])[0].get('result', {}).get('results', [])
        check('investigation includes actual source and version', any('heliotrope_release_probe' in h.get('code', '') and h.get('generation', 0) > 0 for h in entries))
        before = historical_source_count()
        check('initial committed snapshot retained', before >= 1)
        fixture('''from pathlib import Path
import subprocess
p=Path(''' + repr(root) + ''');(p/'engine.c').write_text(''' + repr(second) + ''')
for a in [('add','.'),('commit','-qm','changed source'),('update-ref','refs/remotes/origin/main','HEAD')]:subprocess.run(['git','-C',str(p),*a],check=True,capture_output=True)
''')
        eventually('changed default branch automatically republishes', lambda: bool(query('find', identifier='release_changed').get('hits')))
        check('superseded symbol unavailable in current definition index', not query('find', identifier='release_engine').get('hits'))
        check('superseded original code bytes remain in SQL history', historical_source_count() >= before)
        check('code publication never changes personal memory', control_memory() == memory_before)
        matrix.command('docker', 'stop', args.store_db)
        try:
            status, failed = api('/v1/sessions/create', dict(client_type='index-readiness-outage'))
            check('SQL outage refuses actual canonical session creation', status >= 500 and failed.get('status') == 'error')
            eventually('readiness reports actual SQL dependency failure', lambda: api('/v1/ready', method='GET')[1].get('dependencies', {}).get('db1') == 'fail', 45)
            status, ready = api('/v1/ready', method='GET')
            check('SQL outage returns HTTP 503 and ready false', status == 503 and ready.get('ready') is False)
            observations.append(matrix.safe_readiness_payload('server', status, ready))
            check('independent native memory stays available', control_memory() == memory_before)
        finally:
            matrix.command('docker', 'start', args.store_db)
        eventually('SQL restoration returns readiness', lambda: api('/v1/ready', method='GET')[1].get('ready'), 150)
        status, session = api('/v1/sessions/create', dict(client_type='index-readiness-recovered'))
        check('canonical session creation recovers', status == 200 and session.get('status') == 'ok')
        check('current index recovers after SQL outage', bool(query('find', identifier='release_changed').get('hits')))
        check('original source history survives SQL recovery', historical_source_count() >= before)
    except (RuntimeError, subprocess.SubprocessError, ValueError, KeyError, TypeError, OSError) as error:
        checks.append(dict(name='standalone index qualification completed', passed=False, error=type(error).__name__))
        print('FAIL standalone index qualification: ' + str(error), flush=True)
    finally:
        (args.output / 'standalone-index.json').write_text(json.dumps(dict(checks=checks, observations=observations), indent=2) + '\n')
        if owned and not args.keep:
            owned.compose('down', '--volumes', '--remove-orphans')
    return 0 if checks and all(row['passed'] for row in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
