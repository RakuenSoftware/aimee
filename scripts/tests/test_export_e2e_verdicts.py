#!/usr/bin/env python3
"""Native proof/source objects must never escape in CI verdict artifacts."""
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


export = load('verdict_export', ROOT / 'scripts/validation/export-e2e-verdicts.py')
native = load('native_async', ROOT / 'tests/e2e/memory-native-async-e2e.py')


class VerdictPrivacy(unittest.TestCase):
    def test_native_errors_keep_only_closed_labels_lengths_and_hashes(self):
        private = 'synthetic-private-source-and-authority'
        events = [{'type': 'error', 'message': private, 'capability': {'value': private}},
                  {'type': private, 'status': 'failed'}]
        summary = native.safe_event_summary(events)
        self.assertNotIn(private, json.dumps(summary))
        self.assertEqual(summary['labels'], {'error': 1, 'failed': 1})
        self.assertEqual(summary['error_fingerprints'][0],
                         {'bytes': len(private), 'sha256': hashlib.sha256(private.encode()).hexdigest()})
        self.assertFalse(summary['raw_data_exported'])

    def test_legacy_terminal_envelope_is_observable_without_its_text(self):
        private = 'synthetic-private-terminal-text'
        summary = native.safe_event_summary([{'t': 'legacy_terminal', 'text': private}])
        self.assertNotIn(private, json.dumps(summary))
        self.assertTrue(summary['error_fingerprints'])
        self.assertEqual(summary['labels'], {})

    def test_verdict_discards_proofs_receipts_environment_and_failure_payloads(self):
        private = 'synthetic-private-source-and-authority'
        source = dict(checks=[dict(name='reviewed assertion', passed=False, details=private)],
                      source_contracts=[dict(owner_attestation=private)], receipt_attempts=[private],
                      environment=dict(token=private), provider_intervention_errors=[private],
                      runs=[dict(name='primary native recovery', status=200, provider_requests=1,
                                 provider_request_bytes=[48581], payload=private, run_id=private)],
                      diagnostics=dict(primary_native_recovery=dict(event_count=1, raw=private,
                          labels={private: 1, 'failed': 1}, error_fingerprints=[
                              dict(bytes=len(private), sha256=hashlib.sha256(private.encode()).hexdigest(), raw=private)]),
                          stable_refresh_failure=dict(events=private)))
        raw = json.dumps(source).encode()
        result = export.verdict(source, raw)
        self.assertNotIn(private, json.dumps(result))
        self.assertEqual(result['checks'], [dict(name='reviewed assertion', passed=False)])
        self.assertEqual(result['counts'], dict(total=1, passed=0, failed=1))
        self.assertEqual(result['source_sha256'], hashlib.sha256(raw).hexdigest())
        self.assertEqual(result['runs'][0]['provider_requests'], 1)
        self.assertEqual(set(result['diagnostics']), {'primary_native_recovery'})

    def test_malformed_optional_metadata_never_becomes_payload(self):
        result = export.verdict(dict(runs=None, diagnostics=dict(primary_native_recovery=dict(
            error_fingerprints=None, labels=['untrusted']))), b'{}')
        self.assertEqual(result['checks'], [])
        self.assertEqual(result['diagnostics']['primary_native_recovery']['error_fingerprints'], [])


if __name__ == '__main__':
    unittest.main()
