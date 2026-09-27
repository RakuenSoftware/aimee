#!/usr/bin/env python3
"""MR-02 automatic mutation and bulk-import authority through a real KB owner."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('matrix', ROOT / 'tests/e2e/deployment-matrix.py')
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    checks = []
    env = dict(os.environ, AIMEE_RUNTIME_WEB_ENABLED='0', AIMEE_POSTGRES_VOLUME_MIB='512',
               COMPOSE_PROFILES='', EMBEDDER_MODEL='bekko-a25m',
               EMBEDDER_URL='https://aimee-embedder:8762', EMBEDDER_DIMS='384',
               AIMEE_PROVIDER_CONTEXT_LIMITS=json.dumps(dict(schema_version=1, max_request_bytes=32768)))
    kb = matrix.Stack('kb', env, args.output)
    prefix = 'mr02-' + uuid.uuid4().hex

    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)
        if not passed:
            raise RuntimeError(name)

    def sql(query):
        result = subprocess.run(['docker', 'exec', kb.postgres, 'psql', '-U', 'postgres',
            '-d', 'aimee_store', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-c', query],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        if result.returncode:
            raise RuntimeError('synthetic fixture SQL failed')
        return result.stdout.strip()

    def action(name, body):
        return kb.kb_request('/v1/actions/' + name, body)

    try:
        kb.start()
        for origin in ('agent_message', 'user_stated', 'unknown'):
            for kind in ('world_fact', 'episode', 'experience', 'instruction', 'policy'):
                key = prefix + '-' + origin + '-' + kind
                sql(f"""INSERT INTO memories(tier,kind,epistemic_kind,key,content,provenance_category,
                    updated_at,source_session) VALUES('L0','fact','{kind}','{key}','original','{origin}',
                    pg_now_text('-100 days'),'{key}')""")
        code, result = action('memory.maintenance_run', dict(modes=1, force=True))
        check('automatic maintenance executes through Go owner', code == 200 and result.get('status') == 'ok')
        for origin in ('agent_message', 'user_stated', 'unknown'):
            for kind in ('world_fact', 'episode', 'experience', 'instruction', 'policy'):
                key = prefix + '-' + origin + '-' + kind
                expected = 'retired' if origin == 'agent_message' and kind == 'world_fact' else 'active'
                check('maintenance preserves admission ' + origin + '/' + kind,
                      sql(f"SELECT lifecycle_state FROM memories WHERE key='{key}'") == expected)
                if expected == 'active':
                    code, result = action('memory.fold_session', dict(session_id=key))
                    check('fold refuses protected source ' + origin + '/' + kind,
                          result.get('status') == 'error' and
                          sql(f"SELECT count(*) FROM memories WHERE key='{key}' AND content='original' AND lifecycle_state='active'") == '1')
        workspace = prefix + '-imports'
        for kind in ('episode', 'policy'):
            key = prefix + '-import-' + kind
            row = dict(tier='L2', kind='fact', epistemic_kind=kind, key=key,
                       content='imported protected text', authority='user', provenance_category='user_stated')
            code, result = action('kb.import', dict(workspace=workspace, memories=[row]))
            check('bulk import accepts ' + kind, code == 200 and result.get('imported') == 1)
            stored = json.loads(sql(f"SELECT json_build_object('kind',epistemic_kind,'origin',provenance_category,'content',content) FROM memories WHERE key='{key}'"))
            check('bulk import preserves kind without forging authority ' + kind,
                  stored == dict(kind=kind, origin='agent_message', content='imported protected text'))
            code, result = action('kb.import', dict(workspace=workspace, memories=[dict(row, content='overwrite')]))
            check('bulk import cannot overwrite ' + kind, result.get('status') != 'ok' and
                  sql(f"SELECT content FROM memories WHERE key='{key}' AND lifecycle_state='active'") == 'imported protected text')
            guard = subprocess.run(['docker', 'exec', kb.postgres, 'psql', '-U', 'postgres',
                '-d', 'aimee_store', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-c',
                f"BEGIN; UPDATE memories SET epistemic_kind='world_fact' WHERE key='{key}'; ROLLBACK;"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
            check('database blocks two-step protection downgrade ' + kind, guard.returncode != 0)
        code, exported = action('kb.export', dict(workspace=workspace))
        (args.output / 'export-response.json').write_text(json.dumps([code, exported], indent=2) + '\n')
        kinds = {row['key']: row.get('epistemic_kind') for row in exported.get('memories', [])}
        check('export retains protected kinds for subsequent import', code == 200 and all(
            kinds.get(prefix + '-import-' + kind) == kind for kind in ('episode', 'policy')))
        check('export excludes unrelated scopes', set(kinds) == {prefix+'-import-episode', prefix+'-import-policy'})
        for bad in (None, 42, {}, '', 'invented'):
            code, result = action('memory.store', dict(key=prefix+'-bad', content='bad kind', epistemic_kind=bad))
            check('invalid imported kind is refused ' + repr(bad), result.get('kind') == 'invalid_argument')
    finally:
        (args.output / 'checks.json').write_text(json.dumps(checks, indent=2) + '\n')
        kb.compose('down', '--volumes', '--remove-orphans')


if __name__ == '__main__':
    main()
