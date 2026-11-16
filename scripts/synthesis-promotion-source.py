#!/usr/bin/env python3
"""Resolve reviewed synthesis artifacts, independently of queued wrapper edits."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
INPUTS = ('Dockerfile.llm', 'deploy/container/aimee-llm-entrypoint.sh',
          'scripts/synthesis-model-table.sh')


def resolve(image, root=ROOT):
    lock = json.loads((root / 'dependencies/synthesis-images.lock.json').read_text())
    if lock['schema_version'] != 1 or not re.fullmatch(r'[0-9a-f]{40}', lock['source_commit']):
        raise ValueError('invalid synthesis source lock')
    source = [subprocess.check_output(['git', 'show', lock['source_commit'] + ':' + path],
                                    cwd=root) for path in INPUTS]
    revision = hashlib.sha256(b''.join(source)).hexdigest()[:12]
    runtime = re.findall(rb'^ARG LLAMACPP_VERSION=(b[0-9]+)$', source[0], re.M)
    current = re.findall(rb'^ARG LLAMACPP_VERSION=(b[0-9]+)$',
                         (root / INPUTS[0]).read_bytes(), re.M)
    if len(runtime) != 1 or runtime != current or runtime[0].decode() != lock['runtime']:
        raise ValueError('runtime changed: qualify and lock the new synthesis images')
    if source[2] != (root / INPUTS[2]).read_bytes():
        raise ValueError('model selection changed: qualify and lock the new synthesis images')
    entry = lock['images'][image]
    quant = subprocess.check_output(['sh', str(root / INPUTS[2]), 'quant', entry['model']],
                                    text=True).strip()
    tag = f'{quant}-{lock["runtime"]}-{revision}'
    if entry['tag'] != tag or not re.fullmatch(r'sha256:[0-9a-f]{64}', entry['digest']):
        raise ValueError('synthesis artifact does not match its locked source')
    repository = 'ghcr.io/rakuensoftware/' + image
    return repository + ':' + tag, repository + '@' + entry['digest']


if __name__ == '__main__':
    try:
        tag, pinned = resolve(sys.argv[1])
        print('source_tag=' + tag)
        print('pinned=' + pinned)
    except (KeyError, ValueError, OSError, subprocess.CalledProcessError) as error:
        sys.exit('synthesis promotion source: ' + str(error))
