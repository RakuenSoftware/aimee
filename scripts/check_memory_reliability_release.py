#!/usr/bin/env python3
"""Run frozen MR-18 invariants; incomplete/failed runs never publish a pass artifact."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / 'tests/eval/memory_reliability_release_v1.json'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_events(events, required):
    outcomes = {}
    failed = []
    for event in events:
        action, name = event.get('Action'), event.get('Test')
        if action in ('pass', 'fail', 'skip') and name:
            outcomes[name] = action
        if action in ('fail', 'skip') and (not name or name.split('/')[0] in required):
            failed.append((name or '<package>', action))
    missing = sorted(name for name in required if outcomes.get(name) != 'pass')
    if failed or missing:
        raise ValueError(f'incomplete invariant execution: missing={missing}, failed/skipped={failed}')
    return {name: outcomes[name] for name in sorted(required)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text())
    if manifest['version'] != 1 or len(manifest['groups']) != 12:
        raise ValueError('invalid frozen group manifest')
    for path, expected in manifest['inputs'].items():
        if digest(ROOT / path) != expected:
            raise ValueError('frozen input changed; explicit fixture review required: ' + path)
    required_env = ('AIMEE_MEMORY_EVAL_URL', 'AIMEE_DB2_REPLAY_URL', 'AIMEE_TEST_PG_URL', 'AIMEE_DB_TEST_URL')
    if any(not os.environ.get(key) for key in required_env):
        raise ValueError('disposable memory, DB2, DB1 and evaluation databases required')
    packages = {}
    for group in manifest['groups']:
        if not group['cases']:
            raise ValueError('empty adversarial group')
        for case in group['cases']:
            packages.setdefault(case['package'], set()).add(case['test'])
    python_code = """
import importlib, json, sys, unittest
manifest = json.load(open(sys.argv[1]))
suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromName(name) for name in manifest['python_tests'])
result = unittest.TextTestRunner(stream=sys.stderr).run(suite)
print(json.dumps({'executed': result.testsRun, 'expected': len(manifest['python_tests']), 'skipped': len(result.skipped)}))
sys.exit(0 if result.wasSuccessful() and not result.skipped and result.testsRun == len(manifest['python_tests']) else 1)
"""
    python_run = subprocess.run([os.sys.executable, '-c', python_code, str(MANIFEST)], cwd=ROOT, text=True, capture_output=True, timeout=180)
    if python_run.returncode:
        raise ValueError('frozen Python metric/denominator/release tests failed or skipped')
    python_result = json.loads(python_run.stdout)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = {'version': 1, 'status': 'pass', 'manifest_sha256': digest(MANIFEST),
              'implementation': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'working_diff_sha256': hashlib.sha256(subprocess.check_output(['git', 'diff', 'HEAD'], cwd=ROOT)).hexdigest(),
              'packages': {}, 'python': python_result, 'groups': manifest['groups'],
              'promotion': 'unqualified: deterministic invariants do not measure live-model task quality or cost'}
    # Databases and package tests are intentionally serialized. The memory tests
    # include a real non-owner role; the bootstrap connection is administrative.
    for package, required in packages.items():
        command = ['go', 'test', '-json', '-race', '-count=1', '-timeout=20m', package,
                   '-run', '^(' + '|'.join(sorted(required)) + ')$']
        result = subprocess.run(command, cwd=ROOT / 'server-go', text=True, capture_output=True, timeout=1260)
        events = []
        for line in result.stdout.splitlines():
            events.append(json.loads(line))
        if result.returncode:
            # Do not copy private connection strings or SQL/provider payloads to
            # a public artifact. Diagnostics stay in the operator's test runner.
            names = sorted({e.get('Test', '<package>') for e in events if e.get('Action') == 'fail'})
            raise ValueError(f'{package}: test exit {result.returncode}; failing tests={names}')
        report['packages'][package] = verify_events(events, required)
        print(f'{package}: {len(required)} required tests passed', flush=True)
    fd, temporary = tempfile.mkstemp(dir=args.output.parent, prefix='.' + args.output.name)
    try:
        with os.fdopen(fd, 'w') as out:
            json.dump(report, out, indent=2, allow_nan=False)
            out.write('\n')
        os.replace(temporary, args.output)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
