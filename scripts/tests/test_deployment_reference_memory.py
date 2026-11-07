#!/usr/bin/env python3
"""Provenance and capability-preserving deployment selection, without Docker."""
import hashlib
import importlib.util
import json
import io
import contextlib
import http.client
import pathlib
import types
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
        with patch.object(MATRIX, 'command', side_effect=[manifest, grant, grant, grant]) as call:
            overlay = MATRIX.reference_overlay('server', 'candidate:image', self.binary, self.root, 'test')
        self.assertEqual((self.root / 'test-reference.modules').read_text(),
                         manifest.replace('memory\t/bin/memory', 'memory\t' + MATRIX.REFERENCE_TARGET) + '\n')
        self.assertEqual((self.root / 'test-reference.grant').read_text(),
                         grant.replace('executable=/bin/memory', 'executable=' + MATRIX.REFERENCE_TARGET))
        self.assertEqual(overlay.count('read_only: true'), 5)
        self.assertIn(MATRIX.REFERENCE_TARGET, overlay)
        self.assertNotIn('target: "/usr/local/libexec', overlay)
        self.assertTrue(all('--network' in row.args and '--read-only' in row.args for row in call.call_args_list))
        for name in ('memory-postgres', 'memory-egress'):
            self.assertEqual((self.root / ('test-reference.' + name + '.grant')).read_text(),
                             grant.replace('executable=/bin/memory', 'executable=' + MATRIX.REFERENCE_TARGET))
            self.assertIn('/server/' + name + '.grant', overlay)
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

    def test_outbound_grants_must_match_selected_service_executable(self):
        manifest = 'memory\t/bin/memory'
        grant = 'executable=/bin/memory\nrequest=11266\n'
        for outgoing in ('memory-postgres', 'memory-egress'):
            grants = [grant, grant, grant]
            grants[1 if outgoing == 'memory-postgres' else 2] = 'executable=/bin/wrong\n'
            with self.subTest(outgoing=outgoing), patch.object(
                    MATRIX, 'command', side_effect=[manifest, *grants]):
                with self.assertRaises(ValueError):
                    MATRIX.reference_overlay('kb', 'candidate', self.binary, self.root, 'bad')

    def test_health_projection_never_exports_free_text(self):
        secret = 'private-credential-and-request-body'
        payload = dict(status=secret, ready=secret, postgres_ok=False, reason=secret,
                       dependencies=dict(db1=secret, kb='disabled', private=secret),
                       diagnostics=dict(failed_boundary='modules', missing_module=secret,
                                        breaker_state='closed', last_ingest_at=secret),
                       pool=dict(size=8, in_use=secret, available=False), private=secret)
        result = MATRIX.safe_readiness_payload('server', 503, payload)
        self.assertNotIn(secret, json.dumps(result))
        self.assertEqual(result['dependencies']['db1'], 'unknown')
        self.assertEqual(result['diagnostics']['failed_boundary'], 'modules')
        self.assertFalse(result['postgres_ok'])
        self.assertNotIn('ready', result)
        self.assertEqual(result['pool'], dict(size=8, available=False))

    def test_correction_diagnostic_keeps_assertion_evidence_without_content(self):
        result = MATRIX.safe_correction_summary(dict(kind='forbidden',
            message='idempotent mutations require an authenticated principal',
            proposal=dict(content='private-body', payload_digest='private-digest'),
            private='private-identity'), True)
        self.assertEqual(result['kind'], 'forbidden')
        self.assertEqual(result['message_category'], 'retry_principal_absent')
        self.assertTrue(result['authoritative_content_unchanged'])
        self.assertNotIn('private-', json.dumps(result))

    def test_owner_model_transport_cannot_assert_operator_identity(self):
        model = MATRIX.review_action_headers('transport-fixture', 'service-fixture', True)
        self.assertEqual(model, {'Content-Type': 'application/json',
            'Authorization': 'Bearer transport-fixture',
            'X-Aimee-Service-Authorization': 'Bearer service-fixture'})
        operator = MATRIX.review_action_headers('transport-fixture', 'service-fixture', False)
        self.assertEqual(operator['X-Aimee-Caller-Subject'], 'review_operator')

    def test_container_health_outputs_are_discarded(self):
        secret = 'private-health-command-output'
        result = MATRIX.safe_container_state(dict(Status='running', ExitCode=0, OOMKilled=False,
            Health=dict(Status='unhealthy', FailingStreak=3, Log=[dict(ExitCode=1, Output=secret),
                                                               dict(ExitCode=7, Output=secret)])))
        self.assertEqual(result['health_exit_codes'], [1, 7])
        self.assertEqual(result['health_failing_streak'], 3)
        self.assertNotIn(secret, json.dumps(result))

    def test_container_probe_sanitizes_http_and_file_logs_before_returning(self):
        for role in ('server', 'kb'):
            payload = dict(status='degraded', ready=False, postgres_ok=False,
                           private='private-response-secret',
                           dependencies=dict(db1='fail'), diagnostics=dict(missing_module='memory'))
            case = self
            requests = []
            class Connection:
                def __init__(self, *args, **kwargs): pass
                def request(self, *args): requests.append(args)
                def getresponse(self):
                    return types.SimpleNamespace(status=503, read=lambda size: json.dumps(payload).encode())
                def close(self): pass
            class SourceLog:
                def __init__(self, path): pass
                def is_socket(self): return role == 'server'
                def open(self, mode):
                    return io.BytesIO(b'private-log-secret\nINFO ready: memory module unavailable\nring: attach denied\n')
            def execute(*args, **kwargs):
                case.assertEqual(args[:2], ('docker', 'exec'))
                code = args[5]
                capture = io.StringIO()
                with patch.object(http.client, 'HTTPConnection', Connection), \
                     patch.object(pathlib, 'Path', SourceLog), \
                     patch('sys.argv', ['probe', role]), contextlib.redirect_stdout(capture):
                    exec(code, {})
                return capture.getvalue()
            with patch.object(MATRIX, 'command', side_effect=execute):
                result = MATRIX.container_readiness_probe(types.SimpleNamespace(role=role, application='app'))
            self.assertEqual(requests, [('GET', '/v1/ready' if role == 'server' else '/v1/health')])
            self.assertEqual(result['status_code'], 503)
            self.assertEqual(result['server_file_log']['messages']['ring: attach denied'], 1)
            self.assertNotIn('private-', json.dumps(result))

    def test_unavailable_probe_returns_only_fixed_category(self):
        with patch.object(MATRIX, 'command', side_effect=RuntimeError('private-exec-error')):
            self.assertEqual(MATRIX.container_readiness_probe(
                types.SimpleNamespace(role='kb', application='app')), dict(transport='probe_unavailable'))

    def test_no_listener_still_collects_sanitized_source_log(self):
        class Connection:
            def __init__(self, *args, **kwargs): pass
            def request(self, *args): raise ConnectionRefusedError('private-socket-detail')
            def close(self): pass
        class SourceLog:
            def __init__(self, path): pass
            def is_socket(self): return False
            def open(self, mode):
                return io.BytesIO(b'config module unavailable: private-source-detail\n')
        def execute(*args, **kwargs):
            capture = io.StringIO()
            with patch.object(http.client, 'HTTPConnection', Connection), \
                 patch.object(pathlib, 'Path', SourceLog), \
                 patch('sys.argv', ['probe', 'kb']), contextlib.redirect_stdout(capture):
                exec(args[5], {})
            return capture.getvalue()
        with patch.object(MATRIX, 'command', side_effect=execute):
            result = MATRIX.container_readiness_probe(types.SimpleNamespace(role='kb', application='app'))
        self.assertEqual(result['transport'], 'connection_refused')
        self.assertEqual(result['server_file_log']['messages']['config module unavailable'], 1)
        self.assertNotIn('private-', json.dumps(result))


if __name__ == '__main__':
    unittest.main()
