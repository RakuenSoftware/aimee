#!/usr/bin/env python3
"""Verify exact release bytes and detached signatures against reviewed metadata."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

RELEASE = 'native-memory-v0.3.3'
PUBLIC_KEY = '5d14ca247841adcc1130a4d95b82ead12ba699c03f2ba45f2f47524abde0c6c2'
WHEELS = {
    'aimee_gemma4_12b-0.3.3-cp312-cp312-linux_x86_64.whl',
    'aimee_gemma4_26b-0.3.3-cp312-cp312-linux_x86_64.whl',
    'aimee_gemma4_e2b-0.3.3-cp312-cp312-linux_x86_64.whl',
    'aimee_gemma4_e4b-0.3.3-cp312-cp312-linux_x86_64.whl',
    'aimee_native_runtime-0.3.3-5-cp312-cp312-linux_x86_64.whl',
    'aimee_qwen3_8_27b-0.3.3-cp312-cp312-linux_x86_64.whl',
    'vllm_gguf_plugin-0.0.5+triton-py3-none-any.whl',
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def verify(directory, *, hashes_only=False):
    source = Path(__file__).resolve().parent
    manifest = json.loads((source / 'manifest.json').read_text())
    require(manifest['release'] == RELEASE and manifest['version'] == '0.3.3'
            and manifest['shared_runtime_build'] == 5, 'unexpected release identity')
    require(manifest['public_key_ed25519_hex'] == PUBLIC_KEY, 'unreviewed trust key')
    require(set(manifest['assets']) == WHEELS, 'unexpected wheel set')
    for name in ('manifest.json', 'SHA256SUMS'):
        require((directory / name).read_bytes() == (source / name).read_bytes(),
                f'{name} differs from reviewed bytes')
    sums = {}
    for line in (source / 'SHA256SUMS').read_text().splitlines():
        digest, name = line.split('  ', 1)
        require(name in WHEELS | {'manifest.json'} and name not in sums,
                'unexpected checksum entry')
        sums[name] = digest
    require(set(sums) == WHEELS | {'manifest.json'}, 'incomplete checksum set')
    for name, digest in sums.items():
        require(sha(directory / name) == digest, f'{name}: checksum mismatch')
    for name, info in manifest['assets'].items():
        require(info['sha256'] == sums[name], f'{name}: manifest checksum mismatch')
        require(info['bytes'] == (directory / name).stat().st_size, f'{name}: size mismatch')
        require(info['url'] == 'https://github.com/RakuenSoftware/aimee/releases/download/'
                + RELEASE + '/' + name, f'{name}: unexpected URL')
    if hashes_only:
        print(f'{RELEASE}: seven wheel identities verified; unsigned candidate, signatures unchecked')
        return
    with tempfile.TemporaryDirectory(prefix='aimee-release-verify-') as temporary:
        key = Path(temporary) / 'public.der'
        key.write_bytes(bytes.fromhex('302a300506032b6570032100' + PUBLIC_KEY))
        for name in sorted(WHEELS | {'manifest.json', 'SHA256SUMS'}):
            signature = directory / (name + '.sig')
            require(signature.stat().st_size == 64, f'{name}: invalid signature size')
            subprocess.run(['openssl', 'pkeyutl', '-verify', '-pubin', '-keyform', 'DER',
                            '-inkey', str(key), '-rawin', '-in', str(directory / name),
                            '-sigfile', str(signature)], check=True, capture_output=True)
    print(f'{RELEASE}: seven wheels, reviewed metadata and nine signatures verified')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hashes-only', action='store_true', help='unsigned identity review only')
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    verify(args.directory.resolve(), hashes_only=args.hashes_only)
