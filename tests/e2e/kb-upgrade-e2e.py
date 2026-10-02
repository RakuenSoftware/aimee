#!/usr/bin/env python3
"""Recreate a current-layout KB through shipped Compose and verify persistence.

Run as root on a disposable Docker Linux VM. Candidate images are supplied in
AIMEE_APPLICATION_IMAGE, AIMEE_POSTGRES_IMAGE and AIMEE_EMBEDDER_IMAGE. No retired
image or database layout is adopted. Evidence contains verdicts, not credentials.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('matrix', ROOT / 'tests/e2e/deployment-matrix.py')
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)


def exercise(stack, check):
    stack.start()
    check('current-layout KB reaches healthy state', True)
    check('application metadata contains no credentials', matrix.application_metadata_is_private(stack))
    fixtures = []
    for scope in ('global', 'project'):
        context = dict(project='recreation-canary', scope_context=True) if scope == 'project' else {}
        content = 'Current-layout ' + scope + ' canary 🦊'
        status, result = stack.kb_request('/v1/actions/memory.store',
            dict(key='recreation-' + scope, content=content, **context))
        check('KB writes ' + scope + ' canary', status == 200 and result.get('status') == 'ok' and result.get('id'))
        read = dict(id=str(result['id']), **context)
        status, result = stack.kb_request('/v1/actions/memory.get', read)
        record = result.get('memory', {})
        check('KB reads ' + scope + ' canary', status == 200 and record.get('content') == content)
        fixtures.append((scope, read, record))
    for attempt in range(2):
        stack.compose('down')
        stack.start()
        for scope, read, before in fixtures:
            status, result = stack.kb_request('/v1/actions/memory.get', read)
            after = result.get('memory', {})
            check(f'recreation {attempt + 1} retains {scope} content and authority', status == 200 and
                all(after.get(key) == before.get(key) for key in ('id', 'content', 'provenance_category', 'confidence')))
        status, result = stack.kb_request('/v1/actions/memory.store',
            dict(key=f'after-recreation-{attempt}', content='New write after recreation'))
        check(f'recreation {attempt + 1} accepts new writes', status == 200 and result.get('status') == 'ok' and result.get('id'))
        read = dict(id=str(result['id']))
        status, result = stack.kb_request('/v1/actions/memory.get', read)
        check(f'recreation {attempt + 1} reads new write', status == 200 and
              result.get('memory', {}).get('content') == 'New write after recreation')
        fixtures.append((f'new-{attempt}', read, result['memory']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--keep', action='store_true', help='retain test containers for diagnosis')
    parser.add_argument('--storage', choices=('plain', 'luks'), default='plain')
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('run as root in a disposable Docker VM')
    for name in ('AIMEE_APPLICATION_IMAGE', 'AIMEE_POSTGRES_IMAGE', 'AIMEE_EMBEDDER_IMAGE'):
        if not os.environ.get(name):
            parser.error(name + ' must name the candidate image')
    args.output.mkdir(parents=True, exist_ok=True)
    checks = []
    stack = None

    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)
        if not passed:
            raise RuntimeError(name)

    try:
        env = dict(os.environ, AIMEE_POSTGRES_STORAGE=args.storage, AIMEE_RUNTIME_WEB_ENABLED='0',
                   AIMEE_POSTGRES_VOLUME_MIB='2048', COMPOSE_PROFILES='', EMBEDDER_MODEL='bekko-a25m',
                   EMBEDDER_URL='https://aimee-embedder:8762', EMBEDDER_DIMS='384')
        if args.storage == 'luks':
            env['AIMEE_DEVICE_MAPPER_MAJOR'] = next(line.split()[0] for line in Path('/proc/devices').read_text().splitlines()
                                                  if line.split()[-1:] == ['device-mapper'])
        stack = matrix.Stack('kb', env, args.output)
        exercise(stack, check)
    except (RuntimeError, OSError, ValueError, KeyError, subprocess.SubprocessError):
        checks.append(dict(name='current-layout KB recreation completed', passed=False))
        print('FAIL current-layout KB recreation completed', flush=True)
    finally:
        (args.output / 'kb-recreation-results.json').write_text(json.dumps(checks, indent=2) + '\n')
        if stack and not args.keep:
            stack.compose('down', '--volumes', '--remove-orphans')
    return 0 if checks and all(check['passed'] for check in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
