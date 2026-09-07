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
        self.assertIn('AIMEE_DB2_URL=', packet)
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
        self.assertIn(b'AIMEE_DB2_URL=', options['input'])
        with self.assertRaises(ValueError):
            module.main(['--migrate-store-connections', 'config'])

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
