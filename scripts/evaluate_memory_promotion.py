#!/usr/bin/env python3
"""Evaluate a pre-frozen MR-18 paired task experiment; never promote runtime policy."""
import argparse
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from benchmarks.common.reliability_experiment import evaluate
from benchmarks.common.runner import write_result_file


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('manifest', 'baseline', 'candidate', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    result = evaluate(*(json.loads(getattr(args, key).read_text()) for key in ('manifest', 'baseline', 'candidate')))
    write_result_file(args.output, result)
    return 0 if result['status'] == 'qualified' else 1


if __name__ == '__main__':
    raise SystemExit(main())
