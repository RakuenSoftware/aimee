#!/usr/bin/env python3
"""Require the installed Vault helper to work under egress process hardening."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import json
import os
from pathlib import Path
import secrets
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('matrix', ROOT / 'tests/e2e/deployment-matrix.py')
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    checks, stacks = [], []
    state = dict(token=secrets.token_hex(32), accepted=0)

    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            allowed = self.path == '/api/v1/datasets' and self.headers.get('Authorization') == 'Bearer ' + state['token']
            if allowed:
                state['accepted'] += 1
            body = b'[]' if allowed else b'{}'
            self.send_response(200 if allowed else 401)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    bridge = json.loads(matrix.command('docker', 'network', 'inspect', 'bridge'))[0]
    gateway = bridge['IPAM']['Config'][0]['Gateway']
    provider = ThreadingHTTPServer((gateway, 0), Provider)
    threading.Thread(target=provider.serve_forever, daemon=True).start()
    endpoint = 'http://' + gateway + ':' + str(provider.server_port)

    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)
        if not passed:
            raise RuntimeError(name)

    def children(stack):
        code = '''import json
from pathlib import Path
rows={}
for p in Path('/proc').iterdir():
 if not p.name.isdigit(): continue
 try:
  a=(p/'cmdline').read_bytes().split(b'\\0')
  if Path(a[0].decode()).name=='aimee-module-egress': rows['egress']=int(p.name)
  if b'--egress-vault-resource' in a: rows['helper']=int(p.name)
  if Path(a[0].decode()).name=='aimee-module-memory': rows['memory']=int(p.name)
 except (OSError,UnicodeError): pass
print(json.dumps(rows))
'''
        return json.loads(matrix.command('docker', 'exec', stack.application, 'python3', '-c', code))

    def kill(stack, pid):
        matrix.command('docker', 'exec', stack.application, 'python3', '-c',
                       'import os,signal; os.kill(' + str(pid) + ',signal.SIGTERM)')

    def wait(stack, old=None):
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            rows = children(stack)
            if len(rows) == 3 and (not old or all(rows[key] != old[key] for key in ('egress', 'helper'))):
                return rows
            time.sleep(.2)
        raise RuntimeError('supervisor did not establish a new credential owner')

    try:
        env = dict(os.environ, AIMEE_RUNTIME_WEB_ENABLED='0', COMPOSE_PROFILES='',
                   EMBEDDER_MODEL='bekko-a25m', EMBEDDER_DIMS='384',
                   EMBEDDER_URL='https://aimee-embedder:8762')
        for role in ('kb', 'server'):
            stack = matrix.Stack(role, env, args.output)
            stacks.append(stack)
            stack.network_override.write_text('services:\n  aimee-' + role + ':\n    ports: !reset []\n'
                '    environment:\n      AIMEE_MEMORY_BACKEND: cognee\n'
                '      AIMEE_MEMORY_BACKEND_URL: ' + json.dumps(endpoint) + '\n')
            matrix.command('python3', str(ROOT / 'scripts/compose-vault-init.py'),
                           *stack.compose_args(), 'up', env=stack.env)
            matrix.command('docker', 'compose', *stack.compose_args(), 'run', '--rm', '--no-deps', '-T',
                '--entrypoint', '/usr/sbin/runuser', 'aimee-' + role, '-u', 'aimee', '--', 'aimee-' + role,
                '--bootstrap-vault-stdin', env=stack.env, data='AIMEE_MEMORY_BACKEND_TOKEN=' + state['token'] + '\0')
            before = state['accepted']
            stack.start()
            check(role + ' memory boots through real Vault bearer handoff', state['accepted'] > before)
            check(role + ' container metadata excludes credentials', matrix.application_metadata_is_private(stack))
            rows = wait(stack)
            for key in ('egress', 'helper'):
                result = subprocess.run(['docker', 'exec', '-u', '1000', stack.application,
                    'readlink', '/proc/' + str(rows[key]) + '/exe'], capture_output=True)
                check(role + ' ' + key + ' stays non-dumpable', result.returncode != 0)
            result = subprocess.run(['docker', 'exec', '-u', '1000', stack.application,
                'aimee-server', '--egress-vault-resource'], capture_output=True, timeout=10)
            check(role + ' unattested helper invocation refuses readiness', result.returncode != 0 and not result.stdout)
            state['token'] = secrets.token_hex(32)
            matrix.command('docker', 'exec', '-i', '-u', '1000', '-e', 'AIMEE_VAULT_ENV_OVERWRITE=1',
                stack.application, 'aimee-' + role, '--bootstrap-vault-stdin',
                data='AIMEE_MEMORY_BACKEND_TOKEN=' + state['token'] + '\0')
            before = state['accepted']
            kill(stack, rows['memory'])
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline and state['accepted'] == before:
                time.sleep(.2)
            check(role + ' existing pipe observes Vault credential rotation', state['accepted'] > before)
            check(role + ' rotation retains the hardened credential owner', children(stack).get('egress') == rows['egress'])
            kill(stack, rows['helper'])
            renewed = wait(stack, rows)
            before = state['accepted']
            kill(stack, renewed['memory'])
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline and state['accepted'] == before:
                time.sleep(.2)
            check(role + ' helper failure restarts owner and restores credential handoff', state['accepted'] > before)
            stack.start()
            check(role + ' recovered composition is healthy', True)
    except Exception as error:
        checks.append(dict(name='installed credential handoff gate completed', passed=False,
                           error=type(error).__name__))
        print('FAIL installed credential handoff: ' + str(error), flush=True)
    finally:
        (args.output / 'egress-vault.json').write_text(json.dumps(checks, indent=2) + '\n')
        for stack in reversed(stacks):
            stack.compose('down', '--volumes', '--remove-orphans')
        provider.shutdown()
        provider.server_close()
    return 0 if checks and all(row['passed'] for row in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
