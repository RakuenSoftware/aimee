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
    def test_stack_waits_for_loaded_embedder_after_application_is_healthy(self):
        spec = importlib.util.spec_from_file_location(
            'deployment_matrix', Path(__file__).resolve().parents[2] / 'tests/e2e/deployment-matrix.py')
        matrix = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(matrix)
        with tempfile.TemporaryDirectory() as directory:
            stack = matrix.Stack('kb', {}, Path(directory))
            embedding = iter(['starting', 'healthy'])
            def command(*args, **kwargs):
                if args[1] != 'inspect':
                    return ''
                if args[-1] == stack.embedder:
                    return next(embedding)
                if '{{.State.Health.Status}}' in args:
                    return 'healthy'
                if '{{json .HostConfig.PortBindings}}' in args:
                    return '{}'
                if '{{json .Config.Env}}' in args:
                    return json.dumps(['AIMEE_PROVIDER_CONTEXT_LIMITS=' + stack.env['AIMEE_PROVIDER_CONTEXT_LIMITS']])
                return '{}'
            with patch.object(matrix, 'command', side_effect=command), patch.object(matrix.time, 'sleep') as sleep:
                stack.start()
            sleep.assert_called_once_with(2)

    def test_split_t2_initializes_its_own_generation_before_server_enrollment(self):
        spec = importlib.util.spec_from_file_location(
            'deployment_matrix', Path(__file__).resolve().parents[2] / 'tests/e2e/deployment-matrix.py')
        matrix = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(matrix)
        for boot_only in (False, True):
            with self.subTest(boot_only=boot_only), tempfile.TemporaryDirectory() as directory:
                kb = Mock(role='kb', application='kb', service_identity='service')
                kb.kb_request.side_effect = [
                    (200, dict(postgres_ok=True)), (200, dict(status='ok'))]
                argv = ['matrix', '--topology', 'T2', '--skip-kb-gates',
                        '--skip-kb-free-gate', '--output', directory]
                if boot_only:
                    argv.append('--boot-only')
                order = []
                def stack(role, *args):
                    order.append(role)
                    if role == 'server':
                        raise RuntimeError('server boundary reached')
                    return kb
                def activate(*args):
                    self.assertIs(args[0], kb)
                    order.append('activate')
                env = dict(AIMEE_APPLICATION_IMAGE='app', AIMEE_POSTGRES_IMAGE='pg',
                           AIMEE_EMBEDDER_IMAGE='embedder')
                with patch.dict(matrix.os.environ, env), patch('sys.argv', argv), patch.object(
                        matrix, 'command'), patch.object(matrix, 'Stack', side_effect=stack), patch.object(
                        matrix, 'application_metadata_is_private', return_value=True), patch.object(
                        matrix, 'activate_embedding_generation', side_effect=activate):
                    self.assertEqual(matrix.main(), 1)
                self.assertEqual(order, ['kb', 'server'] if boot_only else ['kb', 'activate', 'server'])

    def test_generation_setup_restores_service_bearer_when_indexing_fails(self):
        spec = importlib.util.spec_from_file_location(
            'deployment_matrix', Path(__file__).resolve().parents[2] / 'tests/e2e/deployment-matrix.py')
        matrix = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(matrix)
        kb = Mock(application='kb', env=dict(AIMEE_KB_API_BEARER_TOKEN='service', EMBEDDER_URL='embedder'))
        kb.kb_request.side_effect = RuntimeError('indexing failed')
        def command(*args, **kwargs):
            return 'healthy' if args[1] == 'inspect' else ''
        with patch.object(matrix, 'command', side_effect=command) as run:
            with self.assertRaisesRegex(RuntimeError, 'indexing failed'):
                matrix.activate_embedding_generation(kb, Mock(), 'generation')
        self.assertEqual(kb.env['AIMEE_KB_API_BEARER_TOKEN'], 'service')
        bootstraps = [call.kwargs['data'] for call in run.call_args_list if 'data' in call.kwargs]
        self.assertEqual(len(bootstraps), 2)
        self.assertNotEqual(bootstraps[0], bootstraps[1])
        self.assertEqual(bootstraps[1], 'AIMEE_KB_API_BEARER_TOKEN=service\0')

    def test_index_failure_keeps_only_fixed_diagnostics_and_still_fails(self):
        spec = importlib.util.spec_from_file_location(
            'deployment_matrix', Path(__file__).resolve().parents[2] / 'tests/e2e/deployment-matrix.py')
        matrix = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(matrix)
        with tempfile.TemporaryDirectory() as directory:
            kb = Mock(application='kb', output=Path(directory), env=dict(
                AIMEE_KB_API_BEARER_TOKEN='service', EMBEDDER_URL='private-provider'))
            kb.kb_request.return_value = (503, dict(status='error', done=4, failed=True,
                last_id=-1, ready=False, message='private credential', provider='private-provider'))
            def command(*args, **kwargs):
                return 'healthy' if args[1] == 'inspect' else ''
            def check(name, passed):
                if not passed:
                    raise RuntimeError(name)
            with patch.object(matrix, 'command', side_effect=command):
                with self.assertRaisesRegex(RuntimeError, 'Typed fixture indexing succeeds'):
                    matrix.activate_embedding_generation(kb, check, 'generation')
            self.assertEqual(json.loads((Path(directory) / 'typed-index-state.json').read_text()),
                             dict(http_status=503, status='error', done=4, ready=False))
            self.assertEqual(kb.kb_request.call_count, 1)
            self.assertEqual(kb.env['AIMEE_KB_API_BEARER_TOKEN'], 'service')

    def test_direct_stack_users_receive_provider_ceiling_and_preserve_override(self):
        spec = importlib.util.spec_from_file_location(
            'deployment_matrix', Path(__file__).resolve().parents[2] / 'tests/e2e/deployment-matrix.py')
        matrix = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(matrix)
        with tempfile.TemporaryDirectory() as directory:
            for role in ('kb', 'server'):
                default = matrix.Stack(role, {}, Path(directory))
                self.assertEqual(json.loads(default.env['AIMEE_PROVIDER_CONTEXT_LIMITS']),
                                 dict(schema_version=1, max_request_bytes=65536))
                cap = json.dumps(dict(schema_version=1, max_request_bytes=0))
                explicit = matrix.Stack(role, {'AIMEE_PROVIDER_CONTEXT_LIMITS': cap}, Path(directory))
                self.assertEqual(explicit.env['AIMEE_PROVIDER_CONTEXT_LIMITS'], cap)

    def test_sql_diagnostics_expose_only_fixed_sqlstate(self):
        spec = importlib.util.spec_from_file_location(
            'deployment_matrix', Path(__file__).resolve().parents[2] / 'tests/e2e/deployment-matrix.py')
        matrix = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(matrix)
        failed = subprocess.CompletedProcess([], 1, stdout='private rows',
                                             stderr='ERROR:  40P01\nprivate credential material\n')
        with patch.object(matrix.subprocess, 'run', return_value=failed):
            with self.assertRaisesRegex(RuntimeError, r'^docker exec failed with exit 1 \(sqlstate_40P01\)$'):
                matrix.command('docker', 'exec', 'private-container', 'psql', '-c', 'private SQL')

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
