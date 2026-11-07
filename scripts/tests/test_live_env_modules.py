"""Authentication rigs arm their actual generic dependencies without a memory owner."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HELPER = Path(__file__).resolve().parents[1] / "lib" / "aimee-live-env.sh"


class LiveEnvironmentModulesTests(unittest.TestCase):
    def check_modules(self, placement, expected):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "armed"
            binaries = root / "src/build/obj"
            binaries.mkdir(parents=True)
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
            home = root / "home"
            home.mkdir(mode=0o700)
            script = '''set -eu
. "$AIMEE_LIVE_HELPER"
live_env_prepare_modules() { :; }
live_env_stop_module() { :; }
live_env_stop_kb_modules() { :; }
live_env_write_config() { :; }
live_env_arm_module() { printf '%s\\n' "$1" >> "$ARM_LOG"; }
'''
            script += "live_env_start_kb_modules\n" if placement == "kb" else "live_env_start_module\n"
            environment = {**os.environ, "AIMEE_LIVE_HELPER": str(HELPER), "ARM_LOG": str(output),
                           "AIMEE_HOME": str(home), "LIVE_KB_HOME": str(home),
                           "LIVE_KB_STORE_URL": "postgres://fixture", "LIVE_KB_MIGRATION_URL": "postgres://fixture",
                           "LIVE_NAME": "test"}
            subprocess.run(["bash", "-c", script], cwd=root, env=environment, check=True,
                           capture_output=True, text=True)
            self.assertEqual([Path(line).name for line in output.read_text().splitlines()], expected)
            self.assertFalse(list((home / "modules.d" / placement).glob("*memory*")))

    def test_kb_config_and_postgres_are_armed_without_memory(self):
        self.check_modules("kb", ["aimee-module-config", "aimee-module-postgres"])

    def test_server_config_and_store_are_armed_without_memory(self):
        self.check_modules("server", ["aimee-module-postgres", "aimee-module-aimee", "aimee-module-config"])


if __name__ == "__main__":
    unittest.main()
