"""Decisions of the first-boot memory provisioning command, without running it."""
import hashlib
import importlib.util
import json
import os
import socket
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'memory_provision', Path(__file__).resolve().parents[2] / 'deploy/container/memory-provision.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

HEALTH = {'status': 'ok', 'dim': 4, 'embedding_identity': {'model': 'm'}, 'embedding_identity_digest': 'sha256:new'}


class Accounts(unittest.TestCase):
    def test_runtime_account_is_always_served_and_a_kb_has_one_owner(self):
        self.assertEqual(module.accounts('server', None), ['aimee'])
        self.assertEqual(module.accounts('server', 'alice, bob'), ['aimee', 'alice', 'bob'])
        self.assertEqual(module.accounts('server', 'alice,aimee'), ['alice', 'aimee'])
        self.assertEqual(module.accounts('kb', 'alice,bob'), ['aimee'])

    def test_invalid_lists_are_refused(self):
        for value in ('a,a', 'has space', ','.join(f'u{i}' for i in range(40)), 'x' * 200):
            with self.assertRaises(module.Refused):
                module.accounts('server', value)


class BrowserAccounts(unittest.TestCase):
    def home(self, work, identities, bootstrap=None, replaced=False):
        home = Path(work); (home / 'webchat').mkdir()
        (home / 'webchat/identities').write_text(identities)
        if bootstrap: (home / 'webchat/bootstrap-user').write_text('generated:' + bootstrap + '\n')
        if replaced: (home / 'webchat/bootstrap-replaced').write_text('')
        return home

    def test_usable_browser_logins_are_served_without_being_listed(self):
        with tempfile.TemporaryDirectory() as work:
            home = self.home(work, 'alice:$y$j9T$abc\nlocked:!$y$j9T$abc\nnopass:\nstar:*\nbob:$6$xyz\n', 'aimee-1a2b')
            self.assertEqual(module.browser_logins(home), ['aimee-1a2b', 'alice', 'bob'])
            self.assertEqual(module.served_accounts('server', 'aimee', home), 'aimee,aimee-1a2b,alice,bob')
            self.assertEqual(module.served_accounts('server', 'bob,carol', home), 'aimee,bob,carol,aimee-1a2b,alice')
            self.assertEqual(module.served_accounts('kb', 'aimee', home), 'aimee')

    def test_replaced_first_boot_login_and_missing_record_serve_nothing_extra(self):
        with tempfile.TemporaryDirectory() as work:
            home = self.home(work, 'alice:$y$abc\n', 'aimee-1a2b', replaced=True)
            self.assertEqual(module.browser_logins(home), ['alice'])
        with tempfile.TemporaryDirectory() as work:
            self.assertEqual(module.browser_logins(Path(work)), [])
            self.assertEqual(module.served_accounts('server', None, Path(work)), 'aimee')

    def test_configured_accounts_win_when_more_than_the_limit_exist(self):
        with tempfile.TemporaryDirectory() as work:
            home = self.home(work, ''.join(f'user{i:02d}:$y$abc\n' for i in range(40)))
            log, module.log = module.log, lambda message: None
            try:
                served = module.served_accounts('server', 'aimee,vip', home).split(',')
            finally:
                module.log = log
            self.assertEqual(len(served), 32)
            self.assertEqual(served[:3], ['aimee', 'vip', 'user00'])


