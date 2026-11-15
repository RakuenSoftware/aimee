import importlib.util
import os
import subprocess
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('compose_vault', Path(__file__).resolve().parents[1] / 'compose-vault-init.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def fixture(role='server'):
    return {'x-aimee-vault': {'AIMEE_KB_API_BEARER_TOKEN': 'fixture-authority'}, 'services': {'aimee-' + role: {},
                         'aimee-store-db': {'environment': {'AIMEE_STORE_RUNTIME_PASSWORD': "a' b\\c",
                             'AIMEE_STORE_MIGRATOR_PASSWORD': 'fixture-migrator'}}}}


class ComposeVaultTests(unittest.TestCase):
    def test_launcher_uses_docker_context_without_client_kernel_discovery(self):
        # A Windows/remote Docker client cannot identify the daemon's devices by
        # reading its own kernel. Simulate a non-Linux invoking machine.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, body in {'uname': 'echo Windows_NT', 'python3': 'exit 0',
                               'docker': 'printf "%s\\n" "$@" > "$LAUNCHER_ARGUMENTS"'}.items():
                executable = root / name
                executable.write_text('#!/bin/sh\n' + body + '\n')
                executable.chmod(0o755)
            output = root / 'arguments'
            script = Path(__file__).resolve().parents[1] / 'compose-local.sh'
            result = subprocess.run(['sh', str(script), '-f', 'compose.yaml', 'config'],
                                    env=dict(os.environ, PATH=str(root) + os.pathsep + os.environ['PATH'],
                                             LAUNCHER_ARGUMENTS=str(output)), capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertEqual(output.read_text().splitlines(), ['compose', '-f', 'compose.yaml', 'config'])

    def test_only_real_up_mutates(self):
        self.assertEqual(module.up_prefix(['-p', 'up', '-f', 'instance.yaml', 'up', '-d']), ['-p','up','-f','instance.yaml'])
        for args in (['config'], ['--dry-run', 'up'], ['-p', 'x', 'down'], ['-f'], ['logs']):
            self.assertIsNone(module.up_prefix(args))

    def test_typed_quoted_database_credentials(self):
        [(owner, packet)] = module.payloads(fixture())
        self.assertEqual(owner, 'aimee-server')
        self.assertIn("password='a\\' b\\\\c'", packet)
        self.assertIn('sslmode=verify-full', packet)
        self.assertNotIn('AIMEE_DB2_URL=', packet)
        self.assertNotIn('LUKS', packet)
        self.assertEqual(packet.count('\0'), 2)

    def test_kb_owns_all_its_credentials(self):
        model = fixture('kb')
        model['x-aimee-vault']['AIMEE_KB_SERVICE_IDENTITY_TOKEN'] = 'fixture-identity'
        [(owner, packet)] = module.payloads(model)
        self.assertEqual(owner, 'aimee-kb')
        self.assertNotIn('AIMEE_DB2_URL=', packet)
        self.assertIn('AIMEE_STORE_MIGRATION_URL=', packet)
        self.assertIn('AIMEE_KB_API_BEARER_TOKEN=fixture-authority\0', packet)
        self.assertIn('AIMEE_KB_SERVICE_IDENTITY_TOKEN=fixture-identity\0', packet)

    def test_invalid_or_missing_inputs_fail_before_bootstrap(self):
        model = fixture()
        model['services']['aimee-store-db']['environment']['AIMEE_STORE_RUNTIME_PASSWORD'] = 'x\0injection'
        with self.assertRaises(ValueError): module.payloads(model)
        model = fixture('kb'); del model['x-aimee-vault']
        with self.assertRaises(ValueError): module.payloads(model)
        model = fixture(); model['services']['aimee-kb'] = {}
        with self.assertRaises(ValueError): module.payloads(model)

    def test_explicit_migration_is_scoped_and_not_persistent(self):
        calls = []
        def run(argv, **kwargs):
            calls.append((argv, kwargs))
            return module.json.dumps(fixture('kb')).encode() if 'config' in argv else b''
        with patch.object(module, 'run', run):
            module.main(['--migrate-store-connections', '-p', 'upgrade', 'up'])
        args, options = calls[-1]
        self.assertIn('AIMEE_VAULT_STORE_MIGRATION=1', args)
        self.assertNotIn('AIMEE_VAULT_ENV_OVERWRITE=1', args)
        self.assertNotIn(b'AIMEE_DB2_URL=', options['input'])
        self.assertIn(b'AIMEE_STORE_MIGRATION_URL=', options['input'])
        with self.assertRaises(ValueError):
            module.main(['--migrate-store-connections', 'config'])

    def composition(self, embedder, identity_output=b"", mode="on"):
        """Run main() against a model with the memory service; return (events, provisioned identities)."""
        events, provisioned = [], []
        def run(argv, **kwargs):
            events.append(argv)
            model = fixture(); model['services']['aimee-memory-init'] = {'environment': {"AIMEE_MEMORY_PROVISION": mode}}
            if embedder: model['services']['aimee-embedder'] = {}
            if 'config' in argv: return module.json.dumps(model).encode()
            return identity_output if 'aimee-embedder' in argv else b''
        def provision(compose, identity):
            events.append(['provision', *compose]); provisioned.append(identity)
        with patch.object(module, 'run', run), patch.object(module, 'provision_memory', provision):
            self.assertEqual(module.main(['-p', 'isolated', 'up', '-d']), 0)
        return events, provisioned

    def test_memory_is_provisioned_before_the_application_image_runs(self):
        events, provisioned = self.composition(embedder=False)
        self.assertEqual(provisioned, [b''])  # no local embedder service: no identity to pin
        order = [event[0] if event[0] == 'provision' else ('vault' if '--bootstrap-vault-stdin' in event else None)
                 for event in events]
        self.assertEqual([step for step in order if step], ['provision', 'vault'])

    def test_embedder_identity_is_read_from_the_image_and_handed_to_provisioning(self):
        identity = {'status': 'ok', 'dim': 4, 'embedding_identity': {'model': 'm'}, 'embedding_identity_digest': 'sha256:x'}
        events, provisioned = self.composition(
            embedder=True, identity_output=b'loader chatter\n' + module.json.dumps(identity).encode() + b'\n')
        reader = next(event for event in events if 'aimee-embedder' in event)
        self.assertEqual(reader[-3:], [module.EMBEDDER_PYTHON, 'aimee-embedder', '-'])
        self.assertIn('--no-deps', reader)
        self.assertEqual(module.json.loads(provisioned[0]), identity)

    def test_provisioning_off_does_not_need_the_embedder_identity(self):
        events, provisioned = self.composition(embedder=True, identity_output=b"no identity here", mode="off")
        self.assertEqual(provisioned, [b''])
        self.assertFalse([event for event in events if 'aimee-embedder' in event])

    def test_embedder_without_an_identity_stops_startup(self):
        for output in (b'{"status": "ok"}\n', b'', b'Traceback: AttributeError\n'):
            with self.assertRaises(module.MemoryStartup) as raised:
                self.composition(embedder=True, identity_output=output)
            self.assertIn('embedder image of this release', str(raised.exception))

    def test_failed_provisioning_shows_its_message_and_stops(self):
        completed = subprocess.CompletedProcess([], 1, b'[memory-provision] fatal: stop the application\nnoise\n', b'')
        with patch.object(module.subprocess, 'run', return_value=completed) as called, \
                patch.object(module.sys, 'stderr') as stderr, self.assertRaises(module.MemoryStartup):
            module.provision_memory(['docker', 'compose'], b'{}')
        self.assertEqual(called.call_args.args[0][-1], 'aimee-memory-init')
        self.assertEqual(called.call_args.kwargs['input'], b'{}')
        shown = ''.join(call.args[0] for call in stderr.write.call_args_list)
        self.assertIn('stop the application', shown)
        self.assertNotIn('noise', shown)

    def test_secret_packet_is_stdin_only(self):
        calls = []
        def run(argv, **kwargs):
            calls.append((argv, kwargs))
            return module.json.dumps(fixture()).encode() if 'config' in argv else b''
        with patch.object(module, 'run', run):
            self.assertEqual(module.main(['-p','isolated','up','-d']), 0)
        args, options = calls[-1]
        self.assertIn('--bootstrap-vault-stdin', args)
        self.assertIn('--rm', args)
        self.assertNotIn('AIMEE_VAULT_STORE_MIGRATION=1', args)
        self.assertNotIn('fixture-migrator', ' '.join(args))
        self.assertIn(b'fixture-migrator', options['input'])


if __name__ == '__main__': unittest.main()
