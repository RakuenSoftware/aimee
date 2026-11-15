"""The vendored memory bundle is refused when it is stale, altered or incomplete."""
import hashlib
import importlib.util
import io
import json
import tarfile
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    'check_memory_bundle', Path(__file__).resolve().parents[1] / 'check_memory_bundle.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

PLATFORM = 'linux-test'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def archive(files, headers, platform=PLATFORM, links=()):
    manifest = {'schema_version': 1, 'platform': platform, 'contains_source': False, 'aims_db_commit': 'a' * 40,
                'aimee_commit': 'b' * 40, 'files': {name: digest(data) for name, data in files.items()},
                'public_headers': headers}
    payload = dict(files, **{'MANIFEST.json': json.dumps(manifest).encode()})
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w:xz') as tar:
        for name, data in payload.items():
            info = tarfile.TarInfo(f'{platform}/{name}')
            info.size, info.mode = len(data), 0o755 if name.startswith('bin/') else 0o644
            tar.addfile(info, io.BytesIO(data))
        for name in links:
            info = tarfile.TarInfo(f'{platform}/{name}')
            info.type, info.linkname = tarfile.SYMTYPE, '/etc/passwd'
            tar.addfile(info)
    return raw.getvalue()


class Tree:
    def __init__(self, work, files=None, links=()):
        self.root = Path(work)
        header = self.root / 'src/headers/shape.h'
        header.parent.mkdir(parents=True)
        header.write_text('struct shape { int a; };\n')
        self.headers = {'src/headers/shape.h': digest(header.read_bytes())}
        self.files = files if files is not None else {name: name.encode() for name in module.REQUIRED}
        vendor = self.root / module.VENDOR
        vendor.mkdir(parents=True)
        self.archive = vendor / f'{PLATFORM}.tar.xz'
        self.archive.write_bytes(archive(self.files, self.headers, links=links))
        self.recorded = self.run('--record', str(self.archive))

    def run(self, *argv):
        return module.main(['--root', str(self.root), *argv])


class CheckMemoryBundle(unittest.TestCase):
    def test_matching_bundle_passes_and_unpacks_verified_files(self):
        with tempfile.TemporaryDirectory() as work:
            tree = Tree(work)
            self.assertEqual(tree.recorded, 0)
            self.assertEqual(tree.run(), 0)
            out = Path(work) / 'out'
            self.assertEqual(tree.run('--platform', PLATFORM, '--unpack', str(out)), 0)
            self.assertEqual((out / 'provision/prepare.py').read_bytes(), b'provision/prepare.py')
            self.assertTrue((out / 'bin/aims-db-module').stat().st_mode & 0o100)
            self.assertFalse((out / 'link/overlay-link.mk').stat().st_mode & 0o100)

    def test_changed_or_removed_public_header_makes_the_bundle_stale(self):
        with tempfile.TemporaryDirectory() as work:
            tree = Tree(work)
            (tree.root / 'src/headers/shape.h').write_text('struct shape { int a; int b; };\n')
            self.assertEqual(tree.run(), 1)
            (tree.root / 'src/headers/shape.h').unlink()
            self.assertEqual(tree.run(), 1)

    def test_altered_archive_and_unknown_platform_are_refused(self):
        with tempfile.TemporaryDirectory() as work:
            tree = Tree(work)
            tree.archive.write_bytes(archive(dict(tree.files, **{'bin/aims-db-module': b'other'}), tree.headers))
            self.assertEqual(tree.run(), 1)
            self.assertEqual(tree.run('--platform', 'linux-none'), 1)

    def test_incomplete_bundle_and_non_regular_members_are_refused_at_unpack(self):
        with tempfile.TemporaryDirectory() as work:
            tree = Tree(work, files={'bin/aims-db-module': b'only'})
            self.assertEqual(tree.run(), 0)
            self.assertEqual(tree.run('--platform', PLATFORM, '--unpack', str(Path(work) / 'out')), 1)
        with tempfile.TemporaryDirectory() as work:
            self.assertEqual(Tree(work, links=('bin/evil',)).recorded, 1)

    def test_missing_index_fails(self):
        with tempfile.TemporaryDirectory() as work:
            self.assertEqual(module.main(['--root', work]), 1)


if __name__ == '__main__':
    unittest.main()
