"""Execute the real harness startup function at its dependency races."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class NativeIntegrationReadiness(unittest.TestCase):
    def run_startup(self, memory_enabled):
        source = (ROOT / "src/tests/test_integration.sh").read_text()
        start = source.index("start_server() {\n")
        function = source[start:source.index("\n}\n", start) + 3]
        contract = (ROOT / "src/headers/config_accessor_keys.def").read_text()
        accessor = re.search(r'CONFIG_NUMBER\(server_api_http_port,\s*int,\s*"([^"]+)"', contract)
        self.assertIsNotNone(accessor, "registered configuration accessor missing")
        lookup_key = accessor.group(1)
        if memory_enabled:
            probe = re.search(r'\{"method":"config.get","key":"([^"]+)"\}', function)
            self.assertIsNotNone(probe, "actual configuration serving probe missing")
            self.assertEqual(probe.group(1), lookup_key,
                             "readiness must use the actual snapshot accessor key, not a setter path")
        owners = ["config", "providers", "delegates", "policy", "tools", "git",
                  "learning", "economizer", "routing"]
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            daemon = home / "owned-test-process"
            daemon.write_text("#!/bin/sh\nexec sleep 10\n")
            daemon.chmod(0o700)
            definitions = "\n".join(f"start_{owner}_module() {{ echo {owner}; }}" for owner in owners)
            harness = r'''
set -e
MODULE_BUS_SOCK="$HOME/module.sock"
HTTP_SOCK="$HOME/http.sock"
BUS_EDGE=pending
# Publish both actual socket entries just after the first bus check observed
# absence. Neither a fork nor socket publication establishes config SERVING.
function [ {
    if [[ "$1" == -S && "$2" == "$MODULE_BUS_SOCK" && "$BUS_EDGE" == pending ]]; then
        BUS_EDGE=published
        python3 - "$MODULE_BUS_SOCK" "$HTTP_SOCK" <<'PY'
import socket,sys
for path in sys.argv[1:]:
    sock=socket.socket(socket.AF_UNIX)
    sock.bind(path)
    sock.close()
PY
        return 1
    fi
    builtin [ "$@"
}
http_rpc() {
    [[ "$AIMEE_TEST_HTTP_TIMEOUT" -gt 0 && "$AIMEE_TEST_HTTP_TIMEOUT" -le 10 ]]
    [[ "$1" == "{\"method\":\"config.get\",\"key\":\"$CONFIG_EXPECTED_KEY\"}" ]] || return 1
    if [[ ! -e "$HOME/config-queried" ]]; then
        touch "$HOME/config-queried"
        echo '{"status":"error","kind":"capability_absent"}'
    else
        touch "$HOME/config-serving"
        printf '{"status":"ok","key":"%s","value":0,"secret":false}\n' "$CONFIG_EXPECTED_KEY"
    fi
}
start_memory_module() {
    # The first config response was a refusal. A fork before the later actual
    # serving acknowledgment reproduces memory's startup exit deterministically.
    [[ -f "$HOME/config-serving" && -f "$HOME/store-serving" ]] || { echo premature-memory-launch >&2; exit 9; }
    echo memory
}
start_db1_module() { touch "$HOME/store-serving"; echo db1; }
trap 'if [[ -n "$SERVER_PID" ]]; then kill "$SERVER_PID" 2>/dev/null || true; wait "$SERVER_PID" 2>/dev/null || true; fi' EXIT
'''
            return subprocess.run(["bash", "-c", harness + "\n" + definitions + "\n" + function + "\nstart_server\n"],
                                  env={**os.environ, "HOME": str(home), "AIMEE_SERVER": str(daemon),
                                       "MEMORY_MODULE_ENABLED": str(memory_enabled), "CONFIG_EXPECTED_KEY": lookup_key},
                                  capture_output=True, text=True, timeout=5), owners

    def test_http_socket_cannot_skip_module_launch_between_readiness_checks(self):
        result, owners = self.run_startup(memory_enabled=0)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), owners + ["db1"])

    def test_memory_launch_requires_actual_success_after_configuration_refusal(self):
        result, owners = self.run_startup(memory_enabled=1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), owners + ["db1", "memory"],
                         "memory launched before successful config serving acknowledgment")


if __name__ == "__main__":
    unittest.main()
