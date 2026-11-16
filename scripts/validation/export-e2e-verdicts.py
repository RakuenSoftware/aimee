#!/usr/bin/env python3
"""Export reviewed verdict/count/hash fields; never copy native proof payloads."""
import argparse
import hashlib
import json
from pathlib import Path
import re

FILES = ('identity.json', 'topology.json', 'startup.json', 'memory-failures.json',
         'provider-boundary.json', 'native-async.json', 'shared-memory.json',
         'local-memory.json', 'semantic-memory.json', 'exploratory-memory.json',
         'correction-review.json', 'kb-free-personal-memory.json', 'kb-recreation-results.json',
         'plain-storage.json', 'luks-fresh.json', 'luks-upgrade.json', 'egress-vault.json')
LABELS = {'error', 'failed', 'cancelled', 'completed', 'unavailable', 'stale_context',
          'request_budget_exceeded', 'request_budget_unavailable', 'tool_call',
          'tool_result', 'execution', 'done', 'message', 'delta'}


def count(value):
    return type(value) is int and value >= 0


def terminal(value):
    if not isinstance(value, dict):
        return {}
    result = {'raw_data_exported': False}
    if count(value.get('event_count')):
        result['event_count'] = value['event_count']
    labels = value.get('labels', {})
    if isinstance(labels, dict):
        result['labels'] = {key: n for key, n in labels.items() if key in LABELS and count(n)}
    fingerprints = value.get('error_fingerprints', [])
    fingerprints = fingerprints if isinstance(fingerprints, list) else []
    result['error_fingerprints'] = [
        {'bytes': row['bytes'], 'sha256': row['sha256']}
        for row in fingerprints[:32]
        if isinstance(row, dict) and count(row.get('bytes')) and
        isinstance(row.get('sha256'), str) and re.fullmatch('[0-9a-f]{64}', row['sha256'])]
    return result


def verdict(value, raw):
    rows = value.get('checks', []) if isinstance(value, dict) else value
    # Check names are the driver's reviewed assertions, never model/source text.
    checks = [{'name': row['name'], 'passed': row['passed']} for row in rows
              if isinstance(row, dict) and isinstance(row.get('name'), str) and
              type(row.get('passed')) is bool] if isinstance(rows, list) else []
    result = dict(schema_version=1, export_policy='verdict-count-hash-whitelist',
                  source_sha256=hashlib.sha256(raw).hexdigest(), checks=checks,
                  counts=dict(total=len(checks), passed=sum(row['passed'] for row in checks),
                              failed=sum(not row['passed'] for row in checks)))
    if not isinstance(value, dict):
        return result
    runs = []
    source_runs = value.get('runs', [])
    for row in source_runs if isinstance(source_runs, list) else []:
        if not isinstance(row, dict) or not isinstance(row.get('name'), str):
            continue
        safe = {'name': row['name']}
        status = row.get('status')
        if status in ('completed', 'failed', 'cancelled') or type(status) is int and 100 <= status <= 599:
            safe['status'] = status
        if count(row.get('provider_requests')):
            safe['provider_requests'] = row['provider_requests']
        elapsed = row.get('elapsed_seconds')
        if type(elapsed) in (int, float) and 0 <= elapsed <= 1e9:
            safe['elapsed_seconds'] = elapsed
        sizes = row.get('provider_request_bytes')
        if isinstance(sizes, list) and all(count(n) for n in sizes):
            safe['provider_request_bytes'] = sizes
        runs.append(safe)
    if runs:
        result['runs'] = runs
    diagnostics = value.get('diagnostics', {})
    if isinstance(diagnostics, dict) and 'primary_native_recovery' in diagnostics:
        result['diagnostics'] = {'primary_native_recovery': terminal(diagnostics['primary_native_recovery'])}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.source.resolve() == args.output.resolve():
        parser.error('output must differ from raw evidence directory')
    args.output.mkdir(parents=True, exist_ok=False)
    for name in FILES:
        path = args.source / name
        if path.is_file() and not path.is_symlink():
            raw = path.read_bytes()
            safe = verdict(json.loads(raw), raw)
            (args.output / name).write_text(json.dumps(safe, indent=2) + '\n')


if __name__ == '__main__':
    main()
