#!/usr/bin/env python3
"""Validate private indexing in a disposable aimee-pairing-e2e-* Compose stack.

Creates repository fixtures, restarts services, temporarily stops PostgreSQL,
changes the instance fusion setting, and restores it. Never target a user stack.
Run after client-pairing-e2e.py to also check its persisted personal memories.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time


def run(*args, data=None):
    p = subprocess.run(args, input=data, text=True, capture_output=True, timeout=180)
    if p.returncode:
        raise RuntimeError('fixture command failed: ' + ' '.join(args[:3]))
    return p.stdout


def check(name, condition):
    if not condition:
        raise AssertionError(name)
    print('PASS ' + name, flush=True)


def eventually(name, fn, timeout=100):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            value = fn()
            if value:
                print('PASS ' + name, flush=True)
                return value
        except (RuntimeError, ValueError, KeyError):
            pass
        time.sleep(2)
    raise AssertionError(name)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--server', required=True)
    ap.add_argument('--store-db', required=True)
    ap.add_argument('--compose-dir', type=Path, required=True)
    args = ap.parse_args()
    for container in (args.server, args.store_db):
        project = run('docker', 'inspect', '--format', '{{index .Config.Labels "com.docker.compose.project"}}', container).strip()
        if not project.startswith('aimee-pairing-e2e-'):
            ap.error('refusing a non-disposable stack')
    model = json.loads(run('docker', 'compose', '--project-directory', str(args.compose_dir), 'config', '--format', 'json'))
    if model['name'] != project:
        ap.error('compose directory does not match the disposable stack')

    def api(path, body=None):
        argv = ['docker', 'exec', args.server, 'curl', '-sS', '--max-time', '30', '--unix-socket',
                '/var/lib/aimee/aimee-http.sock', '-H', 'Content-Type: application/json']
        if body is not None:
            argv += ['-d', json.dumps(body)]
        return json.loads(run(*argv, 'http://localhost/v1/' + path))

    def ready():
        return api('ready').get('ready')

    def stats():
        return api('dashboard/memory_stats')['data']

    def query(kind, **kw):
        return api('index/' + kind, dict(project='index-fixture', **kw))

    def hybrid():
        return query('hybrid', query='quasar_validation')['results'][0]['result']

    def fixture(script):
        return run('docker', 'exec', '-i', '-u', 'aimee', args.server, 'bash', '-s', data='set -eu\n' + script)

    eventually('standalone server ready', ready)
    baseline = stats()['total']
    dash = api('dashboard/all')
    steps = {s['step']: s for s in dash['onboard']['steps']}
    check('database readiness reflects connectivity', steps['database']['status'] == 'ok')
    check('unused delegations are idle', steps['delegations']['status'] == 'skipped')
    check('local retrieval is active without KB', api('ready')['dependencies']['kb'] == 'disabled' and api('ready')['dependencies']['retrieval'] == 'ok')
    fixture('''
p=/var/lib/aimee-workspaces/environment/index-fixture
mkdir -p "$p"
cd "$p"
git init -q -b main
git config user.email fixture@example.invalid
git config user.name Fixture
git remote add origin https://example.invalid/index-fixture.git
cat >engine.c <<'EOF'
/* quasar_validation entry point */
int engine_start(void) { return 42; }
EOF
cat >caller.c <<'EOF'
int fixture_caller(void) { return engine_start(); }
EOF
cat >unrelated.c <<'EOF'
int unrelated_moon(void) { return 0; }
EOF
git add .
git commit -qm fixture
git update-ref refs/remotes/origin/main HEAD
git symbolic-ref refs/remotes/origin/HEAD refs/remotes/origin/main
''')
    run('docker', 'restart', args.server)
    eventually('restart restores readiness', ready)
    eventually('existing repository automatically indexed and embedded', lambda: stats().get('code_index', {}).get('embeddings') == 3)
    check('repository indexing does not invent personal memories', stats()['total'] == baseline)
    check('project inventory includes discovered clone', any(p['name'] == 'index-fixture' for p in api('index/list', {})['projects']))
    check('definition lookup', any(h['file_path'] == 'engine.c' for h in query('find', identifier='engine_start')['hits']))
    check('caller lookup', any(h['file_path'] == 'caller.c' for h in query('find_callers', symbol='engine_start')['hits']))
    result = hybrid()
    check('fusion defaults on', result['graph_code_fusion_state'] == 'on')
    check('fusion includes related code', any(h['file_path'] == 'caller.c' and h['source'] == 'graph' for h in result['results']))
    evidence = query('investigate', query='quasar_validation')['results'][0]['result']['results']
    check('investigation attaches source code and generation', any('quasar_validation' in h.get('code', '') and h.get('generation', 0) > 0 for h in evidence))
    check('blast radius includes callers', 'caller.c' in query('blast_radius', file_path='engine.c')['dependents'])
    check('file structure retains definitions', bool(query('structure', file_path='engine.c').get('definitions')))

    fixture('''
cd /var/lib/aimee-workspaces/environment/index-fixture
rm unrelated.c
printf '\\nint added_sun(void) { return 1; }\\n' >> engine.c
git add -A
git commit -qm update
git update-ref refs/remotes/origin/main HEAD
''')
    eventually('default-branch changes automatically indexed', lambda: bool(query('find', identifier='added_sun').get('hits')))
    check('removed files disappear from index', not query('find', identifier='unrelated_moon')['hits'])
    eventually('updated code is re-embedded', lambda: stats()['code_index']['files'] == 2 and stats()['code_index']['embeddings'] == 2)

    run('docker', 'stop', args.store_db)
    try:
        eventually('database outage fails readiness', lambda: api('ready').get('ready') is False)
        eventually('dashboard reports database failure', lambda: any(s['step'] == 'database' and s['status'] == 'error' for s in api('dashboard/all')['onboard']['steps']))
    finally:
        run('docker', 'start', args.store_db)
    eventually('database recovery restores readiness', ready)

    env_path = args.compose_dir / '.env'
    original = env_path.read_text()
    def recreate():
        run('docker', 'compose', '--project-directory', str(args.compose_dir), 'up', '-d', '--no-build', '--no-deps', '--force-recreate', 'aimee-server')
        eventually('instance restart ready', ready)
    try:
        env_path.write_text(original + '\nAIMEE_GRAPH_FUSION=off\n')
        recreate()
        result = hybrid()
        check('instance off setting is honored', result['graph_code_fusion_state'] == 'off' and stats()['graph_fusion_enabled'] is False)
        check('off retains direct retrieval without graph expansion', any(h['file_path'] == 'engine.c' for h in result['results']) and all(h['source'] != 'graph' for h in result['results']))
        run('docker', 'restart', args.server)
        eventually('off policy survives restart', ready)
        check('off is still active after restart', hybrid()['graph_code_fusion_state'] == 'off')
    finally:
        env_path.write_text(original)
        recreate()
    check('default on restored independently of clients', hybrid()['graph_code_fusion_state'] == 'on')
    print('PASS standalone private indexing end-to-end', flush=True)


if __name__ == '__main__':
    main()
