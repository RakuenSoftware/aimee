#!/usr/bin/env python3
"""Safe failure diagnostics for the real-provider deployment fixture."""
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('provider_boundary', ROOT / 'tests/e2e/memory-provider-boundary-e2e.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ProviderBoundaryDiagnosticsTests(unittest.TestCase):
    def test_known_model_refusal_and_capture_count(self):
        result = MODULE.request_outcome(404, {'error': {'message': 'the requested model is not available'}}, 0)
        self.assertEqual(result['error_category'], 'model_unavailable')
        self.assertEqual(result['http_status'], 404)
        self.assertEqual(result['provider_requests'], 0)

    def test_unknown_error_is_hashed_without_prompt_or_secret(self):
        secret = 'synthetic secret credential and prompt body'
        result = MODULE.request_outcome(503, {'error': {'message': secret}, 'prompt': secret,
                                             'headers': {'Authorization': secret}}, 1)
        self.assertEqual(result['error_category'], 'other')
        self.assertEqual(result['error_message_sha256'], hashlib.sha256(secret.encode()).hexdigest())
        self.assertEqual(result['error_message_bytes'], len(secret.encode()))
        self.assertNotIn(secret, json.dumps(result))
        self.assertEqual(result['provider_requests'], 1)

    def test_success_answer_is_never_retained_as_error(self):
        result = MODULE.request_outcome(200, {'choices': [{'message': {'content': 'private answer'}}]}, 1)
        self.assertEqual(result['error_category'], 'none_recorded')
        self.assertEqual(result['error_message_bytes'], 0)
        self.assertNotIn('private answer', json.dumps(result))


if __name__ == '__main__':
    unittest.main()
