"""A stuck disposable owner must fail the harness instead of hanging CI."""
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == 'posix', 'requires POSIX child signals')
class IntegrationShutdownTest(unittest.TestCase):
    def run_owner(self, ignores_stop, original_exit=0):
        source = (ROOT / 'src/tests/test_integration.sh').read_text()
        start = source.index('stop_memory_module() {')
        stop = source.index('\n}\n', start) + 3
        cleanup_start = source.index('cleanup() {')
        cleanup_stop = source.index('\n}\n', cleanup_start) + 3
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            worker = root / 'worker.py'
            ready = root / 'ready'
            worker.write_text('''import pathlib,signal,sys,time
if sys.argv[2] == 'ignore':
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    signal.signal(signal.SIGQUIT, signal.SIG_IGN)
print('fixture owner ready', flush=True)
pathlib.Path(sys.argv[1]).touch()
time.sleep(120)
''')
            quote = shlex.quote
            script = source[start:stop] + '\n' + source[cleanup_start:cleanup_stop] + '\n' + f'''
stop_workflow_module() {{ :; }}
stop_db1_module() {{ :; }}
stop_pg_module() {{ :; }}
stop_config_module() {{ :; }}
stop_providers_module() {{ :; }}
stop_delegates_module() {{ :; }}
stop_policy_module() {{ :; }}
stop_tools_module() {{ :; }}
stop_git_module() {{ :; }}
stop_learning_module() {{ :; }}
FAIL=0
REACHED_SUMMARY=1
SERVER_PID=""
HOME={quote(str(root))}
trap cleanup EXIT
{quote(sys.executable)} {quote(str(worker))} {quote(str(ready))} {'ignore' if ignores_stop else 'normal'} > "$HOME/aimee-memory.log" 2>&1 &
MEMORY_MODULE_PID=$!
owned_pid=$MEMORY_MODULE_PID
for tick in $(seq 1 200); do
    [ -f {quote(str(ready))} ] && break
    sleep .01
done
[ -f {quote(str(ready))} ] || exit 21
stop_memory_module
[ -z "$MEMORY_MODULE_PID" ] || exit 22
if kill -0 "$owned_pid" 2>/dev/null; then exit 23; fi
printf 'shutdown failures=%s\\n' "$FAIL"
exit {original_exit}
'''
            process = subprocess.Popen(['bash', '-c', script], stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True,
                                       start_new_session=True)
            started = time.monotonic()
            try:
                output, _ = process.communicate(timeout=9)
            finally:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
            self.assertLess(time.monotonic() - started, 8)
            self.assertEqual(process.returncode, 1 if ignores_stop else original_exit, output)
            return output

    def test_graceful_owner_reaps_without_failure(self):
        output = self.run_owner(False)
        self.assertIn('shutdown failures=0', output)
        self.assertNotIn('FAIL:', output)

    def test_uncooperative_owner_is_killed_and_records_failure(self):
        output = self.run_owner(True)
        self.assertIn('shutdown failures=1', output)
        self.assertIn('FAIL: memory owner did not stop within five seconds', output)
        self.assertIn('fixture owner ready', output)

    def test_cleanup_preserves_an_existing_failure(self):
        self.assertIn('shutdown failures=0', self.run_owner(False, original_exit=7))


if __name__ == '__main__':
    unittest.main()
