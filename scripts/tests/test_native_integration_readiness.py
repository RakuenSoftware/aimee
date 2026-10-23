"""Execute the real harness startup function at its dependency races."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class NativeIntegrationReadiness(unittest.TestCase):
    def test_native_catalog_observations_keep_full_declarations_and_scope(self):
        import importlib.util
        import json
        spec = importlib.util.spec_from_file_location("native_live_catalog", ROOT / "src/tests/test_native_ir_live.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as directory:
            observations = module.NativeCatalogObservations(directory)
            tools = [{"type":"function", "name":"read_file", "description":"complete declaration",
                      "parameters":{"type":"object", "properties":{"path":{"type":"string"}}, "required":["path"]}}]
            first = observations.observe("claude", "anthropic", "primary", tools, {"messages":[{"role":"user"}]})
            self.assertEqual(first["tools"], tools)
            self.assertEqual(observations.observe("claude", "anthropic", "primary", tools, {"messages":[{"role":"user"}]})["sha256"], first["sha256"])
            for changed in ([], tools + tools, [{**tools[0], "parameters":{"type":"object"}}]):
                with self.subTest(changed=changed), self.assertRaises(AssertionError):
                    observations.observe("claude", "anthropic", "primary", changed, {"messages":[{"role":"user"}]})
                saved_failure = json.loads((Path(directory) / ("catalog-%04d.json" % len(observations.records))).read_text())
                self.assertEqual(saved_failure["tools"], changed)
            ordered = tools + [{**tools[0], "name":"write_file"}]
            observations.observe("codex", "chatgpt", "primary", ordered, {"input":[{"role":"user"}]})
            with self.assertRaises(AssertionError):
                observations.observe("codex", "chatgpt", "primary", list(reversed(ordered)), {"input":[{"role":"user"}]})
            observations.observe("http-child", "openai", "diagnose", [], {"messages":[]})
            saved = json.loads((Path(directory) / "catalog-0001.json").read_text())
            self.assertEqual(saved["names"], [{"namespace":None,"name":"read_file"}])
            self.assertEqual(saved["tools"], tools)
            self.assertNotIn("headers", saved)

    def test_observed_wire_history_controls_exact_opening_policy_comparison(self):
        import importlib.util
        import json
        spec = importlib.util.spec_from_file_location("native_live_phases", ROOT / "src/tests/test_native_ir_live.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        opening = {"input":[{"type":"message", "role":"user", "content":[]}]}
        established = {"input":[*opening["input"], {"type":"function_call", "name":"read_file", "call_id":"call-1", "arguments":"{}"},
                                 {"type":"function_call_output", "call_id":"call-1", "output":"ok"}]}
        read = {"type":"function", "name":"read_file", "parameters":{"type":"object"}}
        bash = {"type":"function", "name":"bash", "parameters":{"type":"object"}}
        with tempfile.TemporaryDirectory() as directory:
            observer = module.NativeCatalogObservations(directory)
            observer.observe("codex", "chatgpt", "primary", [read], opening)
            observer.observe("codex", "chatgpt", "primary", [bash, read], established)
            observer.observe("codex", "chatgpt", "primary", [bash, read], established) # auth/resume is stable
            with self.assertRaisesRegex(AssertionError, "within policy phase"):
                observer.observe("codex", "chatgpt", "primary", [read], established)
            with self.assertRaisesRegex(AssertionError, "within policy phase"):
                observer.observe("codex", "chatgpt", "primary", [read, bash], established)
            with self.assertRaisesRegex(AssertionError, "within policy phase"):
                observer.observe("codex", "chatgpt", "primary", [{**bash, "parameters":{"type":"object", "properties":{"extra":{}}}}, read], established)
            records = observer.records
            self.assertEqual(records[0]["history_phase"], "opening")
            self.assertEqual(records[1]["history_phase"], "established")
            self.assertEqual(records[1]["history_shape"]["items"][1]["type"], "function_call")
            other = module.NativeCatalogObservations(directory)
            other.observe("codex", "chatgpt", "primary", [read], opening)
            with self.assertRaisesRegex(AssertionError, "beyond opening"):
                other.observe("codex", "chatgpt", "primary", [bash, {**read,"description":"drift"}], established)
            with self.assertRaises(AssertionError):
                module.NativeCatalogObservations().observe("codex", "chatgpt", "primary", [bash,read], opening)
        phase = module.NativeCatalogObservations.history_phase
        self.assertEqual(phase("chatgpt", {"input":[{"type":"function_call_output", "output":"assistant"}]})[0], "opening")
        self.assertEqual(phase("chatgpt", {"input":[{"type":"reasoning", "summary":[]}]})[0], "established")
        for wire in ("anthropic", "openai"):
            self.assertEqual(phase(wire, {"messages":[{"role":"user","content":"assistant"}]})[0], "opening")
            self.assertEqual(phase(wire, {"messages":[{"role":"assistant","content":[]}]})[0], "established")
        mcp = {"type":"namespace", "name":"mcp__aimee", "tools":[bash,read]}
        native = {"type":"namespace", "name":"functions", "tools":[bash,read]}
        self.assertEqual(module.NativeCatalogObservations.opening_tools([mcp,native]), [mcp,{**native,"tools":[read]}])

    def mode_prefix(self, **overrides):
        source = (ROOT / "src/tests/test_integration.sh").read_text()
        prefix = source[:source.index("# Resolve paths relative to repo root")]
        env = {"PATH": "/usr/bin:/bin", **overrides}
        return subprocess.run(["bash", "-c", prefix + "\necho MODE_PREFIX_COMPLETE\n"],
                              env=env, capture_output=True, text=True, timeout=3)

    def test_default_mode_does_not_select_a_focused_slice(self):
        result = self.mode_prefix()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "MODE_PREFIX_COMPLETE\n")
        source = (ROOT / "src/tests/test_integration.sh").read_text()
        # Existing default general branches still run, and the original eight
        # profile condition remains independent of the general-only branches.
        self.assertIn('if [ "$NATIVE_RECOVERY_ONLY" = 0 ] && [ "$NATIVE_MATRIX_ONLY" = 0 ]; then', source)
        self.assertIn('if [ "$NATIVE_RECOVERY_ONLY" = 0 ] && [ -n "${AIMEE_TEST_NATIVE_CODEX:-}" ]', source)

    def test_matrix_mode_refuses_missing_runtime_or_sdk_before_setup(self):
        for overrides in ({"AIMEE_TEST_NATIVE_MATRIX_ONLY": "invalid"},
                          {"AIMEE_TEST_NATIVE_MATRIX_ONLY": "1"},
                          {"AIMEE_TEST_NATIVE_MATRIX_ONLY": "1", "AIMEE_TEST_NATIVE_RECOVERY_ONLY": "1"}):
            with self.subTest(overrides=overrides):
                result = self.mode_prefix(**overrides)
                self.assertEqual(result.returncode, 2)
                self.assertNotIn("MODE_PREFIX_COMPLETE", result.stdout)

    def test_matrix_mode_labels_coverage_without_claiming_general_integration(self):
        result = self.mode_prefix(AIMEE_TEST_NATIVE_MATRIX_ONLY="1",
                                 AIMEE_TEST_NATIVE_CODEX="/usr/bin/true",
                                 AIMEE_TEST_NATIVE_CLAUDE="/usr/bin/true",
                                 AIMEE_STORE_URL="fixture-runtime", AIMEE_STORE_MIGRATION_URL="fixture-migrator")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("all eight profiles, refusal and source fences", result.stdout)
        self.assertIn("general integration and Docker grading are not run", result.stdout)
        self.assertTrue(result.stdout.endswith("MODE_PREFIX_COMPLETE\n"))

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
