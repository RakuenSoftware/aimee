#!/usr/bin/env python3
"""Verify staged or downloaded native-memory v0.2.3 release attachments."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as file:
        for block in iter(lambda: file.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def verify(directory):
    source = Path(__file__).resolve().parent
    files = directory.resolve()
    manifest = json.loads((source / 'manifest.json').read_text())
    assert manifest['release'] == 'native-memory-v0.2.3'
    expected = {
        'aimee_vllm-0.2.3-1-cp312-cp312-linux_x86_64.whl',
        'vllm_gguf_plugin-0.0.5+triton-py3-none-any.whl',
    }
    assert set(manifest['assets']) == expected
    assert (files / 'manifest.json').read_bytes() == (source / 'manifest.json').read_bytes()
    assert (files / 'SHA256SUMS').read_bytes() == (source / 'SHA256SUMS').read_bytes()
    sums = {}
    for line in (source / 'SHA256SUMS').read_text().splitlines():
        digest, name = line.split('  ', 1)
        assert name in expected | {'manifest.json'} and name not in sums
        sums[name] = digest
    assert set(sums) == expected | {'manifest.json'}
    for name, digest in sums.items():
        assert sha(files / name) == digest, name
    for name, info in manifest['assets'].items():
        assert info['sha256'] == sums[name]
        assert info['bytes'] == (files / name).stat().st_size
        assert info['url'] == (
            'https://github.com/RakuenSoftware/aimee/releases/download/'
            'native-memory-v0.2.3/' + name)
    with tempfile.TemporaryDirectory(prefix='aimee-release-verify-') as temporary:
        key = Path(temporary) / 'public.der'
        key.write_bytes(bytes.fromhex('302a300506032b6570032100' +
                                      manifest['public_key_ed25519_hex']))
        for name in sorted(expected | {'manifest.json', 'SHA256SUMS'}):
            signature = files / (name + '.sig')
            if name in {'manifest.json', 'SHA256SUMS'}:
                assert signature.read_bytes() == (source / signature.name).read_bytes()
            else:
                assert signature.stat().st_size == 64
            subprocess.run(['openssl', 'pkeyutl', '-verify', '-pubin',
                            '-keyform', 'DER', '-inkey', str(key), '-rawin',
                            '-in', str(files / name), '-sigfile', str(signature)],
                           check=True, capture_output=True)
    print('native-memory-v0.2.3 assets: hashes, sizes and four signatures verified')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: verify_assets.py ASSET_DIRECTORY')
    verify(Path(sys.argv[1]))
