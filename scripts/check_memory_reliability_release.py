#!/usr/bin/env python3
"""Historical full MR18 gate (unavailable); generic fail-closed event helpers remain."""
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


def load_recorded_events(paths):
    """Group recorded `go test -json` events by import path.

    A recorded package counts only if its stream ends with the package-level
    result; a truncated or interrupted record is not evidence of anything.
    """
    events, finished = {}, set()
    for path in paths:
        for line in Path(path).read_text(encoding='utf-8').splitlines():
            try:
                event = json.loads(line)
            except ValueError:
                continue
            if not isinstance(event, dict) or not event.get('Package'):
                continue
            events.setdefault(event['Package'], []).append(event)
            if event.get('Action') in ('pass', 'fail', 'skip') and not event.get('Test'):
                finished.add(event['Package'])
    return {package: rows for package, rows in events.items() if package in finished}


def import_path(package, module_root=ROOT / 'server-go'):
    """`./modules/memory` -> `<module path>/modules/memory`, as `go test -json` names it."""
    for line in (module_root / 'go.mod').read_text(encoding='utf-8').splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0] == 'module':
            relative = package[2:] if package.startswith('./') else package
            return fields[1] + ('/' + relative.strip('/') if relative.strip('/.') else '')
    raise ValueError('server-go/go.mod declares no module path')


def main():
    parser = argparse.ArgumentParser(description="Historical full MR18 certification is unavailable after evaluator extraction.")
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--go-events', type=Path, action='append', default=[])
    parser.parse_args()
    parser.error('full historical MR18 certification is unavailable in the public adapter checkout; '
                 'the extracted evaluator and immutable baseline are private aims-db experiments/legacy-aimee-go. '
                 'Use check_current_memory_contracts.py for the distinct transitional serving-contract gate; '
                 'it does not recertify MR18.')


if __name__ == '__main__':
    raise SystemExit(main())
