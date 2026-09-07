#!/usr/bin/env python3
"""Gate model image builds on their inputs changing in this push or PR update.

PR path filters retain all earlier changes on the branch. Compare the previous
head with the new head so a later application/test edit cannot rebuild models
whose candidate tags are not published until merge.
"""
import argparse
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


def changed(kind, before, after):
    # Resolve revisions before diff: a missing commit must fail the gate rather
    # than be interpreted as changed inputs and trigger an expensive build.
    revisions = [subprocess.check_output(
        ['git', 'rev-parse', '--verify', '--end-of-options', ref + '^{commit}'],
        text=True).strip() for ref in (before, after)]
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
