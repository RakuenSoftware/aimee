#!/usr/bin/env python3
"""Phased 0.4.6 upgrade checks; retain private synthetic state for a CT snapshot."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('matrix', ROOT / 'tests/e2e/deployment-matrix.py')
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)
spec = importlib.util.spec_from_file_location('placement', ROOT / 'tests/e2e/memory-placement-e2e.py')
placement = importlib.util.module_from_spec(spec)
spec.loader.exec_module(placement)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=('prepare', 'upgrade', 'rollback'))
    parser.add_argument('--stage', type=Path, required=True)
    args = parser.parse_args()
    stage = args.stage.resolve()
    private = stage / 'private'
    private.mkdir(mode=0o700, exist_ok=True)
    os.chmod(private, 0o700)
    evidence = stage / 'evidence/published-upgrade'
    evidence.mkdir(parents=True, exist_ok=True)
    pins = json.loads((stage / 'image-pins.json').read_text())
    baseline = json.loads((stage / 'baseline-pins.json').read_text())
    checks = []

    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)
        if not passed:
            raise RuntimeError(name)

    def call(stack, op, body):
        if stack.role == 'kb':
            status, result = stack.kb_request('/v1/actions/memory.' + op, body)
        else:
            gate = placement.Gate(SimpleNamespace(server=stack.application, store_db=stack.postgres))
            status, result = gate.call(op, body)
        check(stack.role + ' ' + op + ' succeeds', status == 200 and result.get('status') == 'ok')
        return result

    def identity(stack):
        raw = matrix.command('docker', 'exec', stack.application, 'cat', '/var/lib/aimee/instance-identity.json')
        return hashlib.sha256(raw.encode()).hexdigest()

    try:
        source = stage / ('repo' if args.phase == 'upgrade' else 'baseline-compose')
        matrix.ROOT = source
        os.chdir(source)
        for role in ('kb', 'server'):
            state_file = private / (role + '.json')
            if args.phase == 'prepare':
                if state_file.exists():
                    raise RuntimeError('refusing to overwrite a prepared snapshot fixture')
                env = dict(os.environ, AIMEE_APPLICATION_IMAGE=baseline['aimee'],
                    AIMEE_POSTGRES_IMAGE=baseline['aimee-postgres'],
                    AIMEE_EMBEDDER_IMAGE=pins['embedder'], AIMEE_RUNTIME_WEB_ENABLED='0',
                    COMPOSE_PROFILES='', EMBEDDER_MODEL='bekko-a25m', EMBEDDER_DIMS='384',
                    EMBEDDER_URL='https://aimee-embedder:8762')
                stack = matrix.Stack(role, env, evidence)
                stack.start()
                fixtures = []
                for scope in (('global', 'project') if role == 'kb' else ('user',)):
                    context = dict(project='release-upgrade', scope_context=True) if scope == 'project' else {}
                    result = call(stack, 'store', dict(key='upgrade-' + role + '-' + scope,
                        content='Published 0.4.6 ' + role + ' ' + scope + ' canary 🦊', **context))
                    request = dict(id=str(result['id']), include_version=True, **context)
                    record = call(stack, 'get', request)['memory']
                    fixtures.append(dict(request=request, record=record))
                saved = dict(stack=stack.__dict__, fixtures=fixtures, identity=identity(stack))
                fd = os.open(state_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, 'w') as out:
                    json.dump(saved, out, default=str)
                check(role + ' baseline state and identity retained privately', True)
                stack.compose('stop')
                check(role + ' baseline quiesced for whole-CT snapshot', True)
                continue
            saved = json.loads(state_file.read_text())
            stack = matrix.Stack.__new__(matrix.Stack)
            stack.__dict__.update(saved['stack'])
            stack.output = Path(stack.output)
            stack.network_override = Path(stack.network_override)
            selected = pins if args.phase == 'upgrade' else baseline
            stack.env['AIMEE_APPLICATION_IMAGE'] = selected['aimee']
            stack.env['AIMEE_POSTGRES_IMAGE'] = selected['aimee-postgres']
            stack.start()
            check(role + ' retains immutable instance identity', identity(stack) == saved['identity'])
            for fixture in saved['fixtures']:
                current = call(stack, 'get', fixture['request'])['memory']
                original = fixture['record']
                keys = [key for key in ('id', 'content', 'confidence', 'provenance_category', 'version')
                        if key in original]
                check(role + ' preserves published record and authority',
                      all(current.get(key) == original[key] for key in keys))
            if args.phase == 'upgrade':
                version = matrix.command('docker', 'exec', stack.application, 'aimee', '--version')
                check(role + ' runs reviewed testing build', 'testing-ed1d778' in version)
                result = call(stack, 'store', dict(key='post-upgrade-' + role,
                    content='New write on reviewed testing image 🦊'))
                current = call(stack, 'get', dict(id=str(result['id'])))['memory']
                check(role + ' accepts and reads new post-upgrade writes',
                      current['content'] == 'New write on reviewed testing image 🦊')
                stack.compose('restart', 'aimee-' + role)
                stack.start()
                after = call(stack, 'get', dict(id=str(result['id'])))['memory']
                check(role + ' restart retains new writes', after['content'] == current['content'])
            else:
                version = matrix.command('docker', 'exec', stack.application, 'aimee', '--version')
                check(role + ' restored snapshot runs published 0.4.6', 'v0.4.6' in version)
    except Exception as error:
        checks.append(dict(name=args.phase + ' completed', passed=False, error=type(error).__name__))
        print('FAIL ' + args.phase + ': ' + str(error), flush=True)
    finally:
        (evidence / (args.phase + '.json')).write_text(json.dumps(checks, indent=2) + '\n')
    return 0 if checks and all(row['passed'] for row in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
