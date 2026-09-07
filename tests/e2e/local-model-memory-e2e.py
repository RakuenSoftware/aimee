#!/usr/bin/env python3
"""Real local-model recall gate against an explicitly named disposable stack.

Stops/restarts the supplied embedder and Server. The embedding model must be real;
this test asserts semantic retrieval without lexical overlap and uses the owning
Server's own API, CLI, memory module, PostgreSQL module and egress transport.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import time

spec = importlib.util.spec_from_file_location('placement', Path(__file__).with_name('memory-placement-e2e.py'))
placement = importlib.util.module_from_spec(spec)
spec.loader.exec_module(placement)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('server', 'store-db', 'embedder', 'output'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    gate = placement.Gate(args)
    mid = None
    query = 'cycling transportation workplace'
    content = 'I ride my bicycle to the office every morning.'
    def sql(query):
        return gate.docker('exec', args.store_db, 'psql', '-U', 'postgres', '-d', 'aimee_store', '-X', '-At', '-v', 'ON_ERROR_STOP=1', '-c', query)
    def contains(body):
        return any(row.get('memory_id') == mid for row in body.get('recall', {}).get('active_context', []))
    try:
        row = gate.good('store personal semantic fixture', gate.call('store', dict(key=gate.prefix, content=content)))
        mid = row['id']
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            if sql(f'SELECT count(*) FROM user_memory_vectors WHERE memory_id={mid}') == '1':
                break
            time.sleep(1)
        gate.check('real local vector persisted', int(sql(f'SELECT COALESCE(max(vector_dims(embedding)),0) FROM user_memory_vectors WHERE memory_id={mid}')) > 0)
        body = gate.good('semantic recall succeeds',
                         gate.wait('recall', dict(query=query), predicate=contains, timeout=90))
        gate.check('semantic recall finds the nonlexical personal fixture', contains(body))
        cli = gate.cli('recall', '--query', query, '--store', 'user')
        gate.check('CLI semantic recall finds the personal fixture', contains(cli))
        gate.docker('restart', args.server)
        # Recall can return a valid lexical-only result while the personal
        # vector index and embedding transport initialize after restart.
        # Require the nonlexical fixture within the recovery deadline.
        body = gate.good('recall recovers after Server restart',
                         gate.wait('recall', dict(query=query), predicate=contains))
        gate.check('semantic recall survives Server restart', contains(body))
        gate.docker('stop', args.embedder)
        try:
            body = gate.good('local lexical recall survives embedder outage', gate.call('recall', dict(query=gate.prefix)))
            gate.check('outage retains personal record', contains(body))
        finally:
            gate.docker('start', args.embedder)
        body = gate.wait('recall', dict(query=query), predicate=contains, timeout=90)[1]
        gate.check('semantic recall recovers when local embedder returns', contains(body))
        # Expiry is a database constraint shared by background and explicit recall.
        sql(f"UPDATE user_memories SET valid_until=now()-interval '1 second' WHERE id={mid}")
        body = gate.good('recall after expiry', gate.call('recall', dict(query=query)))
        gate.check('expired vector cannot resurrect personal memory', not contains(body))
        sql(f'UPDATE user_memories SET valid_until=NULL WHERE id={mid}')
        gate.good('retire personal fixture', gate.call('delete', dict(id=mid)))
        body = gate.good('recall after retirement', gate.call('recall', dict(query=query)))
        gate.check('retired vector cannot resurrect personal memory', not contains(body))
    finally:
        # Keep only verdicts. Synthetic keys and response content need not become
        # release artifacts or false positives in the credential scanner.
        Path(args.output).write_text(json.dumps([dict(name=c['name'], passed=c['passed']) for c in gate.checks], indent=2) + '\n')
    return 0 if all(c['passed'] for c in gate.checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