class Policy(unittest.TestCase):
    RECORDED = {'accounts': ['aimee', 'alice'], 'embedding_provider_identity_digest': 'sha256:old', 'node': 'n'}

    def test_unspecified_parts_keep_the_recorded_value(self):
        desired = module.desired_policy('server', self.RECORDED, None, None)
        self.assertEqual(module.policy_changes(self.RECORDED, desired), [])

    def test_changed_accounts_and_provider_are_reported(self):
        desired = module.desired_policy('server', self.RECORDED, 'bob', HEALTH)
        self.assertEqual(desired, {'accounts': ['aimee', 'bob'], 'shared_scopes': [],
                                   'embedding_provider_identity_digest': 'sha256:new', 'typed_facts': None})
        self.assertEqual(module.policy_changes(self.RECORDED, desired), ['accounts', 'embedding provider'])

    def test_first_provisioning_without_an_embedder_has_no_provider(self):
        desired = module.desired_policy('server', None, None, None)
        self.assertEqual(desired, {'accounts': ['aimee'], 'shared_scopes': [],
                                   'embedding_provider_identity_digest': None, 'typed_facts': None})

    def test_kb_serves_global_and_each_declared_scope(self):
        self.assertEqual(module.shared_scopes('server', 'project:apollo'), [])
        self.assertEqual(module.shared_scopes('kb', None), ['global', 'project:*', 'workspace:*'])
        self.assertEqual(module.shared_scopes('kb', 'project:*'), ['global', 'project:*'])
        self.assertEqual(module.shared_scopes('kb', 'project:apollo, workspace:w1'),
                         ['global', 'project:apollo', 'workspace:w1'])
        self.assertEqual(module.shared_scopes('kb', 'project:apollo,global'), ['project:apollo', 'global'])
        for value in ('apollo', 'project:', 'user:alice', 'project:a b', 'project:_global', 'project:x,project:x',
                      ','.join(f'project:p{i}' for i in range(120))):
            with self.assertRaises(module.Refused):
                module.shared_scopes('kb', value)

    def test_changed_shared_scopes_are_reported_and_unspecified_ones_kept(self):
        recorded = {'accounts': ['aimee'], 'shared_scopes': ['global', 'project:apollo'],
                    'embedding_provider_identity_digest': 'sha256:old', 'node': 'n'}
        kept = module.desired_policy('kb', recorded, None, None, None)
        self.assertEqual(module.policy_changes(recorded, kept), [])
        wider = module.desired_policy('kb', recorded, None, None, 'project:apollo,project:zephyr')
        self.assertEqual(wider['shared_scopes'], ['global', 'project:apollo', 'project:zephyr'])
        self.assertEqual(module.policy_changes(recorded, wider), ['shared scopes'])
        # inputs written before shared scopes were recorded count as serving none
        legacy = {'accounts': ['aimee'], 'embedding_provider_identity_digest': None, 'node': 'n'}
        self.assertEqual(module.policy_changes(legacy, module.desired_policy('kb', legacy, None, None, None)), [])

    def test_supplied_identity_must_be_a_health_document(self):
        self.assertIsNone(module.supplied_identity(b''))
        self.assertIsNone(module.supplied_identity(b' \n'))
        self.assertEqual(module.supplied_identity(json.dumps(HEALTH).encode()), HEALTH)
        for raw in (b'not json', b'[]', b'{"status": "ok"}', b'{"embedding_identity": {}}'):
            with self.assertRaises(module.Refused):
                module.supplied_identity(raw)

    def test_incomplete_saved_inputs_are_not_silently_replaced(self):
        with tempfile.TemporaryDirectory() as work:
            inputs = Path(work) / 'inputs-server'
            self.assertIsNone(module.recorded_policy(inputs))
            inputs.mkdir()
            with self.assertRaises(module.Refused):
                module.recorded_policy(inputs)
            (inputs / 'OPERATOR_POLICY_SAFE.json').write_text(json.dumps(self.RECORDED))
            self.assertEqual(module.recorded_policy(inputs), dict(self.RECORDED, typed_facts=None))
            (inputs / 'TYPED_FACTS.json').write_text(json.dumps({'model': 'm', 'upstream': 'http://127.0.0.1:9/v1'}))
            self.assertEqual(module.recorded_policy(inputs)['typed_facts']['model'], 'm')

    def test_a_kb_with_a_synthesis_model_selects_typed_facts(self):
        env = {'SYNTHESIS_ENDPOINT': 'https://aimee-llm:8761/v1', 'SYNTHESIS_MODEL': 'gemma-4-E4B',
               'SYNTHESIS_CA_FILE': '/run/ca.pem', 'SYNTHESIS_CERT_FILE': '/run/c.pem',
               'SYNTHESIS_KEY_FILE': '/run/c.key'}
        log = module.log
        module.log = lambda message: None
        try:
            with patch.dict(os.environ, env, clear=False):
                self.assertIsNone(module.typed_facts('server'))
                self.assertEqual(module.typed_facts('kb'), {
                    'model': 'gemma-4-E4B', 'upstream': 'https://aimee-llm:8761/v1', 'ca_file': '/run/ca.pem',
                    'cert_file': '/run/c.pem', 'key_file': '/run/c.key'})
                with patch.dict(os.environ, {'AIMEE_MEMORY_TYPED_FACTS': 'off'}):
                    self.assertIsNone(module.typed_facts('kb'))
                with patch.dict(os.environ, {'SYNTHESIS_KEY_FILE': ''}):
                    self.assertIsNone(module.typed_facts('kb'))
                with patch.dict(os.environ, {'SYNTHESIS_ENDPOINT': 'http://models.example:8000'}):
                    self.assertIsNone(module.typed_facts('kb'))
                with patch.dict(os.environ, {'SYNTHESIS_ENDPOINT': 'http://127.0.0.1:8000'}):
                    self.assertEqual(module.typed_facts('kb')['upstream'], 'http://127.0.0.1:8000/v1')
                recorded = dict(self.RECORDED, typed_facts=None)
                desired = module.desired_policy('kb', recorded, None, None)
                self.assertEqual(module.policy_changes(recorded, desired), ['typed facts'])
        finally:
            module.log = log


