#!/usr/bin/env python3
"""Upgrade the published 0.4.1 KB through shipped Compose, then prove rollback.

Run as root on a disposable Docker Linux VM with LUKS devices. Candidate images
are AIMEE_APPLICATION_IMAGE, AIMEE_POSTGRES_IMAGE and AIMEE_EMBEDDER_IMAGE.
Only this test's containers, copied homes and volumes are removed. Evidence
contains verdicts only; credential-bearing Docker diagnostics are not emitted.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('matrix', ROOT / 'tests/e2e/deployment-matrix.py')
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)


def digest_tree(root):
    digest = hashlib.sha256()
    for path in sorted(root.rglob('*')):
        if path.is_file():
            digest.update(str(path.relative_to(root)).encode() + b'\0')
            with path.open('rb') as stream:
                while block := stream.read(1024 * 1024):
                    digest.update(block)
    return digest.hexdigest()


def request(container, token, path, body=None):
    code = """import http.client,json,sys
a=json.load(sys.stdin);c=http.client.HTTPConnection('127.0.0.1',8741,timeout=30)
c.request('GET' if a['body'] is None else 'POST',a['path'],None if a['body'] is None else json.dumps(a['body']),{'Content-Type':'application/json','Authorization':'Bearer '+a['token']})
r=c.getresponse();print(json.dumps([r.status,json.loads(r.read())]))
"""
    return json.loads(matrix.command('docker', 'exec', '-i', container, 'python3', '-c', code,
        data=json.dumps(dict(token=token, path=path, body=body))))


def wait_legacy(container, token):
    for _ in range(180):
        try:
            status, body = request(container, token, '/v1/health')
            if status == 200 and body.get('db2_ok') and body.get('db2_kb_tables_ok'):
                return
        except (RuntimeError, ValueError):
            pass
        time.sleep(2)
    raise RuntimeError('published 0.4.1 KB did not become healthy')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--keep', action='store_true', help='retain private fixture and containers for diagnosis')
    parser.add_argument('--legacy-image', default='ghcr.io/rakuensoftware/aimee-kb-a25m@sha256:13790bc5ec075cfdb25e9dc3c7719706e25da7445c8febbb988eb0aae5585f46')
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('run as root in a disposable Docker VM')
    for name in ('AIMEE_APPLICATION_IMAGE', 'AIMEE_POSTGRES_IMAGE', 'AIMEE_EMBEDDER_IMAGE'):
        if not os.environ.get(name):
            parser.error(name + ' must name the candidate image')
    args.output.mkdir(parents=True, exist_ok=True)
    checks = []
    stack = None
    root = Path(tempfile.mkdtemp(prefix='aimee-kb-upgrade-'))
    legacy = root.name + '-legacy'
    token = 'scope:service:aimee-server:' + secrets.token_hex(32)

    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)
        if not passed:
            raise RuntimeError(name)

    try:
        home = root / 'legacy-home'
        home.mkdir()
        os.chown(home, 999, 999)
        env_file = root / 'legacy.env'
        env_file.write_text('AIMEE_HOME=/var/lib/aimee\nEMBEDDER_MODEL=bekko-a25m\nAIMEE_KB_API_BEARER_TOKEN=' + token + '\n')
        env_file.chmod(0o600)
        matrix.command('docker', 'run', '-d', '--name', legacy, '--ulimit', 'stack=67108864:67108864',
            '--env-file', str(env_file), '-v', str(home) + ':/var/lib/aimee', args.legacy_image, '--http-port=8741')
        wait_legacy(legacy, token)
        fixtures = []
        for scope in ('global', 'project'):
            body = dict(key='upgrade-canary-' + scope, content='Published legacy ' + scope + ' canary 🦊', scope=scope)
            if scope == 'project':
                body['project'] = 'release-upgrade'
            status, result = request(legacy, token, '/v1/actions/memory.store', body)
            check('published 0.4.1 writes ' + scope + ' canary', status == 200 and result.get('id'))
            fixtures.append(dict(id=result['id'], content=body['content'], scope=scope))
        matrix.command('docker', 'stop', '--time', '60', legacy)
        check('published cluster uses documented direct layout', (home / 'postgres/PG_VERSION').read_text().strip() == '18')
        original = digest_tree(home / 'postgres')
        copied = root / 'upgraded-home'
        shutil.copytree(home, copied, symlinks=True)
        for path in (copied, *copied.rglob('*')):
            if not path.is_symlink():
                os.chown(path, 1000, 1000)
        env = dict(os.environ, AIMEE_RUNTIME_WEB_ENABLED='0', AIMEE_POSTGRES_VOLUME_MIB='2048',
                   COMPOSE_PROFILES='', EMBEDDER_MODEL='bekko-a25m',
                   EMBEDDER_URL='https://aimee-embedder:8762', EMBEDDER_DIMS='384')
        env['AIMEE_DEVICE_MAPPER_MAJOR'] = next(line.split()[0] for line in Path('/proc/devices').read_text().splitlines()
                                               if line.split()[-1:] == ['device-mapper'])
        stack = matrix.Stack('kb', env, args.output)
        # Preserve the operator's existing enrollment settings as documented.
        stack.env['AIMEE_KB_API_BEARER_TOKEN'] = token
        stack.network_override.write_text('services:\n  aimee-kb:\n    ports: !reset []\n    volumes:\n      - ' +
            str(copied) + ':/var/lib/aimee\n  aimee-store-db:\n    volumes:\n      - ' +
            str(home / 'postgres') + ':/mnt/aimee-postgres-legacy:ro\n')
        matrix.command('python3', str(ROOT / 'scripts/compose-vault-init.py'), '--migrate-store-connections',
                       *stack.compose_args(), 'up', env=stack.env)
        if args.keep:
            (root / 'state.json').write_text(json.dumps(dict(project=stack.project, application=stack.application, postgres=stack.postgres, env=stack.env, token=token, override=str(stack.network_override))))
            print('Private fixture: ' + str(root), flush=True)
        stack.start()
        check('shipped upgrade procedure reaches healthy KB', True)
        check('long-lived application metadata contains no credentials', matrix.application_metadata_is_private(stack))
        for fixture in fixtures:
            status, body = request(stack.application, token, '/v1/actions/memory.get', dict(id=fixture['id'], scope='all'))
            check('upgrade retains ' + fixture['scope'] + ' canary and original authority',
                  status == 200 and body.get('memory', {}).get('content') == fixture['content'])
        status, body = request(stack.application, token, '/v1/actions/memory.store',
                               dict(key='upgrade-new-write', content='Written after encrypted migration 🦊'))
        check('upgraded KB accepts new writes', status == 200 and body.get('id'))
        new_id = body.get('id')
        stack.compose('down')
        stack.start()
        status, body = request(stack.application, token, '/v1/actions/memory.get', dict(id=new_id, scope='all'))
        check('container recreation preserves post-upgrade writes', status == 200 and
              body.get('memory', {}).get('content') == 'Written after encrypted migration 🦊')
        check('migration and recreation leave original cluster byte-for-byte intact', digest_tree(home / 'postgres') == original)
        stack.compose('stop')
        matrix.command('docker', 'start', legacy)
        wait_legacy(legacy, token)
        for fixture in fixtures:
            status, body = request(legacy, token, '/v1/actions/memory.get', dict(id=fixture['id'], scope='all'))
            check('original 0.4.1 rollback reads ' + fixture['scope'] + ' canary',
                  status == 200 and body.get('memory', {}).get('content') == fixture['content'])
    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError):
        checks.append(dict(name='published KB upgrade completed', passed=False))
        print('FAIL published KB upgrade completed', flush=True)
    finally:
        (args.output / 'results.json').write_text(json.dumps(checks, indent=2) + '\n')
        if stack and not args.keep:
            stack.compose('down', '--volumes', '--remove-orphans')
        if not args.keep:
            subprocess.run(['docker', 'rm', '-f', legacy], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            shutil.rmtree(root)
    return 0 if checks and all(check['passed'] for check in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
