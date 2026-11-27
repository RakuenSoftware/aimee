"""The thin client's code-index upload against an isolated fake server.

Pins three behaviours a live Server showed were missing:
  * a failed batch stops the upload (it used to push every remaining batch into
    a scan that was already doomed);
  * a scan another client is running is reported and left alone (it used to be
    superseded, so concurrent sessions cancelled each other until none sealed);
  * discovery in a repository that was never indexed starts the first upload as
    one detached background job and answers at once (it used to block the
    session for the whole upload, many minutes for a large repository).
"""
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest


BINARY = Path(os.environ.get("AIMEE_TEST_PROXY_BINARY",
                             Path(__file__).resolve().parents[2] / "aimee")).resolve()


class FakeServer(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, payload, status=200):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def do_GET(self):
        self.do_POST()

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        request = json.loads(body) if body else {}
        server = self.server
        if self.path == "/v1/cli/manifest":
            ops = ("index.list", "index.find")
            return self.reply({"manifest_version": 1, "routes": [
                {"op": op, "verb": "POST", "path": "/v1/" + op.replace(".", "/")} for op in ops]})
        if self.path == "/v1/index/list":
            return self.reply({"projects": []})
        if self.path == "/v1/index/ingest":
            with server.lock:
                server.phases.append(request.get("phase"))
                run = f"oprun_{len(server.phases)}_1"
                server.results[run] = server.answer(request)
            if request.get("phase") == "begin":
                server.began.set()
                server.hold.wait(20)
            return self.reply({"id": run, "status": "queued"})
        if self.path.startswith("/v1/runs/"):
            run = self.path.rsplit("/", 1)[-1]
            return self.reply({"id": run, "status": "completed", "result": server.results[run]})
        return self.reply({"error": "not found"}, 404)


def ok(_request):
    return {"status": "ok", "projects": 1, "files": 1}


class ThinClientIndexScan(unittest.TestCase):
    def setUp(self):
        # The client refuses hidden roots (any path component starting with "."),
        # so the fixture must not live under one.
        base = os.environ.get("AIMEE_TEST_SCAN_DIR")
        self.temp = tempfile.TemporaryDirectory(prefix="aimee-index-scan-", dir=base)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "fixture"
        self.repo.mkdir()
        subprocess.run(["git", "init", "-q", str(self.repo)], check=True)
        subprocess.run(["git", "-C", str(self.repo), "remote", "add", "origin",
                        "https://github.com/example/fixture.git"], check=True)
        # Several byte-bounded batches: each file is ~200 KB, a batch ~600 KB.
        for i in range(12):
            (self.repo / f"part{i}.c").write_text(f"int part{i}(void) {{ return {i}; }}\n" + "/* pad */\n" * 20000)
        self.home = self.root / "home"
        self.home.mkdir()
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeServer)
        self.server.lock = threading.Lock()
        self.server.phases = []
        self.server.results = {}
        self.server.answer = ok
        self.server.began = threading.Event()
        self.server.hold = threading.Event()
        self.server.hold.set()
        thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(self.stop_server, thread)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith(("AIMEE_", "ANTHROPIC_", "OPENAI_"))}
        self.env.update(HOME=str(self.home), AIMEE_HOME=str(self.home / "aimee"),
                        AIMEE_NO_CLIENT_INTEGRATIONS="1", AIMEE_SERVER_TOKEN="remote-test-secret",
                        AIMEE_SERVER_URL=f"http://127.0.0.1:{self.server.server_port}")

    def stop_server(self, thread):
        self.server.hold.set()
        self.server.shutdown()
        thread.join(5)
        self.server.server_close()

    def run_client(self, *args, timeout=60):
        return subprocess.run([str(BINARY), *args], cwd=self.repo, env=self.env, capture_output=True,
                              text=True, timeout=timeout)

    def test_failed_batch_stops_the_upload(self):
        staged = []

        def answer(request):
            if request.get("phase") == "stage":
                staged.append(len(request.get("files", [])))
                if len(staged) == 2:
                    return {"status": "error", "message": "store refused the batch"}
            return ok(request)

        self.server.answer = answer
        done = self.run_client("index", "scan", str(self.repo))
        self.assertNotEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertGreater(sum(1 for _ in self.repo.glob("part*.c")) * 200_000 // 600_000, 2,
                           "fixture must need more than two batches")
        self.assertEqual(len(staged), 2, f"kept uploading after the failed batch: {self.server.phases}")
        self.assertEqual(self.server.phases[-1], "abort")
        self.assertNotIn("seal", self.server.phases)

    def test_scan_in_progress_elsewhere_is_left_alone(self):
        def answer(request):
            if request.get("phase") == "begin":
                return {"status": "error", "reason": "busy",
                        "message": "code index scan refused (HTTP 409): another client is indexing this project"}
            return ok(request)

        self.server.answer = answer
        done = self.run_client("index", "scan", str(self.repo))
        self.assertNotEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("already being indexed by another session", done.stderr)
        self.assertEqual(self.server.phases, ["begin"], "uploaded into a scan another client holds")

    def test_first_discovery_indexes_in_the_background_once(self):
        self.server.hold.clear()  # keep the background job's begin in flight
        started = time.monotonic()
        first = self.run_client("index", "find", "part1", timeout=30)
        elapsed = time.monotonic() - started
        self.assertNotEqual(first.returncode, 0)
        self.assertIn("indexing it now in the background", first.stderr)
        self.assertLess(elapsed, 15, "discovery waited for the upload")
        self.assertTrue(self.server.began.wait(15), "the background job never started the scan")
        logs = list((self.home / ".cache/aimee").glob("index-fixture-*.log"))
        self.assertEqual(len(logs), 1, logs)

        # A second session while the job runs: told about it, no second job.
        second = self.run_client("index", "find", "part2", timeout=30)
        self.assertIn("being indexed for the first time in the background", second.stderr)
        self.server.hold.set()
        deadline = time.monotonic() + 60
        while "seal" not in self.server.phases and time.monotonic() < deadline:
            time.sleep(0.2)
        self.assertIn("seal", self.server.phases, "the background job did not finish the scan")
        self.assertEqual(self.server.phases.count("begin"), 1, self.server.phases)


if __name__ == "__main__":
    unittest.main()
