"""Authentication rigs keep generic defaults or explicitly select the frozen reference."""
import os
import hashlib
import json
import shutil
import re
from pathlib import Path
import subprocess
import tempfile
import unittest

HELPER = Path(__file__).resolve().parents[1] / "lib" / "aimee-live-env.sh"


class LiveEnvironmentModulesTests(unittest.TestCase):
    def check_modules(self, placement, expected, reference=False, invalid=False, invalid_grant=False):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "armed"
            binaries = root / "src/build/obj"
            binaries.mkdir(parents=True)
            if reference:
                (root / 'scripts').mkdir()
                shutil.copyfile(HELPER.parents[1] / 'build_reference_memory_fixture.py',
                                root / 'scripts/build_reference_memory_fixture.py')
                manifest = HELPER.parents[2] / 'tests/fixtures/legacy-memory-owner.json'
                (root / 'tests/fixtures').mkdir(parents=True)
                shutil.copyfile(manifest, root / 'tests/fixtures/legacy-memory-owner.json')
                owner = root / 'reference/aimee-module-memory'
                owner.parent.mkdir()
                owner.write_bytes(b'unit-selection-artifact-not-functional-owner')
                owner.chmod(0o700)
                receipt = json.loads(manifest.read_text())
                receipt['binary_sha256'] = hashlib.sha256(owner.read_bytes()).hexdigest()
                if invalid:
                    receipt['source_commit'] = '0' * 40
                owner.with_name(owner.name + '.json').write_text(json.dumps(receipt))
            for name in ("aimee-module", "aimee-module-aimee", "aimee-module-postgres", "aimee-module-config"):
                binary = binaries / name
                binary.write_text("#!/bin/sh\nexit 1\n")
                binary.chmod(0o700)
            for role, names in (("kb", ("config", "postgres")),
                                ("server", ("aimee", "postgres", "aimee-postgres", "config"))):
                grants = binaries / "module-bundle/grants" / role
                grants.mkdir(parents=True)
                for name in names:
                    (grants / (name + ".grant")).write_text("version=1\nexecutable=/fixture/" + name + "\n")
                for name in ('memory', 'memory-postgres', 'memory-egress'):
                    (grants / (name + '.grant')).write_text(
                        'version=1\nprincipal_ref=7\nuid=self\n'
                        'executable=/fixture/aimee-module-memory\nrequest=11266\nserve=5889\n')
                if invalid_grant:
                    with (grants / 'memory-postgres.grant').open('a') as stream:
                        stream.write('executable=/fixture/other\n')
            home = root / "home"
            home.mkdir(mode=0o700)
            script = '''set -eu
. "$AIMEE_LIVE_HELPER"
live_env_prepare_modules() { :; }
live_env_stop_module() { :; }
live_env_stop_kb_modules() { :; }
live_env_write_config() { :; }
live_env_arm_module() { printf '%s\\n' "$1" >> "$ARM_LOG"; printf '%s\\n' "$@" >> "$ARM_ARGS"; }
'''
            script += "live_env_start_kb_modules\n" if placement == "kb" else "live_env_start_module\n"
            environment = {**os.environ, "AIMEE_LIVE_HELPER": str(HELPER), "ARM_LOG": str(output),
                           "AIMEE_HOME": str(home), "LIVE_KB_HOME": str(home), "ARM_ARGS": str(root / "arm-args"),
                           "LIVE_KB_STORE_URL": "postgres://fixture", "LIVE_KB_MIGRATION_URL": "postgres://fixture",
                           "LIVE_NAME": "test"}
            environment.pop('AIMEE_LIVE_REFERENCE_MEMORY_OWNER', None)
            if reference:
                environment['AIMEE_LIVE_REFERENCE_MEMORY_OWNER'] = str(owner)
            result = subprocess.run(["bash", "-c", script], cwd=root, env=environment,
                                    capture_output=True, text=True)
            if invalid or invalid_grant:
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(output.exists(), 'bad provenance must refuse before arming modules')
                return
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual([Path(line).name for line in output.read_text().splitlines()], expected)
            if reference:
                for name in ('memory', 'memory-postgres', 'memory-egress'):
                    source = binaries / 'module-bundle/grants' / placement / (name + '.grant')
                    selected = home / 'modules.d' / placement / (name + '.grant')
                    self.assertEqual(selected.read_text(), source.read_text().replace(
                        'executable=/fixture/aimee-module-memory', 'executable=' + str(owner)))
                self.assertEqual(output.read_text().splitlines()[-1], str(owner))
                arguments = (root / "arm-args").read_text()
                self.assertEqual(arguments.count("AIMEE_MODULE_PLACEMENT="), 1)
                self.assertIn("AIMEE_MODULE_PLACEMENT=" + placement, arguments)
                self.assertIn("AIMEE_MODULE_POLICY_DIR=" + str(home / "modules.d" / placement), arguments)
            else:
                self.assertFalse(list((home / "modules.d" / placement).glob("*memory*")))

    def test_kb_config_and_postgres_are_armed_without_memory(self):
        self.check_modules("kb", ["aimee-module-config", "aimee-module-postgres"])

    def test_server_config_and_store_are_armed_without_memory(self):
        self.check_modules("server", ["aimee-module-postgres", "aimee-module-aimee", "aimee-module-config"])

    def test_explicit_reference_is_armed_with_exact_kb_grants(self):
        self.check_modules('kb', ['aimee-module-config', 'aimee-module-postgres',
                                 'aimee-module-memory'], reference=True)

    def test_explicit_reference_is_armed_with_exact_server_grants(self):
        self.check_modules('server', ['aimee-module-postgres', 'aimee-module-aimee',
                                     'aimee-module-config', 'aimee-module-memory'], reference=True)

    def test_bad_reference_provenance_never_arms_a_module(self):
        self.check_modules('server', [], reference=True, invalid=True)

    def test_ambiguous_reference_grant_never_arms_a_module(self):
        self.check_modules('server', [], reference=True, invalid_grant=True)

    def test_ci_selects_reference_only_for_memory_action_rig(self):
        workflow = (HELPER.parents[2] / '.github/workflows/ci.yml').read_text()
        job = re.search(r'^  live-auth-rigs:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)',
                        workflow, re.MULTILINE | re.DOTALL).group(1)
        tier = re.search(r'      - name: The tier gate, end to end\n(.*?)(?=\n      - name:|\Z)',
                         job, re.DOTALL).group(1)
        self.assertEqual(job.count('AIMEE_LIVE_REFERENCE_MEMORY_OWNER:'), 1)
        self.assertIn('AIMEE_LIVE_REFERENCE_MEMORY_OWNER:', tier)
        self.assertIn('build_reference_memory_fixture.py --output', job)


if __name__ == "__main__":
    unittest.main()
