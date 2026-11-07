"""Actual shared thinclient + Python binding against an isolated local HTTP UDS."""
import asyncio
import ctypes as C
import json
import os
from pathlib import Path
import tempfile
import ssl
import subprocess
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from model_client import ThinClient, _Options


LIBRARY = os.environ.get("AIMEE_TEST_CLIENT_LIBRARY")
MESSAGE = {"role": "user", "blocks": [{"type": "text", "text": "雪 {hi}"}]}
ANSWER = {"role": "assistant", "blocks": [
    {"type": "thinking", "text": "private reasoning", "signature": "opaque-signature"},
    {"type": "text", "text": "Hello World!", "context": {"origin": 7, "authority": 1, "trust": 2, "sensitivity": 1, "model_visible": True, "revision_domain": "", "revision_scope": "", "revision_epoch": "0"}},
]}


@unittest.skipUnless(LIBRARY, "set AIMEE_TEST_CLIENT_LIBRARY to the built shared thinclient")
class ThinClientTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        environment = dict(os.environ)
        for key in ("AIMEE_API_ENDPOINT", "AIMEE_API_BEARER", "AIMEE_SERVER_URL",
                    "AIMEE_SERVER_TOKEN", "AIMEE_PROFILE", "AIMEE_TLS_INSECURE", "SSL_CERT_FILE"):
            environment.pop(key, None)
        environment["AIMEE_HOME"] = self.directory.name
        self.environment = patch.dict(os.environ, environment, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.socket = Path(self.directory.name) / "http.sock"
        (Path(self.directory.name) / "remote.conf").write_text(
            "unix:" + str(self.socket) + "\nfixture-client-token\n")
        self.requests = []
        self.mode = "complete"
        self.owner = "uid:1000"
        self.alias = ""
        self.chat_started = asyncio.Event()
        self.closed = asyncio.Event()
        self.server = await asyncio.start_unix_server(self.serve, path=str(self.socket))
        self.addAsyncCleanup(self.close_server)
        self.client = ThinClient(SimpleNamespace(aimee_library=LIBRARY, aimee_profile=None,
                                                aimee_socket=self.socket))

    async def close_server(self):
        self.server.close()
        await self.server.wait_closed()

    async def serve(self, reader, writer):
        try:
            self.peer_certificate = writer.get_extra_info("peercert")
            headers = await reader.readuntil(b"\r\n\r\n")
            first, *lines = headers.decode().split("\r\n")
            method, path, _ = first.split()
            fields = dict(line.split(": ", 1) for line in lines if ": " in line)
            body = await reader.readexactly(int(fields.get("Content-Length", "0")))
            value = json.loads(body) if body else None
            self.requests.append((method, path, value, fields))
            if path == "/v1/sessions/create":
                response = {"status": "ok", "session_id": "generated-owned-session"}
            elif path == "/v1/sessions/get":
                response = {"status": "ok", "session": {"id": "generated-owned-session",
                                                         "principal": self.owner}}
            elif path.endswith("/primary"):
                if method == "POST":
                    self.alias = value["agent"]
                response = {"agent": self.alias}
            elif path == "/v1/chat/stream":
                self.chat_started.set()
                if self.mode == "idle-open":
                    await reader.read()
                    return
                events = []
                for sequence, event in enumerate([
                    {"kind": "execution", "payload": {"status": "running"}},
                    {"kind": "message", "message_id": "answer", "payload": ANSWER},
                    {"kind": "execution", "payload": {"status": "completed"}},
                ], 1):
                    events.append({"version": 1, "session_id": "generated-owned-session",
                                   "turn_id": "0123456789abcdef0123456789abcdef", "actor": "alice",
                                   "event_id": f"event-{sequence}", "sequence": str(sequence),
                                   "event": event})
                if self.mode == "partial":
                    events.pop()
                raw = b"".join(json.dumps(e).encode() + b"\n" for e in events)
                writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\nConnection: close\r\n\r\n")
                await writer.drain()
                if self.mode == "idle-read":
                    await reader.read()
                    return
                # Real C transport owns HTTP/NDJSON fragmentation and canonical collection.
                for at in range(0, len(raw), 17):
                    writer.write(raw[at:at + 17])
                    await writer.drain()
                return
            elif path == "/v1/memory/search":
                response = {"facts": [{"key": "fact", "content": "domain data 雪"}]}
            elif path == "/v1/dev/submit":
                response = {"work_item_id": "wi_test"}
            else:
                raise AssertionError(path)
            raw = json.dumps(response).encode()
            status = b"503 Unavailable" if self.mode == "domain-refused" else b"200 OK"
            writer.write(b"HTTP/1.1 " + status + b"\r\nContent-Length: " + str(len(raw)).encode()
                         + b"\r\nConnection: close\r\n\r\n" + raw)
            await writer.drain()
        finally:
            writer.close()
            await writer.wait_closed()
            if self.chat_started.is_set():
                self.closed.set()

    async def test_shared_collector_preserves_blocks_and_server_session(self):
        result = await self.client.chat(MESSAGE, "discord-channel", "registered-bot")
        self.assertEqual(result[0]["role"], "assistant")
        self.assertEqual([(b["type"], b.get("text"), b.get("signature"))
                          for b in result[0]["blocks"]],
                         [("thinking", "private reasoning", "opaque-signature"),
                          ("text", "Hello World!", None)])
        await self.client.chat(MESSAGE, "discord-channel", "registered-bot")
        self.assertEqual(sum(path == "/v1/sessions/create" for _, path, _, _ in self.requests), 1)
        chats = [value for _, path, value, _ in self.requests if path == "/v1/chat/stream"]
        self.assertEqual(len(chats), 2)
        for value in chats:
            self.assertEqual(value["message"], MESSAGE)
            self.assertEqual(value["aimee_session_id"], "generated-owned-session")
            self.assertEqual(value["ir_version"], 1)
            self.assertFalse(value["control_replies"])
            self.assertNotIn("agent", value)
        self.assertEqual(self.alias, "registered-bot")
        self.assertTrue(all(fields.get("Authorization") == "Bearer fixture-client-token"
                            for _, _, _, fields in self.requests))

    async def test_domain_memory_and_workflow_use_same_actual_C_transport(self):
        from workflow_chat import WorkflowClient
        body = {"store": "user", "keywords": ["雪"], "limit": 4}
        self.assertEqual((await self.client.request("POST", "/v1/memory/search", body))["facts"][0]["content"], "domain data 雪")
        workflow = WorkflowClient(self.client)
        self.assertEqual((await workflow.call("POST", "/v1/dev/submit", {"workflow": "discord-paper"}, idempotency="stable"))["work_item_id"], "wi_test")
        self.assertEqual(self.requests[0][2], body)
        self.assertEqual(self.requests[1][2], {"workflow": "discord-paper"})
        self.assertEqual(self.requests[1][3]["Idempotency-Key"], "stable")
        self.assertTrue(all(row[3]["Authorization"] == "Bearer fixture-client-token" for row in self.requests))
        self.assertFalse(any("/sessions/" in row[1] for row in self.requests))
        self.mode = "domain-refused"
        with self.assertRaises(RuntimeError):
            await workflow.call("POST", "/v1/dev/submit", {})

    async def test_actual_workflow_inference_uses_canonical_collector_and_fresh_sessions(self):
        from workflow_chat import ChatWorkflow
        runner = ChatWorkflow.__new__(ChatWorkflow)
        runner.bot = SimpleNamespace(config=SimpleNamespace(aimee_library=LIBRARY,
            aimee_profile=None, aimee_socket=self.socket, model="registered-writer"))
        self.assertEqual(await runner.infer("Assess only this task", "Choose a section", max_tokens=64), "Hello World!")
        self.assertEqual(await runner.infer("Write this section", "Independent task"), "Hello World!")
        self.assertEqual(sum(row[1] == "/v1/sessions/create" for row in self.requests), 2)
        chats = [row[2] for row in self.requests if row[1] == "/v1/chat/stream"]
        self.assertEqual(len(chats), 2)
        self.assertEqual(chats[0]["message"]["blocks"][0]["text"], "Assess only this task")
        self.assertIn("Choose a section", chats[0]["message"]["blocks"][1]["text"])
        self.assertEqual(chats[0]["message"]["blocks"][0]["context"]["origin"], 2)
        self.assertEqual(chats[1]["message"]["blocks"][0]["text"], "Write this section")
        self.assertFalse(any("/v1/memory/" in row[1] or row[1].endswith("/completions") for row in self.requests))
        self.assertNotIn("messages", chats[0])
        self.assertEqual(self.alias, "registered-writer")

    async def test_actual_https_mtls_pin_and_identity_fail_closed(self):
        root = Path(self.directory.name)
        tls = root / "tls"
        tls.mkdir(mode=0o700)

        def certificate(name):
            cert, key = root / (name + ".crt"), root / (name + ".key")
            subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
                            "ec_paramgen_curve:P-256", "-nodes", "-days", "1",
                            "-subj", "/CN=" + name, "-addext", "subjectAltName=DNS:" + name,
                            "-keyout", str(key), "-out", str(cert)], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            key.chmod(0o600)
            return cert, key

        server_cert, server_key = certificate("wrong-host.invalid")
        client_cert, client_key = certificate("fixture-client")
        (tls / "client.crt").write_bytes(client_cert.read_bytes())
        (tls / "client.key").write_bytes(client_key.read_bytes())
        (tls / "client.key").chmod(0o600)
        pin = root / "remote-ca.pem"
        pin.write_bytes(server_cert.read_bytes())
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(server_cert, server_key)
        context.load_verify_locations(client_cert)
        context.verify_mode = ssl.CERT_REQUIRED
        await self.close_server()
        self.server = await asyncio.start_server(self.serve, "127.0.0.1", 0, ssl=context)
        port = self.server.sockets[0].getsockname()[1]
        # Normal enrolled profile resolution and authenticated model streaming use TLS.
        (root / "remote.conf").write_text(f"https://127.0.0.1:{port}\nfixture-client-token\n")
        self.client.socket = None
        result = await self.client.chat(MESSAGE, "discord-channel", "registered-bot")
        self.assertEqual(result[0]["blocks"][-1]["text"], "Hello World!")
        self.assertTrue(self.peer_certificate)
        def direct_https():
            lib = self.client._load()
            stream, output = lib.cli_http_stream_new(), C.c_void_p()
            home = C.c_char_p(os.fsencode(root))
            options = _Options(endpoint=f"https://127.0.0.1:{port}".encode(),
                               io_timeout_ms=2000, connect_timeout_ms=1000,
                               tls_home=C.cast(home, C.c_void_p))
            request = self.client._json({"message": MESSAGE,
                                         "aimee_session_id": "generated-owned-session"})
            try:
                return lib.cli_chat_request_stream_json(stream, C.byref(options), request,
                                                       C.byref(output))
            finally:
                lib.cli_chat_string_free(output)
                lib.cli_http_stream_free(stream)

        # Direct raw SDK URL input must honor https as well as the normalized tls form.
        self.assertEqual(await asyncio.to_thread(direct_https), 0)
        before = len(self.requests)
        loop = asyncio.get_running_loop()
        previous_handler = loop.get_exception_handler()
        handshake_refusals, unexpected = [], []

        def rejected_handshake(_loop, details):
            error = details.get("exception")
            if (details.get("message") == "Error on transport creation for incoming connection"
                    and isinstance(error, (ssl.SSLError, ConnectionResetError))):
                handshake_refusals.append(type(error).__name__)
            else:
                unexpected.append(details.get("message"))
                _loop.default_exception_handler(details)

        loop.set_exception_handler(rejected_handshake)
        self.addCleanup(loop.set_exception_handler, previous_handler)
        pin.write_bytes(client_cert.read_bytes())
        with self.assertRaises(RuntimeError):
            await self.client.chat(MESSAGE, "discord-channel", "registered-bot")
        self.assertEqual(len(self.requests), before)
        pin.write_bytes(server_cert.read_bytes())
        (tls / "client.key").chmod(0o644)
        with self.assertRaises(RuntimeError):
            await self.client.chat(MESSAGE, "discord-channel", "registered-bot")
        self.assertEqual(len(self.requests), before)
        (tls / "client.key").chmod(0o600)
        # A trusted issuer is insufficient when unpinned hostname verification fails.
        pin.unlink()
        os.environ["SSL_CERT_FILE"] = str(server_cert)
        subprocess.run(["openssl", "verify", "-CAfile", str(server_cert), str(server_cert)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with self.assertRaises(RuntimeError):
            await self.client.chat(MESSAGE, "discord-channel", "registered-bot")
        self.assertEqual(len(self.requests), before)
        pin.write_bytes(server_cert.read_bytes())
        result = await self.client.chat(MESSAGE, "discord-channel", "registered-bot")
        self.assertEqual(result[0]["blocks"][-1]["text"], "Hello World!")
        self.assertEqual(len(handshake_refusals), 3)
        self.assertFalse(unexpected)

    async def test_changed_owner_refuses_before_primary_or_chat(self):
        await self.client.chat(MESSAGE, "discord-channel", "registered-bot")
        self.owner = "other-principal"
        before = len(self.requests)
        with self.assertRaisesRegex(RuntimeError, "owner changed"):
            await self.client.chat(MESSAGE, "discord-channel", "registered-bot")
        self.assertEqual([path for _, path, _, _ in self.requests[before:]], ["/v1/sessions/get"])

    async def test_existing_session_selection_is_not_overwritten(self):
        await self.client.chat(MESSAGE, "discord-channel", "registered-bot")
        self.alias = "other-registered-alias"
        before = len(self.requests)
        with self.assertRaisesRegex(RuntimeError, "alias changed"):
            await self.client.chat(MESSAGE, "discord-channel", "registered-bot")
        self.assertEqual([(method, path) for method, path, _, _ in self.requests[before:]],
                         [("POST", "/v1/sessions/get"),
                          ("GET", "/v1/sessions/generated-owned-session/primary")])

    async def test_incomplete_turn_has_no_deliverable_output(self):
        self.mode = "partial"
        with self.assertRaises(RuntimeError):
            await self.client.chat(MESSAGE, "discord-channel", "registered-bot")

    async def test_cancellation_interrupts_open_and_body_then_joins(self):
        for mode in ("idle-open", "idle-read"):
            with self.subTest(mode=mode):
                self.mode = mode
                self.chat_started.clear()
                self.closed.clear()
                task = asyncio.create_task(self.client.chat(MESSAGE, "discord-channel", "registered-bot"))
                await asyncio.wait_for(self.chat_started.wait(), 2)
                task.cancel()
                with self.assertRaises(asyncio.CancelledError):
                    await asyncio.wait_for(task, 2)
                await asyncio.wait_for(self.closed.wait(), 2)

    async def test_deadline_interrupts_same_c_handle(self):
        self.mode = "idle-read"
        self.client.timeout = .1
        with self.assertRaises(TimeoutError):
            await self.client.chat(MESSAGE, "discord-channel", "registered-bot")
        await asyncio.wait_for(self.closed.wait(), 2)


class InputTests(unittest.IsolatedAsyncioTestCase):
    def test_container_ships_shared_thinclient_without_server_or_cli_command(self):
        dockerfile = Path(__file__).with_name("Dockerfile").read_text()
        self.assertIn("--target aimee-client", dockerfile)
        self.assertIn("/build/libaimee-client.so /usr/local/lib/libaimee-client.so", dockerfile)
        runtime = dockerfile.split("FROM python:3.13-slim-bookworm\n", 1)[1]
        self.assertNotIn("/build/aimee ", runtime)
        self.assertNotIn("aimee-server", runtime)

    async def test_request_limit_precedes_library_load(self):
        client = ThinClient(SimpleNamespace(aimee_library="/missing", aimee_profile=None,
                                          aimee_socket=None))
        with self.assertRaises(ValueError):
            await client.chat({"text": "x" * (1024 * 1024)}, "discord-channel", "bot")
