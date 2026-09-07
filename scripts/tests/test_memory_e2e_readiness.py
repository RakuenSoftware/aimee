"""Recovery gates must distinguish lexical availability from semantic recall."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location(
    'local_model_gate', Path(__file__).resolve().parents[2] / 'tests/e2e/local-model-memory-e2e.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
placement = module.placement


def recall(*ids, status='ok', code=200):
    return [code, dict(status=status, recall=dict(active_context=[
        dict(memory_id=mid) for mid in ids]))]


def contains_fixture(body):
    return any(row['memory_id'] == 42 for row in body['recall']['active_context'])


class MemoryReadinessTests(unittest.TestCase):
    def test_wait_requires_success_and_expected_memory(self):
        gate = placement.Gate(SimpleNamespace())
        gate.call = Mock(side_effect=[
            subprocess.CalledProcessError(1, ['docker']), ValueError('not JSON'),
            recall(42, code=502), recall(42, status='error'),
            recall(), recall(99), recall(42)])
        with patch.object(placement.time, 'sleep'), patch.object(placement.time, 'monotonic', return_value=0):
            result = gate.wait('recall', {'query': 'cycling'}, predicate=contains_fixture)
        self.assertEqual(result, recall(42))
        self.assertEqual(gate.call.call_count, 7)

    def test_wait_without_predicate_preserves_lexical_recovery(self):
        gate = placement.Gate(SimpleNamespace())
        gate.call = Mock(return_value=recall())
        self.assertEqual(gate.wait('recall'), recall())
        gate.call.assert_called_once()

    def test_missing_semantic_result_still_fails_at_deadline(self):
        gate = placement.Gate(SimpleNamespace())
        gate.call = Mock(return_value=recall())
        with patch.object(placement.time, 'sleep'), patch.object(
                placement.time, 'monotonic', side_effect=[0, 0, 1, 2]):
            with self.assertRaisesRegex(RuntimeError, 'service did not recover: recall'):
                gate.wait('recall', predicate=contains_fixture, timeout=2)
        self.assertEqual(gate.call.call_count, 2)

    def test_semantic_gate_waits_through_restart_and_embedder_recovery(self):
        # Drive the real gate orchestration and wait helper with a recovering
        # service. Each transition first serves a valid lexical-only response.
        gate = placement.Gate(SimpleNamespace())
        gate.call = Mock(side_effect=[
            [200, dict(status='ok', id=42)],
            recall(), recall(42),       # initial semantic readiness
            recall(), recall(42),       # Server restart
            recall(42),                 # lexical recall during model outage
            recall(), recall(42),       # embedder recovery
            recall(),                   # expired fixture
            [200, dict(status='ok')],    # delete
            recall(),                   # retired fixture
        ])
        gate.cli = Mock(return_value=recall(42)[1])

        def docker(*args, **kwargs):
            if args[0] == 'exec':
                return '384' if 'vector_dims' in args[-1] else '1'
            return ''

        gate.docker = Mock(side_effect=docker)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'verdicts.json'
            argv = ['gate', '--server', 'server', '--store-db', 'store',
                    '--embedder', 'embedder', '--output', str(output)]
            with patch.object(module.placement, 'Gate', return_value=gate), patch(
                    'sys.argv', argv), patch.object(placement.time, 'sleep'), patch.object(
                    placement.time, 'monotonic', return_value=0):
                self.assertEqual(module.main(), 0)
            checks = json.loads(output.read_text())
        self.assertTrue(all(check['passed'] for check in checks))
        self.assertIn('semantic recall survives Server restart', [check['name'] for check in checks])
        self.assertEqual(gate.call.call_count, 11)


if __name__ == '__main__':
    unittest.main()
