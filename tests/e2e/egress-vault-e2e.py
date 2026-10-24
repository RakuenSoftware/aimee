#!/usr/bin/env python3
"""Require the installed Vault helper to work under egress process hardening."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import json
import os
import queue
import shutil
from pathlib import Path
import secrets
import subprocess
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('matrix', ROOT / 'tests/e2e/deployment-matrix.py')
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)


def provider_handler(state):
    """Bounded disposable SSE echo server; bearer values never enter responses."""
    frames = {}
    frames_lock = threading.Lock()
    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def authorized(self):
            allowed = self.headers.get('Authorization') == 'Bearer ' + state['token']
            state['accepted' if allowed else 'rejected'] += 1
            if not allowed:
                self.send_response(401)
                self.send_header('Content-Length', '0')
                self.end_headers()
            return allowed

        def do_GET(self):
            if self.path != '/sse':
                self.send_response(404)
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            if not self.authorized():
                return
            identity = uuid.uuid4().hex
            pending = queue.Queue(maxsize=1)
            with frames_lock:
                frames[identity] = pending
            try:
                self.send_response(200)
                self.send_header('Content-Type', 'text/event-stream')
                self.send_header('Cache-Control', 'no-cache')
                self.end_headers()
                self.wfile.write(('event: endpoint\ndata: /messages/' + identity + '\n\n').encode())
                self.wfile.flush()
                frame = pending.get(timeout=20)
                self.wfile.write(b'event: message\ndata: ' + frame + b'\n\n')
                self.wfile.flush()
            except (queue.Empty, BrokenPipeError, ConnectionResetError):
                pass
            finally:
                with frames_lock:
                    frames.pop(identity, None)

        def do_POST(self):
            if not self.authorized():
                return
            identity = self.path.removeprefix('/messages/')
            with frames_lock:
                pending = frames.get(identity)
            size = int(self.headers.get('Content-Length', '0'))
            if pending is None or not 0 < size <= 4096:
                self.send_response(400)
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            pending.put_nowait(self.rfile.read(size))
            self.send_response(202)
            self.send_header('Content-Length', '0')
            self.end_headers()

    return Provider


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    checks, stacks = [], []
    if not shutil.which('docker'):
        raise RuntimeError('local Docker engine required for the installed Vault handoff gate')
    state = dict(token=secrets.token_hex(32), accepted=0, rejected=0)
    issued_tokens = [state['token']]
    expected_rejections = 0
    fixture = args.output.resolve() / 'probe'
    fixture.mkdir(exist_ok=True)
    fixture.chmod(0o755)
    probe = fixture / 'egress-vault-probe'
    toolchain = shutil.which('go') or os.environ.get('GO')
    if not toolchain:
        raise RuntimeError('Go toolchain required to build the test-only MCP caller')
    # Compile the real SDK and MCP SSE transport, outside the application image.
    # The application still runs its installed, hardened egress and Vault helper.
    compiled = subprocess.run([toolchain, 'build', '-trimpath', '-o', str(probe),
        str(ROOT / 'tests/e2e/fixtures/egress-vault-probe/main.go')],
        cwd=ROOT / 'server-go', env=dict(os.environ, CGO_ENABLED='0'), capture_output=True, timeout=180)
    if compiled.returncode:
        raise RuntimeError('could not build test-only MCP caller')
    probe.chmod(0o755)

    # Production MCP correctly refuses loopback, private and documentation
    # addresses. Give an OWNED --internal Docker network an otherwise public
    # address shape, so the installed public-IP policy remains unchanged. This
    # network carries only our fixture and cannot route to the external Internet.
    network = 'aimee-egress-vault-' + uuid.uuid4().hex[:10]
    subnet = None
    for _ in range(16):
        candidate = '11.' + str(128 + secrets.randbelow(127)) + '.' + str(secrets.randbelow(256)) + '.0/29'
        made = subprocess.run(['docker', 'network', 'create', '--internal', '--subnet', candidate, network], capture_output=True)
        if made.returncode == 0:
            subnet = candidate
            break
    if subnet is None:
        raise RuntimeError('could not create owned internal MCP fixture network')
    gateway = subnet.split('/')[0].rsplit('.', 1)[0] + '.1'
    try:
        provider = ThreadingHTTPServer((gateway, 0), provider_handler(state))
    except Exception:
        matrix.command('docker', 'network', 'rm', network)
        raise
    provider.daemon_threads = True
    threading.Thread(target=provider.serve_forever, daemon=True).start()
    endpoint = 'http://' + gateway + ':' + str(provider.server_port) + '/sse'

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
            if len(rows) == 2 and (not old or all(rows[key] != old[key] for key in ('egress', 'helper'))):
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
                '    networks:\n      default: {}\n      store: {}\n      models: {}\n      vault-fixture: {}\n'
                '    volumes:\n      - ' + json.dumps(str(fixture) + ':/opt/aimee-e2e:ro') + '\n'
                'networks:\n  vault-fixture:\n    external: true\n    name: ' + network + '\n')
            matrix.command('python3', str(ROOT / 'scripts/compose-vault-init.py'),
                           *stack.compose_args(), 'up', env=stack.env)
            matrix.command('docker', 'compose', *stack.compose_args(), 'run', '--rm', '--no-deps', '-T',
                '--entrypoint', '/usr/sbin/runuser', 'aimee-' + role, '-u', 'aimee', '--', 'aimee-' + role,
                '--bootstrap-vault-stdin', env=stack.env, data='AIMEE_MCP_712_TOKEN=' + state['token'] + '\0')
            grant = '\n'.join(['version=1', 'principal_class=1', 'principal_ref=712', 'uid=self',
                'executable=/opt/aimee-e2e/egress-vault-probe', 'publish=', 'subscribe=',
                'request=12291-12294', 'serve=', ''])
            install = """import os,sys
