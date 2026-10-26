"""Exercise the real process/stdin/stdout boundary without a model or server."""
import asyncio
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

from model_client import ThinClient


class ThinClientTests(unittest.IsolatedAsyncioTestCase):
    def client(self, directory, source):
        script = Path(directory) / "aimee"
        script.write_text("#!" + sys.executable + "\n" + source)
        script.chmod(0o700)
        return ThinClient(SimpleNamespace(aimee_cli=str(script), aimee_profile="bot",
                                          aimee_socket=Path("/run/aimee/http.sock")))

    async def test_canonical_roundtrip_uses_stdin_and_normal_socket(self):
        with tempfile.TemporaryDirectory() as directory:
            client = self.client(directory, '''import json,os,sys
assert sys.argv[1:] == ['--json','--profile=bot','model','generate']
assert os.environ['AIMEE_API_ENDPOINT'] == 'unix:/run/aimee/http.sock'
value=json.load(sys.stdin)
assert value['agent']=='registered-bot'
print(json.dumps({'ir':value['ir']}))
''')
            request = {"messages": [{"role": "user", "blocks": [{"type": "text", "text": "雪 {hi}"}]}]}
            self.assertEqual(await client.generate(request, "registered-bot"), request)

    async def test_failures_do_not_expose_child_diagnostics(self):
        for source in ("print('secret');raise SystemExit(1)", "print('not json')",
                       "print('{\"status\":\"error\",\"message\":\"secret\"}')",
                       "print('x'*(8*1024*1024+1))"):
            with self.subTest(source=source), tempfile.TemporaryDirectory() as directory:
                client = self.client(directory, source)
                with self.assertRaises(RuntimeError) as error:
                    await client.generate({}, "bot")
                self.assertNotIn("secret", str(error.exception))

    async def test_timeout_and_cancellation_reap_child(self):
        with tempfile.TemporaryDirectory() as directory:
            pidfile = Path(directory) / "pid"
            client = self.client(directory, "import os,time\nfrom pathlib import Path\n"
                                 + f"Path({str(pidfile)!r}).write_text(str(os.getpid()))\n"
                                 + "time.sleep(30)\n")
            client.timeout = .1
            with self.assertRaises(TimeoutError):
                await client.generate({}, "bot")
            pid = int(pidfile.read_text())
            import os
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)
            client.timeout = 30
            task = asyncio.create_task(client.generate({}, "bot"))
            await asyncio.sleep(.1)
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
            pid = int(pidfile.read_text())
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)

    async def test_request_limit_before_launch(self):
        client = ThinClient(SimpleNamespace(aimee_cli="/missing", aimee_profile=None, aimee_socket=None))
        with self.assertRaises(ValueError):
            await client.generate({"text": "x" * (1024 * 1024)}, "bot")
