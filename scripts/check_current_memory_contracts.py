#!/usr/bin/env python3
"""Transitional serving-owner contracts; explicitly not full historical MR18 certification."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts.check_memory_reliability_release import import_path, load_recorded_events, verify_events

MANIFEST = ROOT / 'tests/eval/memory_reliability_release_v1.json'
EXTRACTED = './modules/memory/cmd/aimee-memory-eval'
CURRENT_PACKAGES = {'./modules/memory', './modules/execution-policy', './modules/aimee/families', './cmd/aimee-module'}


def current_cases(manifest):
    """Keep every remaining case identity; no package is implicitly skipped."""
    cases = [case for group in manifest['groups'] for case in group['cases']]
    extracted = [case for case in cases if case['package'] == EXTRACTED]
    current = [case for case in cases if case['package'] != EXTRACTED]
    if (manifest.get('version') != 1 or len(manifest['groups']) != 12
            or len(cases) != 173 or len(extracted) != 10 or len(current) != 163
            or len({case['id'] for case in cases}) != 173
            or {case['package'] for case in current} != CURRENT_PACKAGES):
        raise ValueError('review the current-serving/extracted case boundary explicitly')
    return current, extracted


def main():
    print('current Go serving-owner certification is unavailable: owner extracted; use the distinct adapter/canonical-producer gate and the independently qualified selected external module', file=sys.stderr)
    return 2


if __name__ == '__main__':
    raise SystemExit(main())
