#!/usr/bin/env python3
"""Receipt health acceptance in an owned, disposable deployment-matrix Server.

Exports only check names and aggregate counts, never ledger details or keys.
"""
import argparse
import http.client
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import time
import uuid


def inside():
    class Local(http.client.HTTPConnection):
        def connect(self):
            self.sock = socket.socket(socket.AF_UNIX)
            self.sock.settimeout(60)
            self.sock.connect('/var/lib/aimee/aimee-http.sock')

    def query(body):
        conn = Local('localhost', timeout=60)
        try:
            conn.request('POST', '/v1/memory/health', json.dumps(body), {'Content-Type': 'application/json'})
            response = conn.getresponse()
            result = json.loads(response.read())
            if response.status != 200 or result.get('status') != 'ok':
                raise RuntimeError('health endpoint did not return an aggregate: ' + str(result.get('kind', 'unknown')))
            return result
        finally:
            conn.close()

    if os.getuid() == 0:
        result = query(dict(window='1h', health_principal='uid:1000', principal='uid:1000', collection_complete=True))
        if result.get('collection_state') != 'not_collected' or result.get('metrics') is not None:
            raise RuntimeError('foreign principal discovered a health population')
        print(json.dumps(dict(foreign_principal_isolated=True)))
        return
    if os.getuid() != 1000 or os.environ.get('AIMEE_MEMORY_HEALTH_ENABLED') != '1':
        raise RuntimeError('requires the opted-in disposable application owner')
    with sqlite3.connect('file:/var/lib/aimee/audit/worm-live.db?mode=ro', uri=True) as db:
        rows = db.execute("SELECT seq,subject,detail FROM audit_event WHERE actor_role='host' AND actor_principal='uid:1000' AND action='memory.provider.prepared' ORDER BY seq DESC LIMIT 256").fetchall()
        if not rows:
            raise RuntimeError('requires real prepared provider inputs from the native fixture')
        binding = json.loads(rows[0][2])['binding']
        scope = dict(project=binding['project'], workspace=binding['workspace'], window='1h')
        result = query(scope)
        health = result.get('health', {})
        collection = result.get('collection', {})
        if collection.get('failed_requests') != 0 or collection.get('truncated') or collection.get('imported_requests', 0) <= 0:
            raise RuntimeError('receipt collector lost fixture input')
        population = health['sample_metrics']['population']
        from datetime import datetime
        parse = lambda value: datetime.fromisoformat(value.replace('Z', '+00:00'))
        expected = {}
        identifiers = []
        for seq, attempt, detail in rows:
            prepared = json.loads(detail)
            b = prepared['binding']
            if b['project'] != scope['project'] or b['workspace'] != scope['workspace']:
                continue
            if not parse(population['from']) <= parse(prepared['at']) < parse(population['until']):
                continue
            stages = {row[0] for row in db.execute('SELECT action FROM audit_event WHERE actor_role=? AND actor_principal=? AND subject=? AND seq<=?', ('host', 'uid:1000', attempt, int(collection['verified_through_sequence'])))}
            if 'memory.provider.acknowledged' in stages:
                stage = 'dispatched'
            elif stages & {'memory.provider.dispatch_started', 'memory.provider.outcome_unknown'}:
                stage = 'network_uncertain'
            elif 'memory.provider.dispatch_admitted' in stages:
                stage = 'unknown_dispatch'
            else:
                continue  # Active/prior ownership resolution is tested separately.
            expected[stage] = expected.get(stage, 0) + 1
            identifiers.append(attempt)
        actual = health['exact_retained_attempts_by_stage']
        if not expected or any(actual.get(stage, 0) != count for stage, count in expected.items()):
            raise RuntimeError('health counters do not match committed stage evidence')
        if any(identity in json.dumps(result) for identity in identifiers):
            raise RuntimeError('aggregate exposed private attempt identities')
        if not health['metadata_gaps'].get('query_fingerprint') or 'Missing query_fingerprint:' not in result.get('text', ''):
            raise RuntimeError('legacy metadata gap was hidden')
        forged = query(dict(scope, health_principal='another-principal', principal='another-principal', ledger_events=[], collection_complete=True))
        if forged['health']['exact_retained_attempts_by_stage'] != actual:
            raise RuntimeError('public fields changed authenticated collection')
        print(json.dumps(dict(counts=actual, sampled=health['sampled_invocations'], scope_isolated=True,
                              late_stage_reconciled=True, metadata_gaps_visible=True, public_fields_ignored=True)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server')
    parser.add_argument('--output')
    parser.add_argument('--inside', action='store_true')
    args = parser.parse_args()
    if args.inside:
        inside()
        return
    server = args.server or ''
    if not server.startswith('aimee-e2e-server-') or not server.endswith('-aimee-server-1'):
        parser.error('requires an owned deployment-matrix Server')
    info = json.loads(subprocess.check_output(['docker', 'inspect', server], text=True))[0]
    if info['Config']['Labels'].get('com.docker.compose.project') != server.removesuffix('-aimee-server-1'):
        parser.error('Server ownership mismatch')
    remote = '/tmp/' + uuid.uuid4().hex + '-memory-health.py'
    checks = []
    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))
        Path(args.output).write_text(json.dumps(checks, indent=2) + '\n')
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)
        if not passed:
            raise RuntimeError(name)
    def run(uid):
        return json.loads(subprocess.check_output(['docker', 'exec', '-u', str(uid), server, 'python3', remote, '--inside'], text=True, timeout=180))
    try:
        subprocess.run(['docker', 'cp', str(Path(__file__).resolve()), server + ':' + remote], check=True, stdout=subprocess.DEVNULL)
        before = run(1000)
        check('health migration and committed receipt import through actual HTTP and store bus', before.get('sampled', 0) > 0)
        check('health public principal and ledger fields cannot replace host identity', before['public_fields_ignored'])
        check('health missing metadata is explicit in JSON and text', before['metadata_gaps_visible'])
        check('health foreign UDS principal cannot enumerate another population', run(0)['foreign_principal_isolated'])
        subprocess.run(['docker', 'kill', '--signal', 'KILL', server], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(['docker', 'start', server], check=True, stdout=subprocess.DEVNULL)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            state = subprocess.check_output(['docker', 'inspect', '--format', '{{.State.Health.Status}}', server], text=True).strip()
            if state == 'healthy':
                break
            time.sleep(1)
        else:
            raise RuntimeError('owned health fixture did not recover')
        after = run(1000)
        check('health persisted attempts reconcile idempotently after SIGKILL and restart', before['counts'] == after['counts'])
    finally:
        subprocess.run(['docker', 'exec', '-u', '0', server, 'rm', '-f', remote], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
