"""Refusal checks for the public three-model release verifier."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
VERIFIER = ROOT / 'docs/releases/native-memory-v0.3.2/verify_assets.py'


class NativeMemoryReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='native-release-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'reviewed'
        self.assets = self.root / 'assets'
        self.source.mkdir()
        self.assets.mkdir()
        shutil.copyfile(VERIFIER, self.source / 'verify_assets.py')
        spec = importlib.util.spec_from_file_location('native_release_test',
                                                    self.source / 'verify_assets.py')
        self.verifier = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.verifier)
        self.key = self.root / 'test-key.pem'
        subprocess.run(['openssl', 'genpkey', '-algorithm', 'ED25519', '-out', str(self.key)],
                       check=True, capture_output=True)
        der = subprocess.check_output(['openssl', 'pkey', '-in', str(self.key),
                                       '-pubout', '-outform', 'DER'])
        self.verifier.PUBLIC_KEY = der[-32:].hex()
        info = {}
        for name in sorted(self.verifier.WHEELS):
            payload = ('synthetic artifact ' + name).encode()
            (self.assets / name).write_bytes(payload)
            info[name] = {'sha256': hashlib.sha256(payload).hexdigest(), 'bytes': len(payload),
                          'url': 'https://github.com/RakuenSoftware/aimee/releases/download/'
                          + self.verifier.RELEASE + '/' + name}
        manifest = {'release': self.verifier.RELEASE, 'version': '0.3.2', 'build': 2,
                    'assets': info, 'public_key_ed25519_hex': self.verifier.PUBLIC_KEY}
        (self.source / 'manifest.json').write_text(json.dumps(manifest))
        sums = ''.join(v['sha256'] + '  ' + n + '\n' for n, v in info.items())
        sums += self.verifier.sha(self.source / 'manifest.json') + '  manifest.json\n'
        (self.source / 'SHA256SUMS').write_text(sums)
        for name in ('manifest.json', 'SHA256SUMS'):
            shutil.copyfile(self.source / name, self.assets / name)

    def sign(self):
        for name in self.verifier.WHEELS | {'manifest.json', 'SHA256SUMS'}:
            subprocess.run(['openssl', 'pkeyutl', '-sign', '-inkey', str(self.key), '-rawin',
                            '-in', str(self.assets / name),
                            '-out', str(self.assets / (name + '.sig'))],
                           check=True, capture_output=True)

    def test_unsigned_review_cannot_pass_publication_verification(self):
        self.verifier.verify(self.assets, hashes_only=True)
        with self.assertRaises(FileNotFoundError):
            self.verifier.verify(self.assets)

    def test_signed_payload_and_invalid_signature(self):
        self.sign()
        self.verifier.verify(self.assets)
        (self.assets / 'manifest.json.sig').write_bytes(bytes(64))
        with self.assertRaises(subprocess.CalledProcessError):
            self.verifier.verify(self.assets)

    def test_changed_wheel_is_rejected_even_in_hash_only_mode(self):
        name = sorted(self.verifier.WHEELS)[0]
        (self.assets / name).write_bytes(b'changed payload')
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            self.verifier.verify(self.assets, hashes_only=True)

    def test_downloaded_metadata_cannot_replace_reviewed_metadata(self):
        (self.assets / 'manifest.json').write_text('{}')
        with self.assertRaisesRegex(ValueError, 'differs from reviewed bytes'):
            self.verifier.verify(self.assets, hashes_only=True)


if __name__ == '__main__':
    unittest.main()