class Placeholder(unittest.TestCase):
    def setUp(self):
        self.owned = module.owned
        module.owned = lambda path: None  # ownership needs root; the decisions do not

    def tearDown(self):
        module.owned = self.owned

    def test_off_creates_only_mount_points_and_they_can_be_replaced_later(self):
        with tempfile.TemporaryDirectory() as work:
            installed, prefix = Path(work) / 'private-memory/server', Path(work) / 'volume/server'
            module.placeholder(installed, prefix)
            for base in (installed, prefix):
                self.assertEqual(sorted(path.name for path in base.iterdir()), ['etc', 'run', 'state'])
            self.assertFalse((prefix / 'bin/launch-resource').exists())  # nothing the entrypoint would activate
            module.remove_placeholder(installed)
            self.assertFalse(installed.exists())
            module.remove_placeholder(installed)  # nothing there is fine

    def test_directories_holding_files_are_never_removed(self):
        with tempfile.TemporaryDirectory() as work:
            installed = Path(work) / 'private-memory/server'
            module.placeholder(installed, Path(work) / 'volume/server')
            (installed / 'state/records').write_text('memory')
            with self.assertRaises(module.Refused):
                module.remove_placeholder(installed)
            self.assertEqual((installed / 'state/records').read_text(), 'memory')

    def test_off_leaves_an_existing_installation_untouched(self):
        with tempfile.TemporaryDirectory() as work:
            installed = Path(work) / 'private-memory/server'
            installed.mkdir(parents=True)
            (installed / 'installation.json').write_text('{}')
            module.placeholder(installed, Path(work) / 'volume/server')
            self.assertEqual([path.name for path in installed.iterdir()], ['installation.json'])
            self.assertFalse((Path(work) / 'volume').exists())


