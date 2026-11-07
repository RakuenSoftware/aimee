"""Make graph probes must not remove or rewrite an existing build tree."""
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class MakeMetadataReadOnlyTest(unittest.TestCase):
    def test_integrity_database_queries_are_read_only(self):
        import re
        script = (ROOT/'src/tests/test_build_integrity.sh').read_text()
        probes = re.findall(r'make (-[a-z]+) 2>/dev/null', script)
        self.assertGreaterEqual(len(probes), 2)
        self.assertIn('make -n -f "$makefile_file" -f /tmp/aimee_bi_print.mk print-DEPS', script)
        self.assertTrue(all('q' in flags for flags in probes if 'p' in flags), probes)

    def test_changed_branch_and_version_do_not_mutate_probe_object_tree(self):
        with tempfile.TemporaryDirectory(prefix='aimee-make-probe-') as temporary:
            objects = Path(temporary)/'objects'
            objects.mkdir()
            (objects/'branch.txt').write_text('different-existing-branch\n')
            (objects/'version.txt').write_text('different-existing-version\n')
            (objects/'live-build.o').write_bytes(b'owned sentinel, not a real object')
            def snapshot():
                return {str(p.relative_to(objects)): hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in objects.rglob('*') if p.is_file()}
            before = snapshot()
            for flags in ('-n', '-q', '-qp'):
                with self.subTest(flags=flags):
                    result = subprocess.run(['make', '--no-print-directory', '-C', str(ROOT/'src'),
                        flags, 'OBJDIR='+str(objects), 'GIT_VERSION=probe-version',
                        '--eval=.PHONY: metadata-probe', '--eval=metadata-probe:; @:',
                        'metadata-probe'], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                        text=True, timeout=60)
                    self.assertIn(result.returncode, (0, 1), result.stderr)
                    self.assertEqual(snapshot(), before)

if __name__ == '__main__':
    unittest.main()
