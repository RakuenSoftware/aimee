#!/usr/bin/env python3
"""Gate synthesis on a newer upstream runtime release, embedders on changed inputs.

Wrapper and weight edits wait for the next synthesis runtime release. Publication
also checks upstream release existence and the per-runtime registry marker.
"""
import argparse
import re
import subprocess


INPUTS = {
    'llm': (
        'Dockerfile.llm',
        'deploy/container/aimee-llm-entrypoint.sh',
        'scripts/synthesis-model-table.sh',
    ),
    'embedder': (
        'Dockerfile.embedder',
        'deploy/container/aimee-embedder-entrypoint.sh',
        'scripts/bake-embedder.py',
        'scripts/embedder-server.py',
        'scripts/embedders.json',
    ),
}


def runtime_release(revision):
    source = subprocess.check_output(
        ['git', 'show', revision + ':Dockerfile.llm'], text=True)
    pins = re.findall(r'^ARG LLAMACPP_VERSION=(.*)$', source, re.MULTILINE)
    if len(pins) != 1 or not re.fullmatch(r'b[0-9]+', pins[0]):
        raise ValueError('Dockerfile.llm must pin one upstream llama.cpp build release')
    return int(pins[0][1:])


def changed(kind, before, after):
    # Resolve revisions before diff: a missing commit must fail the gate rather
    # than be interpreted as changed inputs and trigger an expensive build.
    revisions = [subprocess.check_output(
        ['git', 'rev-parse', '--verify', '--end-of-options', ref + '^{commit}'],
        text=True).strip() for ref in (before, after)]
    if kind == 'llm':
        # Wrapper, model table and application edits wait for the next runtime
        # release. Downgrades and reverts must never authorize a fresh build.
        return runtime_release(revisions[1]) > runtime_release(revisions[0])
    result = subprocess.run(['git', 'diff', '--quiet', *revisions, '--', *INPUTS[kind]])
    if result.returncode not in (0, 1):
        raise subprocess.CalledProcessError(result.returncode, result.args)
    return result.returncode == 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=INPUTS)
    parser.add_argument('before')
    parser.add_argument('after')
    args = parser.parse_args()
    print('changed=' + str(changed(args.kind, args.before, args.after)).lower())


if __name__ == '__main__':
    main()