class Staleness(unittest.TestCase):
    def test_installation_prepared_from_other_executables_is_stale(self):
        with tempfile.TemporaryDirectory() as work:
            bundle, prefix, public = Path(work) / 'bundle', Path(work) / 'prefix', Path(work) / 'public-exe'
            (bundle / 'bin').mkdir(parents=True)
            prefix.mkdir()
            public.write_bytes(b'public')
            files = {}
            for relative, name in module.BUNDLE_COPIES.items():
                (bundle / 'bin' / name).write_bytes(name.encode())
                files[relative] = hashlib.sha256(name.encode()).hexdigest()
            manifest = {'files': files, 'container_public_executables': {
                str(public): {'sha256': hashlib.sha256(b'public').hexdigest(), 'prepared_copy': 'x'}}}
            (prefix / 'overlay-manifest.json').write_text(json.dumps(manifest))
            reasons = module.stale_reasons(prefix, 'server', bundle)
            # Only the resource binary, which this fixture cannot place at its image path, may differ.
            self.assertEqual([reason for reason in reasons if 'aimee-server' not in reason], [])
            (bundle / 'bin/aims-db-module').write_bytes(b'newer release')
            public.write_bytes(b'newer public')
            reasons = module.stale_reasons(prefix, 'server', bundle)
            self.assertIn('memory executable aims-db-module differs', reasons)
            self.assertIn(f'{public} differs', reasons)


class RunningApplication(unittest.TestCase):
    def test_only_a_listening_bus_socket_counts_as_running(self):
        with tempfile.TemporaryDirectory() as work:
            home = Path(work)
            self.assertFalse(module.application_running('server', home))
            path = home / 'server-module-bus.sock'
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            listener.bind(str(path))
            self.assertFalse(module.application_running('server', home))  # bound, nobody listening
            listener.listen(1)
            self.assertTrue(module.application_running('server', home))
            listener.close()
            self.assertFalse(module.application_running('server', home))  # stale socket file


if __name__ == '__main__':
    unittest.main()


class RunningApplication(unittest.TestCase):
    """A repeated `up` against a running application never fails for a policy-only change."""

    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        root = Path(self.work.name)
        self.saved = {name: getattr(module, name) for name in (
            'ROOT', 'HOME', 'BUNDLE', 'owned', 'recorded_policy', 'stale_reasons', 'application_running',
            'install_home', 'prepare', 'served_accounts')}
        module.ROOT, module.HOME, module.BUNDLE = root / 'volume', root / 'home', root / 'bundle'
        for path in (module.ROOT / 'server', module.HOME / 'private-memory/server', module.BUNDLE / 'provision'):
            path.mkdir(parents=True)
        (module.BUNDLE / 'provision/prepare.py').write_text('')
        (module.HOME / 'private-memory/server/installation.json').write_text('{}')
        (module.ROOT / 'server/overlay-manifest.json').write_text('{}')
        module.owned = lambda path: None
        import shutil, types
        self.saved_shutil = module.shutil
        module.shutil = types.SimpleNamespace(**{name: getattr(shutil, name) for name in dir(shutil)
                                                 if not name.startswith('__')})
        module.shutil.chown = lambda *args, **kwargs: None  # ownership needs the runtime user
        module.recorded_policy = lambda inputs: {'accounts': ['aimee'], 'embedding_provider_identity_digest': None,
                                                 'node': 'n', 'shared_scopes': []}
        module.served_accounts = lambda role, value, home: 'aimee,first-boot-login'
        module.install_home = lambda prefix: None
        module.application_running = lambda role, *args: True
        self.prepared = []
        module.prepare = lambda *args: self.prepared.append(args)

    def tearDown(self):
        for name, value in self.saved.items():
            setattr(module, name, value)
        module.shutil = self.saved_shutil
        self.work.cleanup()

    def test_a_new_login_waits_for_the_next_start(self):
        module.stale_reasons = lambda prefix, role: []
        self.assertEqual(module.provision('server'), 'deferred')
        self.assertEqual(self.prepared, [])

    def test_a_stale_installation_still_needs_a_stop(self):
        module.stale_reasons = lambda prefix, role: ['the image changed']
        with self.assertRaises(module.Refused):
            module.provision('server')
        self.assertEqual(self.prepared, [])
