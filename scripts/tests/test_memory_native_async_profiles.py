#!/usr/bin/env python3
"""Declared async profiles preserve strict shared-client qualification boundaries."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('native_async_profiles', ROOT / 'tests/e2e/memory-native-async-e2e.py')
ASYNC = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ASYNC)


class NativeAsyncProfiles(unittest.TestCase):
    @staticmethod
    def require(name, passed):
        if not passed:
            raise AssertionError(name)

    @staticmethod
    def unavailable():
        return dict(status='unavailable', message='memory bus service unavailable or unsupported',
                    retryable=True, dependency='kb')

    def test_owner_client_preserves_remote_shared_scenarios(self):
        api = Mock(side_effect=AssertionError('profile probing must not replace shared scenarios'))
        self.assertTrue(ASYNC.shared_lane_enabled('owner-client', True, api, self.require))
        self.assertFalse(ASYNC.shared_lane_enabled('owner-client', False, api, self.require))
        api.assert_not_called()

    def test_explicit_reference_requires_exact_unavailable(self):
        api = Mock(return_value=(200, self.unavailable()))
        self.assertFalse(ASYNC.shared_lane_enabled('public-reference', True, api, self.require))
        api.assert_called_once_with('/v1/memory/recall', dict(store='kb', query='native-profile-boundary', scope='all'))

    def test_reference_rejects_failures_or_data_instead_of_skipping(self):
        for status, changes in [(503, {}), (200, {'status': 'error'}), (200, {'retryable': False}),
                                (200, {'dependency': 'other'}), (200, {'recall': {}}),
                                (200, {'memory': None}), (200, {'results': []})]:
            with self.subTest(status=status, changes=changes):
                reply = dict(self.unavailable(), **changes)
                with self.assertRaises(AssertionError):
                    ASYNC.shared_lane_enabled('public-reference', True, Mock(return_value=(status, reply)), self.require)

    def test_unknown_profile_never_downgrades(self):
        with self.assertRaises(ValueError):
            ASYNC.shared_lane_enabled('unknown', True, Mock(), self.require)

    def test_inside_parser_defaults_to_owner_client_and_propagates_explicit_profile(self):
        for args, expected in [([], 'owner-client'), (['--shared-profile', 'public-reference'], 'public-reference')]:
            with self.subTest(profile=expected), patch('sys.argv', ['fixture', '--inside', '--output', 'result.json', *args]), \
                 patch.object(ASYNC, 'inside', return_value=0) as inside:
                self.assertEqual(ASYNC.main(), 0)
                inside.assert_called_once_with('result.json', expected)


if __name__ == '__main__':
    unittest.main()
