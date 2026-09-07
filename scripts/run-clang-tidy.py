#!/usr/bin/env python3
"""Analyze native production sources using the Makefile's actual compile flags.

Compiler/analyzer process failures fail the target. Diagnostics are retained in
the report; warnings remain advisory, as in the original static-analysis lane.
"""
from concurrent.futures import ThreadPoolExecutor
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def compilation_database(plan, directory):
    entries = []
    for line in plan.splitlines():
        if not re.match(r'^(?:\S*/)?(?:gcc|clang|cc)(?:-[0-9]+)?\s', line):
            continue
        args = shlex.split(line)
        if '-c' not in args:
            continue
        sources = [arg for arg in args[1:] if arg.endswith('.c')]
        if len(sources) != 1:
            raise ValueError('compile command must identify exactly one C source')
        source = Path(sources[0])
        if 'vendor' in source.parts:
            continue
        source = (directory / source).resolve()
        if not source.is_relative_to(directory) or not source.is_file():
            raise ValueError('compile command source is outside the native tree or missing')
        # GCC-specific warning promotion is not a Clang diagnostic policy. Real
        # parse errors still make clang-tidy fail; preserve every warning in logs.
        args = [arg for arg in args if arg != '-Werror' and not arg.startswith('-Wl,')]
        entries.append(dict(directory=str(directory), arguments=args, file=str(source)))
    if not entries:
        raise ValueError('Make produced no production compile commands')
    return entries


def analyze(executable, database, source):
    # Source files are clang-tidy operands. Never put them after `--`, where
    # clang interprets them as compiler flags and reports "no input files".
    result = subprocess.run([executable, '-p', str(database), '--quiet', str(source)],
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return result.returncode, result.stdout


def self_test():
    with tempfile.TemporaryDirectory() as temp:
        directory = Path(temp)
        source = directory / 'sample.c'
        source.write_text('int main(void) { return 0; }\n')
        entries = compilation_database('gcc -c -Iheaders -DNAME=\'"literal"\' -Werror -o sample.o sample.c', directory)
        assert entries[0]['arguments'][-1] == 'sample.c'
        assert '-DNAME="literal"' in entries[0]['arguments']
        for plan in ('', 'gcc -c -o empty.o', 'gcc -c a.c b.c'):
            try:
                compilation_database(plan, directory)
            except ValueError:
                pass
            else:
                raise AssertionError('invalid compile plan accepted')
        tool = directory / 'fake-tidy'
        tool.write_text('#!/bin/sh\n[ "$1" = -p ] && [ "$3" = --quiet ] && [ -f "$4" ] || exit 99\necho "sample.c:1: error: planted analyzer failure"\nexit 17\n')
        tool.chmod(0o700)
        code, output = analyze(str(tool), directory, source)
        assert code == 17 and 'planted analyzer failure' in output
    print('clang-tidy runner: source operands, compile flags and failure propagation passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--executable', default='clang-tidy')
    parser.add_argument('--jobs', type=int, default=min(4, os.cpu_count() or 2))
    parser.add_argument('--log', type=Path, default=ROOT / 'src/build/clang-tidy.log')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    generated = subprocess.run(['make', 'agent_help_data.h', 'tool_prompts_data.h',
        'schema_data.h', 'kb/http/openapi_data.h', 'server/openapi_server_data.h'],
        cwd=ROOT / 'src', text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if generated.returncode:
        print(generated.stderr, file=sys.stderr)
        return generated.returncode
    plan = subprocess.run(['make', '-nB', 'kb', 'server'], cwd=ROOT / 'src',
                          text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if plan.returncode:
        print(plan.stderr, file=sys.stderr)
        return plan.returncode
    try:
        entries = compilation_database(plan.stdout, ROOT / 'src')
    except ValueError as error:
        print('clang-tidy: ' + str(error), file=sys.stderr)
        return 2
    sources = sorted({entry['file'] for entry in entries})
    args.log.parent.mkdir(parents=True, exist_ok=True)
    failures, warnings = 0, 0
    with tempfile.TemporaryDirectory(prefix='aimee-tidy-') as temp, args.log.open('w') as report:
        database = Path(temp)
        (database / 'compile_commands.json').write_text(json.dumps(entries))
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for index, (code, output) in enumerate(pool.map(lambda source: analyze(args.executable, database, source), sources), 1):
                report.write(output)
                warnings += output.count('warning:')
                if code:
                    failures += 1
                    print(f'clang-tidy: failed {sources[index - 1]} (exit {code})', flush=True)
                if index % 100 == 0:
                    report.flush()
                    print(f'clang-tidy: checked {index}/{len(sources)} sources', flush=True)
    print(f'clang-tidy: {len(sources)} sources; {warnings} warnings; {failures} process failures; report {args.log}')
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
