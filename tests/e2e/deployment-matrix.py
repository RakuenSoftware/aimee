#!/usr/bin/env python3
"""Fresh, isolated Docker release topologies using the shipped compositions.

T1: shared KB. T2: standalone Server enrolled into a separately deployed KB.
T3: KB-free Server. Images must already exist locally. All created containers,
networks and volumes belong to a random project and are removed unless --keep.
Credentials remain in memory and test output contains verdicts only.
"""
import argparse
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
        return ('--env-file', '/dev/null', '-p', self.project, '-f', self.file,
                '-f', str(self.network_override))

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
        deadline = time.monotonic() + 240
        while time.monotonic() < deadline:
            status = command('docker', 'inspect', '--format', '{{.State.Health.Status}}', self.application)
            if status == 'healthy':
                return
            time.sleep(2)
        raise RuntimeError(self.role + ' did not become healthy')

    def kb_request(self, path, body=None, authenticated=True):
        code = '''import http.client,json,sys
a=json.load(sys.stdin); c=http.client.HTTPConnection('127.0.0.1',8741,timeout=40)
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
    env['AIMEE_DEVICE_MAPPER_MAJOR'] = next(line.split()[0] for line in Path('/proc/devices').read_text().splitlines()
                                           if line.split()[-1:] == ['device-mapper'])
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
            code, body = kb.kb_request('/v1/search', dict(query='shared deployment fixture', scope='all', max_results=3))
            check('KB ranked search uses its local embedding service', code == 200 and isinstance(body.get('hits'), list))
            code, body = kb.kb_request('/v1/actions/memory.find_facts', dict(query='shared deployment fixture', limit=3, graph_code_fusion_state='on'))
            check('KB graph and memory retrieval survives the deepest worker path', code == 200 and isinstance(body.get('facts'), list))
        if args.topology in ('T2', 'T3'):
            server = Stack('server', env, args.output)
            stacks.append(server)
            server.start()
            check('Server boots without a KB connection', True)
            check('Server application metadata contains no database or enrollment credential', application_metadata_is_private(server))
            gate_script = ROOT / 'tests/e2e/memory-placement-e2e.py'
            common = ['--server', server.application, '--store-db', server.postgres]
            command('python3', str(gate_script), *common, '--output', str(args.output / 'local-memory.json'))
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
