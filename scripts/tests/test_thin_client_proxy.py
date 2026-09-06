"""Exercise the shipping thin client against an isolated HTTP/mTLS peer."""
import concurrent.futures
import gzip
import http.client
import http.server
import json
import os
from pathlib import Path
import queue
import re
import select
import shutil
import socket
import shlex
import ssl
import subprocess
import tempfile
import threading
import unittest


BINARY = Path(os.environ.get("AIMEE_TEST_PROXY_BINARY",
                             Path(__file__).resolve().parents[2] / "aimee")).resolve()


class Peer(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        self.do_POST()

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.server.requests.put((self.path, dict(self.headers), body))
        if self.path == "/v1/responses" and self.server.codex_response:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Connection", "close")
            self.end_headers()
            message = {"id": "msg_proxy_test", "type": "message", "role": "assistant",
                       "status": "completed", "content": [{"type": "output_text", "text": "PROXY_OK", "annotations": []}]}
            response = {"id": "resp_proxy_test", "object": "response", "created_at": 1,
                        "status": "completed", "model": "aimee", "output": [message],
                        "usage": {"input_tokens": 1, "output_tokens": 1, "total_tokens": 2}}
            events = [
                {"type": "response.created", "response": {**response, "status": "in_progress", "output": []}},
                {"type": "response.output_item.added", "output_index": 0,
                 "item": {**message, "status": "in_progress", "content": []}},
                {"type": "response.content_part.added", "item_id": message["id"], "output_index": 0,
                 "content_index": 0, "part": {"type": "output_text", "text": "", "annotations": []}},
                {"type": "response.output_text.delta", "item_id": message["id"], "output_index": 0,
                 "content_index": 0, "delta": "PROXY_OK"},
                {"type": "response.output_text.done", "item_id": message["id"], "output_index": 0,
                 "content_index": 0, "text": "PROXY_OK"},
                {"type": "response.content_part.done", "item_id": message["id"], "output_index": 0,
                 "content_index": 0, "part": message["content"][0]},
                {"type": "response.output_item.done", "output_index": 0, "item": message},
                {"type": "response.completed", "response": response},
            ]
            if self.server.codex_failed:
                events = [events[0], {"type": "response.failed", "response": {
                    **response, "status": "failed", "output": [],
                    "error": {"code": "model_not_found", "message": "test model unavailable"}}}]
            for event in events:
                self.wfile.write(f"event: {event['type']}\ndata: {json.dumps(event)}\n\n".encode())
                self.wfile.flush()
            self.close_connection = True
            return
        if self.path == "/v1/responses":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"data: first\n\n")
            self.wfile.flush()
            if b'"cancel"' in body:
                self.connection.settimeout(4)
                if self.connection.recv(1) == b"":
                    self.server.cancelled.set()
                self.close_connection = True
                return
            self.server.release.wait(5)
            try:
                self.wfile.write(b"data: second\n\n")
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                pass
            self.close_connection = True
        else:
            payload = b'{"error":"client certificate rejected"}' if self.server.reject else b'{"data":[]}'
            if self.server.gzip:
                payload = gzip.compress(payload)
            self.send_response(403 if self.server.reject else 200)
            self.send_header("Content-Type", "application/json")
            if self.server.gzip:
                self.send_header("Content-Encoding", "gzip")
            if self.server.chunked:
                self.send_header("Transfer-Encoding", "chunked")
            else:
                self.send_header("Content-Length", str(len(payload)))
            self.send_header("Connection", "close")
            self.end_headers()
            if self.server.chunked:
                payload = f"{len(payload):x}\r\n".encode() + payload + b"\r\n0\r\n\r\n"
            self.wfile.write(payload)
            self.close_connection = True


