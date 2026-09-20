#!/usr/bin/env python3
"""Concurrent public requests and owner failure on an owned disposable Server.

Creates synthetic records and suspends/kills the Go memory process. Never run
against a user's installation. The C bus and application stay running.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
from pathlib import Path
import time

spec = importlib.util.spec_from_file_location('placement', Path(__file__).with_name('memory-placement-e2e.py'))
placement = importlib.util.module_from_spec(spec)
spec.loader.exec_module(placement)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('server', 'store-db', 'output'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    suffix = '-aimee-server-1'
    if not args.server.startswith('aimee-e2e-server-') or not args.server.endswith(suffix):
        parser.error('requires a disposable deployment-matrix Server')
    if args.store_db != args.server.removesuffix(suffix) + '-aimee-store-db-1':
        parser.error('store-db must belong to the same disposable Server project')
    gate = placement.Gate(args)
    fixtures = []
    timings = {}

    def write(index):
        content = ('Synthetic concurrent fixture %d αβ🦊\n' % index) * 200
        code, row = gate.call('store', dict(key=gate.prefix + '-' + str(index), content=content))
        if code != 200 or row.get('status') != 'ok':
            raise RuntimeError('concurrent store failed')
        return row['id'], content

    def read(fixture):
        mid, content = fixture
        code, row = gate.call('get', dict(id=mid))
        return code == 200 and row.get('memory', {}).get('content') == content

    try:
        with ThreadPoolExecutor(max_workers=6) as pool:
            fixtures = list(pool.map(write, range(24)))
            gate.check('concurrent writes produce distinct IDs', len({mid for mid, _ in fixtures}) == 24)
            gate.check('concurrent reads preserve full Unicode content', all(pool.map(read, fixtures)))
        for invalid in (0, -1, 1.5, True, [], {}, '1junk', '9223372036854775808'):
            code, row = gate.call('get', dict(id=invalid))
            gate.check('malformed ID rejected ' + repr(invalid), code == 400 and row.get('kind') == 'invalid_argument')
        large_id = 9007199254740993
        large_content = 'Exact int64 synthetic fixture αβ🦊'
        gate.docker('exec', args.store_db, 'psql', '-U', 'postgres', '-d', 'aimee_store',
            '-X', '-At', '-v', 'ON_ERROR_STOP=1', '-c',
            f"INSERT INTO user_memories(id,key,content) VALUES({large_id},'{gate.prefix}-int64','{large_content}')")
        try:
            code, row = gate.call('get', dict(id=str(large_id)))
            gate.check('HTTP preserves int64 ID above float precision', code == 200 and
                       row.get('memory', {}).get('id') == large_id and row['memory'].get('content') == large_content)
            text = gate.docker('exec', '-u', '1000', '-e',
                'AIMEE_API_ENDPOINT=unix:/var/lib/aimee/aimee-http.sock', args.server,
                'aimee', 'memory', 'get', str(large_id))
            gate.check('CLI text preserves int64 ID and content', str(large_id) in text and large_content in text)
            row = gate.cli('get', str(large_id))
            gate.check('CLI preserves int64 ID above float precision', row.get('memory', {}).get('id') == large_id)
            code, body = gate.mcp('memory_get', dict(id=str(large_id), store='user'))
            gate.check('MCP preserves int64 ID above float precision', code == 200 and str(large_id) in body)
        finally:
            gate.good('retire exact int64 fixture', gate.call('delete', dict(id=str(large_id))))
        mid, content = fixtures[0]
        # Decimal string IDs preserve exact tokens through the public transport.
        code, row = gate.call('get', dict(id=str(mid)))
        gate.check('decimal string ID resolves exact record', code == 200 and row.get('memory', {}).get('content') == content)
        pid = int(gate.docker('exec', args.server, 'python3', '-c',
            "from pathlib import Path; import os,signal; "
            "pids=[int(p.name) for p in Path('/proc').iterdir() if p.name.isdigit() and "
            "(p/'cmdline').exists() and (p/'cmdline').read_bytes().split(b'\\0')[0].endswith(b'/aimee-module-memory')]; "
            "assert len(pids)==1,pids; os.kill(pids[0],signal.SIGSTOP); print(pids[0])"))
        try:
            started = time.monotonic()
            code, row = gate.call('get', dict(id=mid))
            timings['owner_unavailable_seconds'] = time.monotonic() - started
            gate.check('paused personal owner produces bounded HTTP failure',
                       code >= 500 and row.get('status') == 'error' and timings['owner_unavailable_seconds'] < 70)
            code, _ = gate.call('/v1/health', method='GET')
            gate.check('application stays live during memory owner outage', code == 200)
        finally:
            gate.docker('exec', args.server, 'python3', '-c',
                'import os,signal,sys; p=int(sys.argv[1]); os.kill(p,signal.SIGCONT); os.kill(p,signal.SIGTERM)', str(pid))
        started = time.monotonic()
        gate.wait('get', dict(id=mid), predicate=lambda row: row.get('memory', {}).get('content') == content)
        timings['owner_recovery_seconds'] = time.monotonic() - started
        gate.check('supervised owner restart preserves committed content', True)
        with ThreadPoolExecutor(max_workers=6) as pool:
            gate.check('all concurrent records survive owner restart', all(pool.map(read, fixtures)))
        for mid, _ in fixtures:
            code, row = gate.call('delete', dict(id=mid))
            if code != 200 or row.get('status') != 'ok':
                raise RuntimeError('fixture retirement failed')
        with ThreadPoolExecutor(max_workers=6) as pool:
            def retired(fixture):
                code, row = gate.call('get', dict(id=fixture[0]))
                return code == 404 and row.get('kind') == 'not_found'
            gate.check('concurrent reads exclude every retired record', all(pool.map(retired, fixtures)))
    finally:
        Path(args.output).write_text(json.dumps(dict(checks=[dict(name=c['name'], passed=c['passed'])
            for c in gate.checks], timings=timings), indent=2) + '\n')
    return 0 if gate.checks and all(c['passed'] for c in gate.checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
