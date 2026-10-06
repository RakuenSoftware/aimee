#!/usr/bin/env python3
"""Validate populated native-memory export in a disposable published-image stack."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[3]


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tests/e2e' / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    matrix = load('matrix', 'deployment-matrix.py')
    placement = load('placement', 'memory-placement-e2e.py')
    env = dict(os.environ, AIMEE_RUNTIME_WEB_ENABLED='0', COMPOSE_PROFILES='',
               EMBEDDER_MODEL='bekko-a25m', EMBEDDER_URL='https://aimee-embedder:8762',
               EMBEDDER_DIMS='384')
    for key in ('AIMEE_APPLICATION_IMAGE', 'AIMEE_POSTGRES_IMAGE', 'AIMEE_EMBEDDER_IMAGE'):
        if not env.get(key):
            parser.error(key + ' must identify an already pulled image')
    stack = matrix.Stack('server', env, args.output)
    gate = placement.Gate(SimpleNamespace(server=stack.application, store_db=stack.postgres))
    checks = []

    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)
        if not passed:
            raise RuntimeError(name)

    def primitive(body):
        status, result = gate.call('/v1/native/primitive', body)
        check('native primitive returns its schema', status == 200 and result.get('schema') == 1)
        return result['records']

    try:
        stack.start()
        check('empty store exports an empty record bank', primitive(dict(session_start=True)) == [])
        key = 'native-release-overlap'
        stored = []
        for kind, name in (('fact', 'identity:' + key), ('preference', key)):
            status, result = gate.call('store', dict(key=name, kind=kind, tier='L2',
                content=key + ' ' + kind, confidence=1))
            check('store populated ' + kind + ' fixture', status == 200 and result.get('status') == 'ok')
            stored.append(result['id'])
        request = dict(task_hint=key, session_start=True, limit_tokens=8192)
        status, ordinary = gate.call('recall', request)
        check('ordinary recall succeeds', status == 200 and ordinary.get('status') == 'ok')
        groups = ordinary['recall']
        source = [row for group in ('identity', 'preferences', 'active_context', 'open_commitments')
                  for row in groups[group]]
        expected = {}
        for row in source:
            version = row['version']
            expected.setdefault((version['owner_id'], version['record_id']), row)
        check('fixture actually overlaps recall sections', len(source) > len(expected) >= 2)
        rows = primitive(request)
        check('overlapping populated records export once in selection order', rows == list(expected.values())[:32])
        check('record identities and revisions remain exact strings', all(
            isinstance(row['version'][key], str) for row in rows
            for key in ('owner_id', 'record_id', 'record_revision')))
        check('warm export retains the same current bank', primitive(request) == rows)
        mid = stored[0]
        status, result = gate.call('get', dict(id=mid, include_version=True))
        check('read current fixture version', status == 200 and result.get('status') == 'ok')
        before = result['memory']['version']
        status, changed = gate.call('supersede', dict(old_id=mid,
            new_content=key + ' corrected', expected_version=before))
        check('authorized correction commits', status == 200 and changed.get('status') == 'ok')
        updated = primitive(request)
        corrected = [row for row in updated if row['version']['record_id'] == str(mid)]
        check('corrected export observes content and committed revision', len(corrected) == 1 and
              corrected[0]['content'] == key + ' corrected' and
              corrected[0]['version'] == changed['version'] and changed['version'] != before)
        stack.compose('restart', 'aimee-server')
        stack.start()
        check('restart retains populated current export', primitive(request) == updated)
        status, _ = gate.call('/v1/native/primitive', dict(request, store='kb'))
        check('native primitive refuses implicit shared-store substitution', status == 400)
        status, _ = json.loads(matrix.command('docker', 'exec', '-i', stack.application,
            'python3', '-c', placement.HTTP, data=json.dumps(dict(
                method='POST', path='/v1/native/primitive', body=[]))))
        check('native primitive refuses a non-object request', status == 400)
    except Exception as error:
        checks.append(dict(name='native primitive gate completed', passed=False,
                           error=type(error).__name__))
        print('FAIL native primitive gate: ' + str(error), flush=True)
    finally:
        (args.output / 'native-primitive.json').write_text(json.dumps(checks, indent=2) + '\n')
        stack.compose('down', '--volumes', '--remove-orphans')
    return 0 if checks and all(row['passed'] for row in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