@unittest.skipUnless(os.name == "posix" and BINARY.is_file(), "requires a built POSIX thin client")
class ThinClientProxyTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="aimee-proxy-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith(("AIMEE_", "OPENAI_", "ANTHROPIC_"))}
        self.env.update(AIMEE_HOME=str(self.root), AIMEE_NO_CLIENT_INTEGRATIONS="1",
                        AIMEE_PROXY_TOKEN="local-test-secret", AIMEE_SERVER_TOKEN="remote-test-secret")
        self.env["CODEX_HOME"] = str(self.root / "codex-home")
        self.env["HOME"] = str(self.root)
        Path(self.env["CODEX_HOME"]).mkdir()
        self.peer = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Peer)
        self.peer.requests = queue.Queue()
        self.peer.release = threading.Event()
        self.peer.cancelled = threading.Event()
        self.peer.reject = False
        self.peer.gzip = False
        self.peer.chunked = False
        self.peer.codex_response = False
        self.peer.codex_failed = False
        self.peer_thread = None
        self.proxy = None
        self.addCleanup(self.cleanup_processes)

    def cleanup_processes(self):
        self.peer.release.set()
        if self.proxy:
            self.proxy.terminate()
            try:
                _, errors = self.proxy.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                self.proxy.kill()
                self.proxy.communicate()
                self.fail("proxy did not stop with active connections")
            self.assertNotIn(b"ERROR: AddressSanitizer", errors)
            self.assertNotIn(b"ERROR: LeakSanitizer", errors)
            self.assertNotIn(b"runtime error:", errors)
        if self.peer_thread:
            self.peer.shutdown()
            self.peer_thread.join(5)
        self.peer.server_close()

    def start_peer(self, tls=False):
        if tls:
            self.require_openssl()
            key, cert = self.root / "peer.key", self.root / "remote-ca.pem"
            subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
                            "ec_paramgen_curve:P-256", "-nodes", "-keyout", str(key),
                            "-out", str(cert), "-days", "1", "-subj", "/CN=proxy-test"],
                           check=True, capture_output=True)
            identity = self.root / "tls"
            identity.mkdir()
            shutil.copyfile(key, identity / "client.key")
            (identity / "client.key").chmod(0o600)
            shutil.copyfile(cert, identity / "client.crt")
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(str(cert), str(key))
            ctx.load_verify_locations(str(cert))
            ctx.verify_mode = ssl.CERT_REQUIRED
            self.peer.socket = ctx.wrap_socket(self.peer.socket, server_side=True)
        self.env["AIMEE_SERVER_URL"] = f'{"https" if tls else "http"}://127.0.0.1:{self.peer.server_port}'
        self.peer_thread = threading.Thread(target=self.peer.serve_forever, daemon=True)
        self.peer_thread.start()

    def require_openssl(self):
        if not shutil.which("openssl"):
            self.skipTest("openssl is required for the mTLS fixture")

    def start_proxy(self):
        self.proxy = subprocess.Popen([str(BINARY), "proxy", "--port", "0"], env=self.env,
                                      cwd=self.root, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        readable, _, _ = select.select([self.proxy.stderr], [], [], 10)
        self.assertTrue(readable, "proxy did not announce readiness")
        line = self.proxy.stderr.readline().decode()
        match = re.search(r"http://127\.0\.0\.1:(\d+)/v1", line)
        self.assertIsNotNone(match, line)
        self.port = int(match[1])

    def request(self, path="/v1/models", headers=None, body=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
        auth = {"Authorization": "Bearer local-test-secret"}
        if headers:
            auth.update(headers)
        conn.request("POST" if body is not None else "GET", path, body=body, headers=auth)
        return conn, conn.getresponse()

    def test_bearer_replacement_and_protocol_headers(self):
        self.start_peer()
        self.start_proxy()
        conn, response = self.request(headers={"OpenAI-Beta": "responses=v1",
            "X-Aimee-Proxy-Authorization": "must-not-forward", "Cookie": "must-not-forward",
            "session_id": "test-session"})
        self.assertEqual(response.status, 200)
        self.assertEqual(response.read(), b'{"data":[]}')
        conn.close()
        _, headers, _ = self.peer.requests.get(timeout=2)
        self.assertEqual(headers["Authorization"], "Bearer remote-test-secret.aimee-session.test-session")
        self.assertEqual(headers["OpenAI-Beta"], "responses=v1")
        self.assertNotIn("Cookie", headers)
        self.assertNotIn("X-Aimee-Proxy-Authorization", headers)
        self.assertNotIn("local-test-secret", str(headers))

    def test_anthropic_api_key_and_headers(self):
        self.start_peer()
        self.start_proxy()
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
        conn.request("POST", "/v1/messages", body=b'{"messages":[]}', headers={
            "x-api-key": "local-test-secret", "anthropic-version": "2023-06-01",
            "Content-Type": "application/json"})
        response = conn.getresponse()
        self.assertEqual(response.status, 200)
        response.read()
        conn.close()
        path, headers, body = self.peer.requests.get(timeout=2)
        self.assertEqual(path, "/v1/messages")
        self.assertEqual(body, b'{"messages":[]}')
        self.assertEqual(headers["Authorization"], "Bearer remote-test-secret")
        self.assertEqual(headers["anthropic-version"], "2023-06-01")
        self.assertNotIn("x-api-key", headers)

    def test_large_request_is_forwarded_without_rewriting(self):
        self.start_peer()
        self.start_proxy()
        body = b'{"input":"' + b"x" * (4 * 1024 * 1024 - 12) + b'"}'
        self.assertEqual(len(body), 4 * 1024 * 1024)
        conn, response = self.request("/v1/chat/completions", body=body)
        self.assertEqual(response.status, 200)
        response.read()
        conn.close()
        self.assertEqual(self.peer.requests.get(timeout=2)[2], body)

    def test_chunked_gzip_response_preserved(self):
        self.start_peer()
        self.peer.chunked = self.peer.gzip = True
        self.start_proxy()
        conn, response = self.request()
        self.assertEqual(response.status, 200)
        self.assertEqual(response.getheader("Transfer-Encoding"), "chunked")
        self.assertEqual(response.getheader("Content-Encoding"), "gzip")
        self.assertEqual(gzip.decompress(response.read()), b'{"data":[]}')
        conn.close()

    def test_stream_arrives_before_upstream_completes_and_other_requests_run(self):
        self.start_peer()
        self.start_proxy()
        conn, response = self.request("/v1/responses", body=b'{"stream":true}')
        self.assertEqual(response.readline(), b"data: first\n")
        self.assertFalse(self.peer.release.is_set())
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            def fetch(_):
                other, result = self.request()
                result.read()
                other.close()
                return result.status
            self.assertEqual(list(pool.map(fetch, range(3))), [200] * 3)
        self.peer.release.set()
        self.assertIn(b"data: second", response.read())
        conn.close()

    def test_rejects_auth_routes_and_ambiguous_framing_before_upstream(self):
        self.start_peer()
        self.start_proxy()
        cases = [
            ("/v1/models", "Authorization: Bearer wrong\r\n", 401),
            ("/v1/models", "", 401),
            ("/v1/api/rotate_bearer", "Authorization: Bearer local-test-secret\r\n", 404),
            ("http://example.com/v1/models", "Authorization: Bearer local-test-secret\r\n", 404),
            ("/v1/models", "Authorization: Bearer local-test-secret\r\nTransfer-Encoding: chunked\r\n", 400),
            ("/v1/models", "Authorization: Bearer local-test-secret\r\nContent-Length: 0\r\nContent-Length: 0\r\n", 400),
            ("/v1/models", "Authorization: Bearer local-test-secret\r\nContent-Length: 4194305\r\n", 413),
            ("/v1/models", "Authorization: Bearer local-test-secret\r\nOrigin: https://example.com\r\n", 400),
            ("/v1/models", "Authorization: Bearer local-test-secret\r\nsession_id: bad value\r\n", 400),
            ("/v1/models", "Authorization: Bearer local-test-secret\r\nAuthorization: Bearer local-test-secret\r\n", 401),
            ("/v1/models", "Authorization: Bearer local-test-secret\r\nContent-Length: -1\r\n", 400),
            ("/v1/models", "Authorization: Bearer local-test-secret\r\nContent-Length: 99999999999999999999999\r\n", 413),
            ("/v1/models", "Authorization: Bearer local-test-secret\r\nHost: example.com\r\n", 400),
            ("/v1/models", "Authorization: Bearer local-test-secret\r\nUpgrade: websocket\r\n", 400),
            ("/v1/models", "Authorization: Bearer local-test-secret\r\nExpect: 100-continue\r\n", 400),
            ("/v1/models", "Authorization: Bearer local-test-secret\r\n folded: value\r\n", 400),
            ("/v1/models", "Authorization: Bearer local-test-secret\r\nX-Invalid: embedded\x00value\r\n", 400),
        ]
        for path, fields, status in cases:
            with self.subTest(path=path, fields=fields), socket.create_connection(("127.0.0.1", self.port), 3) as conn:
                conn.sendall(f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{self.port}\r\n{fields}\r\n".encode())
                self.assertIn(f" {status} ".encode(), conn.recv(1024))
        self.assertTrue(self.peer.requests.empty())

    def test_header_limit_and_truncated_body_do_not_reach_upstream(self):
        self.start_peer()
        self.start_proxy()
        with socket.create_connection(("127.0.0.1", self.port), 3) as conn:
            conn.sendall(b"GET /v1/models HTTP/1.1\r\nX-Fill: " + b"x" * 65536)
            self.assertIn(b" 431 ", conn.recv(1024))
        with socket.create_connection(("127.0.0.1", self.port), 3) as conn:
            conn.sendall(f"POST /v1/responses HTTP/1.1\r\nHost: 127.0.0.1:{self.port}\r\nAuthorization: Bearer local-test-secret\r\nContent-Length: 10\r\n\r\nshort".encode())
            conn.shutdown(socket.SHUT_WR)
            self.assertEqual(conn.recv(1024), b"")
        self.assertTrue(self.peer.requests.empty())

    def test_missing_credentials_remote_and_invalid_ports_fail_startup(self):
        self.start_peer()
        for args, remove in [([], "AIMEE_PROXY_TOKEN"), ([], "AIMEE_SERVER_URL"),
                             (["--port", "65536"], None), (["--port", "-1"], None),
                             (["--port", "1oops"], None), (["--port"], None)]:
            with self.subTest(args=args, remove=remove):
                env = dict(self.env)
                if remove:
                    env.pop(remove)
                result = subprocess.run([str(BINARY), "proxy", *args], cwd=self.root, env=env,
                                        capture_output=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
        self.assertTrue(self.peer.requests.empty())

    def test_port_collision_is_reported_without_stealing_listener(self):
        self.start_peer()
        self.start_proxy()
        result = subprocess.run([str(BINARY), "proxy", "--port", str(self.port)], cwd=self.root,
                                env=self.env, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 1)
        conn, response = self.request()
        self.assertEqual(response.status, 200)
        response.read()
        conn.close()

    def test_unreachable_upstream_returns_502(self):
        self.start_peer()
        self.start_proxy()
        self.peer.shutdown()
        self.peer.server_close()
        self.peer_thread.join(5)
        self.peer_thread = None
        conn, response = self.request()
        self.assertEqual(response.status, 502)
        response.read()
        conn.close()

    def test_mtls_uses_existing_identity_and_preserves_rejection(self):
        self.start_peer(tls=True)
        self.start_proxy()
        conn, response = self.request()
        self.assertEqual(response.status, 200)
        response.read()
        conn.close()
        self.peer.reject = True
        conn, response = self.request()
        self.assertEqual(response.status, 403)
        self.assertIn(b"client certificate rejected", response.read())
        conn.close()

    def test_bad_pin_fails_closed(self):
        self.start_peer(tls=True)
        (self.root / "remote-ca.pem").write_text("invalid certificate")
        self.start_proxy()
        conn, response = self.request()
        self.assertEqual(response.status, 502)
        response.read()
        conn.close()
        self.assertTrue(self.peer.requests.empty())

    def test_missing_client_identity_fails_closed(self):
        self.start_peer(tls=True)
        (self.root / "tls/client.key").unlink()
        self.start_proxy()
        conn, response = self.request()
        self.assertEqual(response.status, 502)
        response.read()
        conn.close()
        self.assertTrue(self.peer.requests.empty())

    def test_insecure_client_key_permissions_fail_closed(self):
        self.start_peer(tls=True)
        (self.root / "tls/client.key").chmod(0o644)
        self.start_proxy()
        conn, response = self.request()
        self.assertEqual(response.status, 502)
        response.read()
        conn.close()
        self.assertTrue(self.peer.requests.empty())

    def test_shutdown_cancels_active_stream(self):
        self.start_peer()
        self.start_proxy()
        conn, response = self.request("/v1/responses", body=b'{"stream":true}')
        self.assertEqual(response.readline(), b"data: first\n")
        self.proxy.terminate()
        self.assertEqual(self.proxy.wait(timeout=3), 0)
        conn.close()

    def test_client_disconnect_cancels_quiet_upstream(self):
        self.start_peer()
        self.start_proxy()
        conn, response = self.request("/v1/responses", body=b'{"stream":true,"cancel":true}')
        self.assertEqual(response.readline(), b"data: first\n")
        response.close()
        conn.close()
        self.assertTrue(self.peer.cancelled.wait(2), "disconnected client left its upstream running")

    def test_certificate_status_does_not_misdiagnose_bearer_or_replace_identity(self):
        self.start_peer(tls=True)
        self.peer.reject = True
        original = (self.root / "tls/client.crt").read_bytes()
        result = subprocess.run([str(BINARY), "remote", "status"], cwd=self.root,
                                env=self.env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        self.assertIn("server rejected the client certificate", result.stdout)
        self.assertNotIn("rejected the stored token", result.stdout)
        self.assertEqual((self.root / "tls/client.crt").read_bytes(), original)

    def test_gateway_launch_configures_codex_and_owns_proxy_lifetime(self):
        self.start_peer()
        client = self.root / "codex"
        client.write_text("""#!/usr/bin/env python3
import json, os, sys, urllib.request
from pathlib import Path
profile = sys.argv[sys.argv.index('--profile') + 1]
profile_file = Path(os.environ['CODEX_HOME']) / (profile + '.config.toml')
assert profile_file.read_text() == '[plugins."aimee@local"]\\nenabled = false\\n'
assert profile_file.stat().st_mode & 0o777 == 0o600
base = os.environ['OPENAI_BASE_URL']
request = urllib.request.Request(base + '/models', headers={'Authorization': 'Bearer ' + os.environ['OPENAI_API_KEY']})
with urllib.request.urlopen(request, timeout=3) as response:
    assert response.status == 200
print(json.dumps({'argv': sys.argv[1:], 'base': base, 'token': os.environ['OPENAI_API_KEY']}))
assert 'AIMEE_SERVER_TOKEN' not in os.environ
sys.exit(7)
""")
        client.chmod(0o700)
        result = subprocess.run([str(BINARY), "launch", "--gateway", "--", str(client), "literal;$value"],
                                cwd=self.root, env=self.env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 7, result.stderr)
        actual = json.loads(result.stdout)
        self.assertIn('model_provider="aimee"', actual["argv"])
        self.assertIn('model="aimee"', actual["argv"])
        self.assertIn('plugins."aimee@local".enabled=false', actual["argv"])
        self.assertEqual(actual["argv"][-1], "literal;$value")
        self.assertEqual(list(Path(self.env["CODEX_HOME"]).glob('aimee-proxy-*.config.toml')), [])
        self.assertEqual(len(actual["token"]), 64)
        self.assertNotEqual(actual["token"], self.env["AIMEE_SERVER_TOKEN"])
        _, headers, _ = self.peer.requests.get(timeout=2)
        self.assertRegex(headers["Authorization"], r"^Bearer remote-test-secret\.aimee-session\.[0-9a-f]{32}$")
        port = int(actual["base"].split(":")[-1].split("/")[0])
        with self.assertRaises(OSError):
            socket.create_connection(("127.0.0.1", port), timeout=1)

    def test_actual_codex_parses_generated_provider_and_disabled_mcp(self):
        codex = shutil.which("codex")
        if not codex:
            if os.environ.get("AIMEE_TEST_REQUIRE_CODEX") == "1":
                self.fail("Codex is required by this gate but not installed")
            self.skipTest("Codex parser compatibility gate runs when Codex is installed")
        self.start_peer()
        config_dir = Path(self.env["CODEX_HOME"])
        config_dir.mkdir(exist_ok=True)
        config = config_dir / "config.toml"
        original = 'model = "user-selected-model"\n[plugins."aimee@local"]\nenabled = true\n'
        config.write_text(original)
        result = subprocess.run([str(BINARY), "launch", "--gateway", "--", codex, "mcp", "list"],
                                cwd=self.root, env=self.env, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(config.read_text(), original)
        self.assertFalse((config_dir / "plugins").exists(), "launcher installed a CLI plugin")

    def test_actual_codex_consumes_responses_stream_through_mtls_proxy(self):
        codex = shutil.which("codex")
        if not codex:
            if os.environ.get("AIMEE_TEST_REQUIRE_CODEX") == "1":
                self.fail("Codex is required by this gate but not installed")
            self.skipTest("Codex wire compatibility gate runs when Codex is installed")
        self.start_peer(tls=True)
        self.peer.codex_response = True
        # This is a deterministic local model fixture, with no provider calls
        # and no generated tool actions. It exercises the real Codex SSE parser.
        result = subprocess.run([str(BINARY), "launch", "--gateway", "--", codex,
                                 "exec", "--ephemeral", "--skip-git-repo-check", "--sandbox", "read-only",
                                 "Reply with PROXY_OK."], cwd=self.root, env=self.env,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("PROXY_OK", result.stdout)
        request_path, headers, body = self.peer.requests.get(timeout=2)
        self.assertEqual(request_path, "/v1/responses")
        self.assertEqual(json.loads(body)["model"], "aimee")
        self.assertTrue(headers["Authorization"].startswith("Bearer remote-test-secret.aimee-session."))

    def test_actual_codex_installed_plugin_hook_is_disabled_but_user_hook_runs(self):
        codex = shutil.which("codex")
        if not codex:
            if os.environ.get("AIMEE_TEST_REQUIRE_CODEX") == "1":
                self.fail("Codex is required by this gate but not installed")
            self.skipTest("requires Codex")
        self.start_peer(tls=True)
        self.peer.codex_response = True
        config_dir = Path(self.env["CODEX_HOME"])
        marketplace = self.root / "marketplace"
        manifest_dir = marketplace / ".agents/plugins"
        plugin = marketplace / "plugins/aimee"
        (plugin / ".codex-plugin").mkdir(parents=True)
        (plugin / "hooks").mkdir()
        manifest_dir.mkdir(parents=True)
        (plugin / ".codex-plugin/plugin.json").write_text(json.dumps({
            "name": "aimee", "version": "0.0.1", "description": "Proxy hook regression fixture"}))
        plugin_marker = self.root / "plugin-hook-ran"
        user_marker = self.root / "user-hook-ran"
        def hook(marker):
            # Controlled local fixture only: these hooks just create sentinels.
            return {"hooks": {"SessionStart": [{"hooks": [{"type": "command",
                "command": "touch " + shlex.quote(str(marker))}]}]}}
        (plugin / "hooks/hooks.json").write_text(json.dumps(hook(plugin_marker)))
        (manifest_dir / "marketplace.json").write_text(json.dumps({"name": "local", "plugins": [{
            "name": "aimee", "source": {"source": "local", "path": "./plugins/aimee"},
            "policy": {"installation": "AVAILABLE", "authentication": "ON_USE"},
            "category": "Coding"}]}))
        for args in (["plugin", "marketplace", "add", str(marketplace)],
                     ["plugin", "add", "aimee@local"]):
            installed = subprocess.run([codex, *args], cwd=self.root, env=self.env,
                                       capture_output=True, text=True, timeout=20)
            self.assertEqual(installed.returncode, 0, installed.stderr)
        (config_dir / "hooks.json").write_text(json.dumps(hook(user_marker)))
        original = (config_dir / "config.toml").read_bytes()
        args = [codex, "exec", "--ephemeral", "--skip-git-repo-check", "--sandbox", "read-only",
                "--dangerously-bypass-hook-trust", "Reply with PROXY_OK."]
        # Positive control: prove the installed plugin hook is discoverable.
        self.start_proxy()
        control = subprocess.run([*args[:1], "-c", 'model_provider="fixture"',
            "-c", 'model="aimee"', "-c", 'model_providers.fixture.name="fixture"',
            "-c", f'model_providers.fixture.base_url="http://127.0.0.1:{self.port}/v1"',
            "-c", 'model_providers.fixture.env_key="AIMEE_PROXY_TOKEN"',
            "-c", 'model_providers.fixture.request_max_retries=0',
            *args[1:]], cwd=self.root, env=self.env, capture_output=True, timeout=20)
        self.assertEqual(control.returncode, 0, control.stderr)
        self.assertTrue(plugin_marker.exists(), control.stderr)
        plugin_marker.unlink()
        user_marker.unlink()
        result = subprocess.run([str(BINARY), "launch", "--gateway", "--", *args],
                                cwd=self.root, env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("PROXY_OK", result.stdout)
        self.assertFalse(plugin_marker.exists(), "Aimee CLI plugin hook ran in proxy mode")
        self.assertTrue(user_marker.exists(), "proxy mode disabled unrelated user hooks")
        self.assertEqual((config_dir / "config.toml").read_bytes(), original)
        self.assertEqual(list(config_dir.glob('aimee-proxy-*.config.toml')), [])

    def test_explicit_codex_profile_is_preserved(self):
        self.start_peer()
        client = self.root / "codex"
        client.write_text("#!/usr/bin/env python3\nimport json, sys\nprint(json.dumps(sys.argv[1:]))\n")
        client.chmod(0o700)
        for args in (["--profile", "user-profile"], ["-p", "user-profile"],
                     ["--profile=user-profile"]):
            with self.subTest(args=args):
                result = subprocess.run([str(BINARY), "launch", "--gateway", "--", str(client), *args],
                    cwd=self.root, env=self.env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout)[-len(args):], args)
                self.assertIn("using your Codex profile", result.stderr)
                self.assertEqual(list(Path(self.env["CODEX_HOME"]).glob('aimee-proxy-*.config.toml')), [])

    def test_profile_creation_failure_does_not_launch_client(self):
        self.start_peer()
        config_dir = Path(self.env["CODEX_HOME"])
        config_dir.rmdir()
        config_dir.write_text("not a directory")
        client = self.root / "codex"
        client.write_text("#!/bin/sh\nexit 99\n")
        client.chmod(0o700)
        result = subprocess.run([str(BINARY), "launch", "--gateway", "--", str(client)],
            cwd=self.root, env=self.env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("could not create the temporary Codex proxy profile", result.stderr)
        self.assertEqual(config_dir.read_text(), "not a directory")

    def test_http_200_with_response_failed_is_not_reported_as_codex_success(self):
        codex = shutil.which("codex")
        if not codex:
            if os.environ.get("AIMEE_TEST_REQUIRE_CODEX") == "1":
                self.fail("Codex is required by this gate but not installed")
            self.skipTest("requires Codex")
        self.start_peer(tls=True)
        self.peer.codex_response = self.peer.codex_failed = True
        result = subprocess.run([str(BINARY), "launch", "--gateway", "--", codex,
                                 "-c", "model_providers.aimee.stream_max_retries=0",
                                 "-c", "model_providers.aimee.request_max_retries=0",
                                 "exec", "--ephemeral", "--skip-git-repo-check", "--sandbox", "read-only",
                                 "Reply with PROXY_OK."], cwd=self.root, env=self.env,
                                capture_output=True, text=True, timeout=20)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("test model unavailable", result.stderr)
        self.assertNotIn("PROXY_OK", result.stdout)

    def test_launcher_termination_stops_child_and_proxy(self):
        self.start_peer()
        client = self.root / "codex"
        client.write_text("""#!/usr/bin/env python3
import os, signal, time
signal.signal(signal.SIGTERM, lambda *_: exit(0))
print(os.environ['OPENAI_BASE_URL'], flush=True)
while True: time.sleep(0.1)
""")
        client.chmod(0o700)
        process = subprocess.Popen([str(BINARY), "launch", "--gateway", "--", str(client)],
                                   cwd=self.root, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.proxy = process
        readable, _, _ = select.select([process.stdout], [], [], 5)
        self.assertTrue(readable)
        base = process.stdout.readline().decode().strip()
        port = int(base.split(":")[-1].split("/")[0])
        process.terminate()
        self.assertEqual(process.wait(timeout=5), 0)
        self.assertEqual(list(Path(self.env["CODEX_HOME"]).glob('aimee-proxy-*.config.toml')), [])
        with self.assertRaises(OSError):
            socket.create_connection(("127.0.0.1", port), timeout=1)

    def test_nonexistent_client_returns_127_without_hanging(self):
        self.start_peer()
        result = subprocess.run([str(BINARY), "launch", "--gateway", "--", str(self.root / "codex")],
                                cwd=self.root, env=self.env, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 127)
        self.assertEqual(list(Path(self.env["CODEX_HOME"]).glob('aimee-proxy-*.config.toml')), [])


if __name__ == "__main__":
    if not BINARY.is_file():
        raise SystemExit(f"required thin-client binary does not exist: {BINARY}")
    unittest.main()
