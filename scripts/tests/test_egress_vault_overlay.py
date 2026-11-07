#!/usr/bin/env python3
"""The hardened egress fixture must retain its explicitly selected owner mounts."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import yaml

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('egress_overlay_gate', ROOT / 'tests/e2e/egress-vault-e2e.py')
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)

class ProbeOverlayTests(unittest.TestCase):
    def test_reference_owner_and_all_exact_grants_survive_network_extension(self):
        for role in ('kb', 'server'):
            with self.subTest(role=role), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary)
                owner = output / 'aimee-module-memory'
                manifest = 'memory\t/shipped/memory\nconfig\t/shipped/config'
                grant = 'version=1\nprincipal_ref=7\nexecutable=/shipped/memory\nrequest=73\nserve=5889\n'
                with patch.object(GATE.matrix, 'validate_reference_owner', return_value=(owner, {})), \
                     patch.object(GATE.matrix, 'command', side_effect=[manifest, grant, grant, grant]):
                    stack = GATE.matrix.Stack(role, {'AIMEE_APPLICATION_IMAGE': 'fixture'}, output, owner)
                original = stack.network_override.read_text()
                GATE.configure_probe_overlay(stack, output / 'probe', 'owned-network')
                text = stack.network_override.read_text()
                self.assertTrue(text.startswith(original))
                self.assertEqual(text.count('    volumes:\n'), 1)
                document = yaml.load(text, Loader=yaml.BaseLoader)
                service = document['services']['aimee-' + role]
                mounts = service['volumes']
                self.assertEqual(len(mounts), 6)
                targets = {entry['target'] for entry in mounts if isinstance(entry, dict)}
                self.assertEqual(targets, {GATE.matrix.REFERENCE_TARGET,
                    f'/opt/aimee/module-grants/{role}.modules',
                    *(f'/opt/aimee/module-grants/{role}/{name}.grant' for name in
                      ('memory', 'memory-postgres', 'memory-egress'))})
                self.assertTrue(all(entry['read_only'] == 'true' for entry in mounts if isinstance(entry, dict)))
                self.assertIn(str(output / 'probe') + ':/opt/aimee-e2e:ro', mounts)
                self.assertEqual(set(service['networks']), {'default', 'store', 'models', 'vault-fixture'})
                self.assertEqual(document['networks']['vault-fixture'],
                                 {'external': 'true', 'name': 'owned-network'})
                self.assertEqual(service['ports'], [])

    def test_owner_free_fixture_remains_owner_free(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            stack = GATE.matrix.Stack('server', {'AIMEE_APPLICATION_IMAGE': 'fixture'}, output)
            GATE.configure_probe_overlay(stack, output / 'probe', 'owned-network')
            document = yaml.load(stack.network_override.read_text(), Loader=yaml.BaseLoader)
            self.assertEqual(document['services']['aimee-server']['volumes'],
                             [str(output / 'probe') + ':/opt/aimee-e2e:ro'])
            self.assertNotIn(GATE.matrix.REFERENCE_TARGET, stack.network_override.read_text())

if __name__ == '__main__':
    unittest.main()