from pathlib import Path
p=Path('/var/lib/aimee/modules.d')/sys.argv[1]/'egress-vault-probe.grant'
p.parent.mkdir(parents=True,exist_ok=True)
for directory in (p.parent.parent,p.parent):
 directory.chmod(0o700);os.chown(directory,1000,1000)
p.write_text(sys.stdin.read());p.chmod(0o600);os.chown(p,1000,1000)
"""
            matrix.command('docker', 'compose', *stack.compose_args(), 'run', '--rm', '--no-deps', '-T',
                '--entrypoint', 'python3', 'aimee-' + role, '-c', install, role, env=stack.env, data=grant)
            stack.start()

            def exchange(mode='exchange'):
                # Never include provider response/error bytes in raised errors.
                return matrix.command('docker', 'exec', '-u', '1000', stack.application,
                    '/opt/aimee-e2e/egress-vault-probe', '/var/lib/aimee/' + role + '-module-bus.sock',
                    endpoint, mode, timeout=30)

            before = state['accepted']
            check(role + ' MCP SSE uses real Vault bearer handoff', exchange() == 'ok' and state['accepted'] == before + 2)
            before = state['accepted']
            check(role + ' another MCP credential handle is refused before network',
                  exchange('foreign-handle') == 'denied' and state['accepted'] == before)
            # The provider refuses the valid Vault credential for one exchange.
            # Verify the failing client never copies transport-controlled bytes
            # or the credential to its output, then restore normal admission.
            actual_token = state['token']
            state['token'] = secrets.token_hex(32)
            try:
                refused = subprocess.run(['docker', 'exec', '-u', '1000', stack.application,
                    '/opt/aimee-e2e/egress-vault-probe', '/var/lib/aimee/' + role + '-module-bus.sock',
                    endpoint, 'exchange'], capture_output=True, timeout=30)
            finally:
                state['token'] = actual_token
            expected_rejections += 1
            check(role + ' failed provider authentication emits no credential',
                  refused.returncode != 0 and not refused.stdout and
                  refused.stderr == b'egress Vault probe failed\n' and
                  state['rejected'] == expected_rejections)
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
            issued_tokens.append(state['token'])
            matrix.command('docker', 'exec', '-i', '-u', '1000', '-e', 'AIMEE_VAULT_ENV_OVERWRITE=1',
                stack.application, 'aimee-' + role, '--bootstrap-vault-stdin',
                data='AIMEE_MCP_712_TOKEN=' + state['token'] + '\0')
            before = state['accepted']
            check(role + ' existing pipe observes Vault credential rotation',
                  exchange() == 'ok' and state['accepted'] == before + 2)
            check(role + ' rotation retains the hardened credential owner', children(stack).get('egress') == rows['egress'])
            kill(stack, rows['helper'])
            wait(stack, rows)
            before = state['accepted']
            check(role + ' helper failure restarts owner and restores credential handoff',
                  exchange() == 'ok' and state['accepted'] == before + 2)
            check(role + ' provider saw no unexpected stale or unscoped credential', state['rejected'] == expected_rejections)
            stack.start()
            check(role + ' recovered composition is healthy', True)
            logs = subprocess.run(['docker', 'logs', stack.application], capture_output=True, timeout=30)
            captured = logs.stdout + logs.stderr
            check(role + ' application output excludes all issued bearers',
                  logs.returncode == 0 and all(token.encode() not in captured for token in issued_tokens))
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
        matrix.command('docker', 'network', 'rm', network)
    return 0 if checks and all(row['passed'] for row in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
