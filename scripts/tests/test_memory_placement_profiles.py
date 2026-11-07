#!/usr/bin/env python3
"""Explicit owner-transport qualification cannot silently certify Server forwarding."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tests/e2e' / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

PLACEMENT = load('placement_profiles', 'memory-placement-e2e.py')
MATRIX = load('placement_transport', 'deployment-matrix.py')

class PlacementProfiles(unittest.TestCase):
    def gate(self, **values):
        return PLACEMENT.Gate(SimpleNamespace(server='server-fixture', **values))

    def test_default_requires_original_forwarding_suite(self):
        gate = self.gate()
        with patch.object(gate, 'hygiene_preview', side_effect=RuntimeError('forwarding required')), \
             patch.object(gate, 'run_public_reference') as reference:
            with self.assertRaisesRegex(RuntimeError, 'forwarding required'):
                gate.run()
        reference.assert_not_called()

    def test_public_profile_is_explicit(self):
        gate = self.gate(shared_profile='public-reference')
        with patch.object(gate, 'run_public_reference', return_value=True) as reference, \
             patch.object(gate, 'hygiene_preview') as forwarding:
            self.assertTrue(gate.run())
        reference.assert_called_once_with()
        forwarding.assert_not_called()

    def test_public_profile_requires_explicit_credentials(self):
        with patch.dict('os.environ', {}, clear=True):
            with self.assertRaisesRegex(RuntimeError, 'explicit owner credentials'):
                self.gate().reference_owner_action('get', {'id': 1})

    def check_refusals(self, http, mcp, digests=('same', 'same')):
        gate = self.gate()
        with patch.object(gate, 'digest', side_effect=digests), \
             patch.object(gate, 'call', side_effect=lambda op, body: (200, mcp) if op == 'recall' else http), \
             patch.object(gate, 'mcp_document', return_value=mcp), \
             contextlib.redirect_stdout(io.StringIO()):
            gate.server_reference_refusals(1)
        return gate.checks

    def test_exact_typed_unavailability_and_immutable_store(self):
        checks = self.check_refusals((503, dict(status='error', kind='unavailable')),
            dict(status='unavailable', message='memory bus service unavailable or unsupported',
                 retryable=True, dependency='kb'))
        self.assertEqual(len(checks), 7)
        self.assertTrue(all(row['passed'] for row in checks))

    def test_success_cannot_be_reclassified_as_capability_gap(self):
        checks = self.check_refusals((200, dict(status='ok', memories=[])), dict(status='ok', id=1))
        self.assertFalse(any(row['passed'] for row in checks[:-1]))

    def test_domain_error_is_not_transport_unavailable(self):
        checks = self.check_refusals((400, dict(status='error', kind='invalid_projection')),
                                    dict(status='error', kind='invalid_projection'))
        self.assertFalse(any(row['passed'] for row in checks[:-1]))

    def test_unavailable_boundary_must_not_change_owner_rows(self):
        checks = self.check_refusals((503, dict(status='error', kind='unavailable')),
            dict(status='unavailable', message='memory bus service unavailable or unsupported',
                 retryable=True, dependency='kb'), ('before', 'changed'))
        self.assertFalse(checks[-1]['passed'])

    def test_actual_embedded_transport_compiles_and_credentials_only_use_stdin(self):
        with patch.object(MATRIX, 'command', return_value=json.dumps([200, {'status': 'ok'}])) as call:
            result = MATRIX.authenticated_reference_owner_action('server-fixture',
                '/v1/actions/memory.update', {'authority': 'user'}, 'test-bearer', 'test-service', True)
        self.assertEqual(result, {'status': 'ok'})
        args, kwargs = call.call_args
        compile(args[-1], '<container owner transport>', 'exec')
        self.assertNotIn('test-bearer', str(args))
        self.assertNotIn('test-service', str(args))
        self.assertTrue(json.loads(kwargs['data'])['model'])
        self.assertIn('ssl.create_default_context', args[-1])
        self.assertIn('context.load_cert_chain', args[-1])
        self.assertEqual(MATRIX.review_action_headers('test', 'test', True).keys(),
            {'Content-Type', 'Authorization', 'X-Aimee-Service-Authorization'})

if __name__ == '__main__':
    unittest.main()
