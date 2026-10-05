#!/usr/bin/env python3
"""Run the existing device lifecycle gate against a pinned published image."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('matrix', ROOT / 'tests/e2e/deployment-matrix.py')
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, AIMEE_RUNTIME_WEB_ENABLED='1', COMPOSE_PROFILES='',
        EMBEDDER_MODEL='bekko-a25m', EMBEDDER_DIMS='384',
        EMBEDDER_URL='https://aimee-embedder:8762')
    for key in ('AIMEE_APPLICATION_IMAGE', 'AIMEE_POSTGRES_IMAGE', 'AIMEE_EMBEDDER_IMAGE'):
        if not env.get(key):
            parser.error(key + ' must identify an already pulled image')
    stack = matrix.Stack('server', env, args.output)
    stack.project = 'aimee-pairing-e2e-' + uuid.uuid4().hex[:10]
    stack.application = stack.project + '-aimee-server-1'
    stack.postgres = stack.project + '-aimee-store-db-1'
    stack.embedder = stack.project + '-aimee-embedder-1'
    reservations = []
    for port in (8743, 8443):
        reservation = socket.socket()
        reservation.bind(('127.0.0.1', 0))
        reservations.append((port, reservation))
    # Docker can reassign an automatic published port on restart. Reserve two
    # unused CT loopback ports once and persist those exact bindings instead.
    stack.network_override.write_text('services:\n  aimee-server:\n    ports: !override\n' +
        ''.join('      - "127.0.0.1:' + str(sock.getsockname()[1]) + ':' + str(port) + '"\n'
                for port, sock in reservations))
    result = dict(passed=False)
    private = args.output.parent.parent / ('private/client-pairing-' + stack.project)
    private.mkdir(mode=0o700, parents=True)
    try:
        matrix.command('python3', str(ROOT / 'scripts/compose-vault-init.py'),
            *stack.compose_args(), 'up', env=stack.env)
        for _, reservation in reservations:
            reservation.close()
        stack.compose('up', '-d', '--no-build', '--pull', 'never')
        deadline = time.monotonic() + 240
        while time.monotonic() < deadline:
            if matrix.command('docker', 'inspect', '--format', '{{.State.Health.Status}}',
                              stack.application) == 'healthy':
                break
            time.sleep(2)
        else:
            raise RuntimeError('pairing stack did not become healthy')
        ports = json.loads(matrix.command('docker', 'inspect', '--format',
            '{{json .NetworkSettings.Ports}}', stack.application))
        urls = {}
        for name, port in (('web', '8443/tcp'), ('api', '8743/tcp')):
            binding = ports[port]
            if len(binding) != 1 or binding[0]['HostIp'] != '127.0.0.1':
                raise RuntimeError('pairing listener escaped loopback')
            urls[name] = 'https://127.0.0.1:' + binding[0]['HostPort']
        client = args.output / 'aimee-testing-client'
        matrix.command('docker', 'cp', stack.application + ':/usr/local/bin/aimee', str(client))
        version = matrix.command(str(client), '--version')
        if 'testing-ed1d778' not in version:
            raise RuntimeError('pairing client version does not match reviewed image')
        completed = subprocess.run(['python3', str(ROOT / 'tests/e2e/client-pairing-e2e.py'),
            '--server', stack.application, '--store-db', stack.postgres, '--client', str(client),
            '--client-root', str(private / 'clients'),
            '--web-url', urls['web'], '--api-url', urls['api']],
            capture_output=True, text=True, timeout=1200)
        output = completed.stdout + completed.stderr
        (args.output / 'client-pairing.log').write_text(output + '\n')
        print(output, flush=True)
        if completed.returncode:
            raise RuntimeError('device lifecycle fixture exited ' + str(completed.returncode))
        private_compose = args.output.parent.parent / 'private/client-compose'
        private_compose.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chmod(private_compose, 0o700)
        shutil.copyfile(ROOT / 'compose.yaml', private_compose / 'compose.yaml')
        values = {key: value for key, value in stack.env.items()
                  if key.startswith(('AIMEE_', 'EMBEDDER_', 'COMPOSE_'))}
        values['COMPOSE_PROJECT_NAME'] = stack.project
        values['COMPOSE_FILE'] = str(ROOT / 'compose.yaml') + ':' + str(stack.network_override.resolve())
        env_file = private_compose / '.env'
        fd = os.open(env_file, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'w') as out:
            out.write(''.join(key + '=' + str(value) + '\n' for key, value in values.items()))
        completed = subprocess.run(['python3', str(ROOT / 'tests/e2e/standalone-index-e2e.py'),
            '--server', stack.application, '--store-db', stack.postgres,
            '--compose-dir', str(private_compose)], capture_output=True, text=True, timeout=1200)
        output = completed.stdout + completed.stderr
        (args.output / 'private-index.log').write_text(output + '\n')
        print(output, flush=True)
        if completed.returncode:
            raise RuntimeError('private index fixture exited ' + str(completed.returncode))
        result = dict(passed=True, client_version=version, listeners='CT loopback only',
                      private_index='passed')
    except Exception as error:
        result['error'] = type(error).__name__ + ': ' + str(error)
        print('FAIL pairing: ' + str(error), flush=True)
    finally:
        for _, reservation in reservations:
            reservation.close()
        (args.output / 'client-pairing.json').write_text(json.dumps(result, indent=2) + '\n')
        if result['passed']:
            stack.compose('down', '--volumes', '--remove-orphans')
        else:
            fd = os.open(private / 'stack.json', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, 'w') as out:
                json.dump(stack.__dict__, out, default=str)
            logs = subprocess.run(['docker', 'logs', stack.application], capture_output=True, text=True)
            (private / 'application.log').write_text(logs.stdout + logs.stderr)
            print('Retained failed disposable pairing project: ' + stack.project, flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
