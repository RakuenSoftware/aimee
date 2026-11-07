#!/usr/bin/env python3
"""Provenance and capability-preserving deployment selection, without Docker."""
import hashlib
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('deployment_matrix', ROOT / 'tests/e2e/deployment-matrix.py')
MATRIX = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MATRIX)


class ReferenceSelectionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.binary = self.root / 'aimee-module-memory'
        self.binary.write_bytes(b'unit-test-artifact-not-a-functional-owner')
        self.binary.chmod(0o755)
        self.provenance = json.loads((ROOT / 'tests/fixtures/legacy-memory-owner.json').read_text())
        self.provenance['binary_sha256'] = hashlib.sha256(self.binary.read_bytes()).hexdigest()
        self.sidecar = Path(str(self.binary) + '.json')
        self.sidecar.write_text(json.dumps(self.provenance))

    def test_exact_provenance_required(self):
        MATRIX.validate_reference_owner(self.binary)
        for field, value in [('source_commit', '0' * 40), ('purpose', 'shipped'), ('binary_sha256', '0' * 64)]:
            with self.subTest(field=field):
                changed = dict(self.provenance, **{field: value})
                self.sidecar.write_text(json.dumps(changed))
                with self.assertRaises(ValueError):
                    MATRIX.validate_reference_owner(self.binary)
        self.sidecar.write_text(json.dumps(self.provenance))
        self.binary.write_bytes(b'tampered')
        with self.assertRaises(ValueError):
            MATRIX.validate_reference_owner(self.binary)

    def test_symlinks_rejected(self):
        link = self.root / 'linked'
        link.symlink_to(self.binary)
        with self.assertRaises(ValueError):
            MATRIX.validate_reference_owner(link)
        actual = self.root / 'sidecar'
        self.sidecar.rename(actual)
        self.sidecar.symlink_to(actual)
        with self.assertRaises(ValueError):
            MATRIX.validate_reference_owner(self.binary)

    def test_only_executable_is_changed(self):
        manifest = 'config\t/bin/config\nmemory\t/bin/memory\npostgres\t/bin/postgres'
        grant = 'version=1\nprincipal_ref=7\nuid=self\nexecutable=/bin/memory\nserve=5889,6143\nrequest=\n'
        with patch.object(MATRIX, 'command', side_effect=[manifest, grant]) as call:
            overlay = MATRIX.reference_overlay('server', 'candidate:image', self.binary, self.root, 'test')
        self.assertEqual((self.root / 'test-reference.modules').read_text(),
                         manifest.replace('memory\t/bin/memory', 'memory\t' + MATRIX.REFERENCE_TARGET) + '\n')
        self.assertEqual((self.root / 'test-reference.grant').read_text(),
                         grant.replace('executable=/bin/memory', 'executable=' + MATRIX.REFERENCE_TARGET))
        self.assertEqual(overlay.count('read_only: true'), 3)
        self.assertIn(MATRIX.REFERENCE_TARGET, overlay)
        self.assertNotIn('target: "/usr/local/libexec', overlay)
        self.assertTrue(all('--network' in row.args and '--read-only' in row.args for row in call.call_args_list))
        saved = json.loads((self.root / 'test-reference.json').read_text())
        self.assertEqual(saved['purpose'], 'external-reference-not-shipped')
        self.assertEqual(saved['binary_sha256'], self.provenance['binary_sha256'])

    def test_missing_container_diagnostics_still_clean_up_failed_stack(self):
        import types
        stack = unittest.mock.Mock(role='kb', application='missing-app', embedder='missing-embedder')
        stack.start.side_effect = RuntimeError('startup refused')
        def command(*args, **kwargs):
            if args[:2] == ('docker', 'inspect'):
                raise RuntimeError('container absent')
            return '{}'
        output = self.root / 'evidence'
        with patch.object(MATRIX, 'Stack', return_value=stack), \
             patch.object(MATRIX, 'command', side_effect=command), \
             patch.object(MATRIX.subprocess, 'run', return_value=types.SimpleNamespace(stdout='', stderr='')), \
             patch.dict(MATRIX.os.environ, dict(AIMEE_APPLICATION_IMAGE='app', AIMEE_POSTGRES_IMAGE='pg', AIMEE_EMBEDDER_IMAGE='embed')), \
             patch('sys.argv', ['deployment-matrix.py', '--topology', 'T1', '--output', str(output)]):
            self.assertEqual(MATRIX.main(), 1)
        stack.compose.assert_called_once_with('down', '--volumes', '--remove-orphans')
        self.assertEqual(json.loads((output / 'memory-failures.json').read_text()),
                         [dict(role='kb', diagnostics='unavailable')])

    def test_ambiguous_or_mismatched_owner_refused(self):
        for manifest, grant in [('memory\t/bin/memory\nmemory\t/bin/other', 'executable=/bin/memory'),
                                ('memory\t/bin/memory', 'executable=/bin/other'),
                                ('config\t/bin/config', 'executable=/bin/memory')]:
            with self.subTest(manifest=manifest), patch.object(MATRIX, 'command', side_effect=[manifest, grant]):
                with self.assertRaises(ValueError):
                    MATRIX.reference_overlay('kb', 'candidate', self.binary, self.root, 'test')


if __name__ == '__main__':
    unittest.main()
