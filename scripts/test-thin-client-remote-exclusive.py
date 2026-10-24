#!/usr/bin/env python3
"""Black-box regression test for exclusive thin-client transport selection."""

from __future__ import annotations

import argparse
import http.server
import json
import os
import pathlib
import socket
import ssl
import subprocess
import tempfile
import threading


class RemoteHandler(http.server.BaseHTTPRequestHandler):
    # ThreadingHTTPServer serves each request on its own thread, so the request
    # counter is shared mutable state: `requests += 1` is a non-atomic
    # read-modify-write that can lose an increment under concurrency and make an
    # assertion flaky. Guard every access with a lock.
    requests = 0
    paths: list[str] = []
    workspaces = ["/tmp"]
    _lock = threading.Lock()

    @classmethod
    def count(cls) -> int:
        with cls._lock:
            return cls.requests

    @classmethod
    def paths_since(cls, start: int) -> list[str]:
        with cls._lock:
            return cls.paths[start:]

    def _respond(self) -> None:
        with type(self)._lock:
            type(self).requests += 1
            type(self).paths.append(self.path)
        length = int(self.headers.get("Content-Length", "0"))
        if length:
            self.rfile.read(length)
        # The client asks the server which methods it routes before it can route
        # anything (GET /v1/cli/manifest); it no longer carries a compiled-in
        # map. A stand-in server therefore has to answer that too, or every
        # command fails before the request under test is ever made. Only the
        # methods this test drives need rows.
        if self.path.startswith("/v1/cli/manifest"):
            body = json.dumps(
                {
                    "manifest_version": 1,
                    "server_version": "stub",
                    "routes": [
                        {"op": "model.list", "verb": "GET", "path": "/v1/models"},
                        {"op": "agent.list", "verb": "GET", "path": "/v1/agents"},
                        {"op": "memory.search", "verb": "POST", "path": "/v1/memory/search"},
                        {"op": "config.show", "verb": "GET", "path": "/v1/config"},
                        {"op": "workspace.list", "verb": "GET", "path": "/v1/workspaces"},
                    ],
                }
            ).encode()
        elif self.path == "/v1/workspaces":
            body = json.dumps({"status": "ok", "workspaces": type(self).workspaces}).encode()
        else:
            body = json.dumps(
                {"status": "ok", "agents": [], "any_delegate_available": False}
            ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    do_GET = _respond
    do_POST = _respond

    def log_message(self, _format: str, *_args: object) -> None:
        pass


class LocalUdsSentinel:
    """A local unix socket that must never be contacted.

    `connect()` to a listening socket succeeds as soon as the kernel queues the
    connection, well before this server accepts it. Reading `contacts` right
    after the client exits would therefore race a forbidden connection still
    sitting in the backlog, and the test would pass on exactly the regression it
    exists to catch. `contacts_after_settle()` closes that race: accepts are
    FIFO, so once the sentinel has served a probe connection opened *after* the
    client exited, every earlier queued connection has already been counted.
    """

    def __init__(self, path: pathlib.Path) -> None:
        self.contacts = 0
        self._path = path
        # `contacts` is written on the accept thread and read on the test
        # thread; guard both so the counter has no unsynchronized access, matching
        # RemoteHandler.
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.bind(str(path))
        self._sock.listen()
        self._sock.settimeout(0.1)
        self._thread = threading.Thread(target=self._serve, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def _serve(self) -> None:
        while not self._stop.is_set():
            try:
                conn, _ = self._sock.accept()
            except TimeoutError:
                continue
            with conn:
                with self._lock:
                    self.contacts += 1
                # Bound the handler: a contact that connects without sending
                # would otherwise block the accept loop forever, so nothing
                # queued behind it could be counted.
                conn.settimeout(2)
                try:
                    conn.recv(65536)
                    body = b'{"status":"ok","agents":[],"any_delegate_available":false}'
                    conn.sendall(
                        b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        + f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n".encode()
                        + body
                    )
                except OSError:
                    pass

    def contacts_after_settle(self) -> int:
        """Drain the accept backlog, then report contacts excluding the probe."""
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
            probe.settimeout(5)
            probe.connect(str(self._path))
            probe.sendall(b"\n")
            probe.shutdown(socket.SHUT_WR)
            while probe.recv(65536):
                pass
        # The probe was served, so every connection queued before it was too.
        # Both increments happened on the sentinel thread; this read is ordered
        # after them by the probe's own completed response.
        with self._lock:
            self.contacts -= 1
            return self.contacts

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1)
        self._sock.close()


def run_client(
    binary: pathlib.Path,
    home: pathlib.Path,
    endpoint: str,
    command: list[str],
    integrations: bool = False,
) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env.update(
        {
            "HOME": str(home),
            "AIMEE_HOME": str(home),
            "AIMEE_API_ENDPOINT": endpoint,
            "AIMEE_API_BEARER": "test-only-token",
        }
    )
    if integrations:
        (home / ".claude").mkdir(exist_ok=True)
    else:
        env["AIMEE_NO_CLIENT_INTEGRATIONS"] = "1"
    for key in ("AIMEE_SERVER_URL", "AIMEE_SERVER_TOKEN", "AIMEE_TLS_INSECURE",
                "AIMEE_SESSION_ID", "CLAUDE_SESSION_ID", "CODEX_THREAD_ID", "AIMEE_HOOK_CLIENT"):
        env.pop(key, None)
    return subprocess.run(
        [str(binary), "--json", *command],
        env=env,
        cwd=home,
        text=True,
        capture_output=True,
        input='{"tool_name":"Read","tool_input":{},"cwd":"/tmp"}\n'
        if command[:1] == ["hooks"]
        else json.dumps({"session_id": "remote-exclusive-start", "cwd": str(home)}) + "\n"
        if command == ["session-start"]
        else None,
        timeout=20,
        check=False,
    )


def unused_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class NativeHandler(http.server.BaseHTTPRequestHandler):
    paths: list[str] = []
    requests: list[dict] = []
    error = False

    def respond(self) -> None:
        type(self).paths.append(self.path)
        assert self.headers.get("Authorization") == "Bearer exact-child-grant"
        assert self.headers.get("Host") == f"127.0.0.1:{self.server.server_port}"
        if self.command == "GET":
            assert self.path == "/v1/cli/manifest"
            body = {"manifest_version": 1, "marshal": [], "routes": [
                {"op": "index.investigate", "verb": "POST", "path": "/v1/index/investigate"}
            ]}
        else:
            assert self.path == "/v1/native/cli"
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            type(self).requests.append(request)
            nonce = request["_cli_request_id"]
            assert len(nonce) == 32 and all(c in "0123456789abcdef" for c in nonce)
            body = {"content": [{"type": "text", "text": "owned result"}],
                    "isError": type(self).error}
        raw = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    do_GET = respond
    do_POST = respond

    def log_message(self, _format: str, *_args: object) -> None:
        pass


def native_main_check(binary: pathlib.Path) -> None:
    """Run actual main(), including bootstrap, with only a child capability."""
    with tempfile.TemporaryDirectory(prefix="aimee-native-main-") as tmp:
        home = pathlib.Path(tmp)
        (home / ".claude").mkdir()
        (home / "remote.conf").write_text("tcp:127.0.0.1:1\noperator-secret\n")
        (home / "private-auth.json").write_text('{"access_token":"never-export-this"}')
        original = {p.name: p.read_bytes() for p in home.iterdir() if p.is_file()}
        sentinel = LocalUdsSentinel(home / "aimee-http.sock")
        sentinel.start()
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), NativeHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = dict(os.environ, HOME=tmp, AIMEE_HOME=tmp, AIMEE_NATIVE_CLI="1",
                   AIMEE_API_ENDPOINT=f"http://127.0.0.1:{server.server_port}",
                   AIMEE_API_BEARER="exact-child-grant",
                   AIMEE_SERVER_TOKEN="wrong-global-token",
                   AIMEE_CONFIGURE_CLIENT_INTEGRATIONS_ONLY="1")
        def run(command: list[str], changes: dict | None = None) -> subprocess.CompletedProcess:
            return subprocess.run([str(binary), *command], env=env | (changes or {}),
                                  capture_output=True, text=True, timeout=20)
        try:
            result = run(["--json", "index", "investigate", "actual main query"])
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert json.loads(result.stdout)["isError"] is False
            assert NativeHandler.requests[-1]["query"] == "actual main query"
            assert NativeHandler.paths == ["/v1/cli/manifest", "/v1/native/cli"]
            NativeHandler.error = True
            result = run(["index", "investigate", "tool refusal"])
            assert result.returncode != 0, (result.stdout, result.stderr)
            before = len(NativeHandler.paths)
            for command in (["agent", "token", "private"], ["remote", "set", "tcp:127.0.0.1:1"],
                            ["--profile", "foreign", "agent", "token", "private"]):
                result = run(command)
                assert result.returncode != 0 and "never-export-this" not in result.stdout
            # Native SDKs use this registered stdio service. A clean EOF closes
            # it successfully without granting ordinary credential/profile access.
            result = subprocess.run([str(binary), "mcp-serve"], env=env, input="",
                                    capture_output=True, text=True, timeout=20)
            assert result.returncode == 0 and result.stdout == "", (result.returncode, result.stdout)
            assert "never-export-this" not in result.stderr
            result = run(["index", "investigate", "no grant"], {"AIMEE_API_BEARER": ""})
            assert result.returncode != 0
            result = run(["index", "investigate", "no endpoint"], {"AIMEE_API_ENDPOINT": ""})
            assert result.returncode != 0
            assert all(p == "/v1/cli/manifest" for p in NativeHandler.paths[before:])
            assert sentinel.contacts_after_settle() == 0
            assert {p.name: p.read_bytes() for p in home.iterdir() if p.is_file()} == original
            assert sorted(p.name for p in home.iterdir()) == [
                ".claude", "aimee-http.sock", "private-auth.json", "remote.conf"]
            assert not list((home / ".claude").iterdir())
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=1)
            sentinel.close()


def network_authority_check(binary: pathlib.Path) -> None:
    """Real TLS verifies the bare hostname while HTTP retains its authority."""
    class AuthorityHandler(RemoteHandler):
        seen: list[str] = []
        authority = ""

        def _respond(self) -> None:
            assert self.headers.get("Host") == type(self).authority
            type(self).seen.append(self.path)
            super()._respond()

        do_GET = _respond
        do_POST = _respond

    with tempfile.TemporaryDirectory(prefix="aimee-http-authority-") as tmp:
        home = pathlib.Path(tmp)
        cert, key = home / "server.crt", home / "server.key"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                        "-days", "1", "-keyout", str(key), "-out", str(cert)],
                       check=True, capture_output=True, timeout=20)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), AuthorityHandler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        names: list[str | None] = []
        context.set_servername_callback(lambda _ssl, name, _context: names.append(name))
        server.socket = context.wrap_socket(server.socket, server_side=True)
        AuthorityHandler.authority = f"localhost:{server.server_port}"
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = dict(os.environ, HOME=tmp, AIMEE_HOME=tmp, SSL_CERT_FILE=str(cert),
                   AIMEE_API_ENDPOINT=f"tls:localhost:{server.server_port}",
                   AIMEE_API_BEARER="test-transport")
        for key_name in ("AIMEE_TLS_INSECURE", "AIMEE_NATIVE_CLI"):
            env.pop(key_name, None)
        try:
            good = subprocess.run([str(binary), "--json", "agent", "list"], env=env,
                                  capture_output=True, text=True, timeout=20)
            assert good.returncode == 0, (good.stdout, good.stderr)
            assert names and all(name == "localhost" for name in names), names
            assert "/v1/models" in AuthorityHandler.seen
            before = len(AuthorityHandler.seen)
            # Same trusted certificate, wrong IP identity: no request may pass.
            wrong = env | {"AIMEE_API_ENDPOINT": f"tls:127.0.0.1:{server.server_port}"}
            bad = subprocess.run([str(binary), "--json", "agent", "list"], env=wrong,
                                 capture_output=True, text=True, timeout=20)
            assert bad.returncode != 0 and len(AuthorityHandler.seen) == before
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=1)

        class IPv6Server(http.server.ThreadingHTTPServer):
            address_family = socket.AF_INET6

        ipv6 = IPv6Server(("::1", 0), AuthorityHandler)
        AuthorityHandler.authority = f"[::1]:{ipv6.server_port}"
        AuthorityHandler.seen = []
        thread = threading.Thread(target=ipv6.serve_forever, daemon=True)
        thread.start()
        try:
            env["AIMEE_API_ENDPOINT"] = f"tcp:[::1]:{ipv6.server_port}"
            result = subprocess.run([str(binary), "--json", "agent", "list"], env=env,
                                    capture_output=True, text=True, timeout=20)
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert "/v1/models" in AuthorityHandler.seen
        finally:
            ipv6.shutdown()
            ipv6.server_close()
            thread.join(timeout=1)


def network_mcp_bootstrap_check(binary: pathlib.Path) -> None:
    """The actual network stdio owner must keep bootstrap prose off JSON-RPC.

    HTTP index/catalog responses are explicit service peers; repository identity,
    collection, ingest and MCP framing run in the real CLI, without owner stubs.
    """
    class BootstrapHandler(http.server.BaseHTTPRequestHandler):
        def respond(self) -> None:
            size = int(self.headers.get("Content-Length", "0"))
            request = json.loads(self.rfile.read(size)) if size else None
            self.server.calls.append((self.path, request))
            status = 200
            if self.path == "/v1/cli/manifest":
                body = {"manifest_version": 1, "routes": [
                    {"op": "index.list", "verb": "GET", "path": "/v1/index/projects"},
                    {"op": "mcp.tools_list", "verb": "GET", "path": "/v1/mcp/tools_list"},
                ]}
            elif self.path == "/v1/index/projects":
                if self.server.mode == "bootstrap-failure":
                    status = 503
                    body = {"status": "error", "message": "fixture index unavailable"}
                else:
                    projects = [{"name": "canonical-mcp-bootstrap", "root": str(repo)}]
                    body = {"status": "ok", "projects": projects if self.server.mode == "indexed" else []}
            elif self.path == "/v1/index/ingest":
                assert request["name"] == "canonical-mcp-bootstrap", request
                assert request["root"] == str(repo)
                body = {"status": "ok"}
            elif self.path == "/v1/mcp/tools_list":
                if self.server.mode == "tool-failure":
                    status = 403
                    body = {"status": "error", "message": "fixture catalog denied"}
                else:
                    body = {"status": "ok", "tools": [], "generation": 1}
            else:
                status = 404
                body = {"status": "error", "message": "unexpected fixture route"}
            raw = json.dumps(body).encode()
            self.send_response(status)
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(raw)

        do_GET = respond
        do_POST = respond

        def log_message(self, _format: str, *_args: object) -> None:
            pass

    with tempfile.TemporaryDirectory(prefix="aimee-mcp-bootstrap-") as tmp:
        root = pathlib.Path(tmp)
        repo, home = root / "repo", root / "home"
        repo.mkdir()
        home.mkdir()
        (repo / "aimee.workspace.yaml").write_text("id: canonical-mcp-bootstrap\n")
        (repo / "main.py").write_text("print('actual collected source')\n")
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), BootstrapHandler)
        server.calls = []
        server.mode = "unindexed"
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = dict(os.environ, HOME=str(home), AIMEE_HOME=str(home),
                   AIMEE_API_ENDPOINT=f"tcp:127.0.0.1:{server.server_port}",
                   AIMEE_API_BEARER="test-only-bootstrap", AIMEE_NO_CLIENT_INTEGRATIONS="1")
        for name in ("AIMEE_NATIVE_CLI", "AIMEE_SERVER_URL", "AIMEE_SERVER_TOKEN",
                     "AIMEE_SESSION_ID", "CLAUDE_SESSION_ID", "CODEX_THREAD_ID"):
            env.pop(name, None)
        initialize = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}
        listing = {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}
        wire = "".join(json.dumps(frame) + "\n" for frame in (initialize, listing))
        try:
            for mode in ("unindexed", "indexed", "bootstrap-failure", "tool-failure"):
                server.mode, server.calls = mode, []
                result = subprocess.run([str(binary), "mcp-serve"], env=env, cwd=repo,
                                        input=wire, capture_output=True, text=True, timeout=20)
                assert result.returncode == 0, (mode, result.stdout, result.stderr)
                # Parse every byte-bearing stdout line; do not discard progress
                # or redirect stdout, which would hide the real protocol defect.
                try:
                    frames = [json.loads(line) for line in result.stdout.splitlines()]
                except json.JSONDecodeError as error:
                    raise AssertionError(("non-JSON MCP stdout", mode, result.stdout)) from error
                assert len(frames) == 2 and all(f["jsonrpc"] == "2.0" for f in frames), (mode, frames)
                assert frames[0]["id"] == 1 and frames[0]["result"]["serverInfo"]["name"] == "aimee"
                if mode == "tool-failure":
                    assert frames[1]["id"] == 2 and frames[1]["error"]["code"] == -32603
                    assert "fixture catalog denied" in frames[1]["error"]["message"]
                else:
                    assert frames[1] == {"jsonrpc": "2.0", "id": 2, "result": {"tools": []}}
                ingest = [body for path, body in server.calls if path == "/v1/index/ingest"]
                if mode in ("unindexed", "tool-failure"):
                    assert [r["phase"] for r in ingest] == ["begin", "stage", "seal"], ingest
                    files = {f["rel_path"]: f["content"] for f in ingest[1]["files"]}
                    assert files["main.py"] == "print('actual collected source')\n"
                    assert ingest[-1]["expected_files"] == len(files)
                else:
                    assert not ingest, (mode, ingest)
                assert ("could not bootstrap the remote code index" in result.stderr) == (mode == "bootstrap-failure")

            # The same real checkout must not trigger network indexing in
            # scoped-native mode or when the selected transport is local UDS.
            # These are framing/transport checks, not affirmative tool grants.
            for scoped in (True, False):
                server.calls = []
                changes = {"AIMEE_NATIVE_CLI": "1"} if scoped else {}
                sentinel = None
                if not scoped:
                    sentinel = LocalUdsSentinel(home / "aimee-http.sock")
                    sentinel.start()
                    changes["AIMEE_API_ENDPOINT"] = "unix:" + str(home / "aimee-http.sock")
                try:
                    result = subprocess.run([str(binary), "mcp-serve"], env=env | changes,
                                            cwd=repo, input=json.dumps(initialize) + "\n",
                                            capture_output=True, text=True, timeout=20)
                    assert result.returncode == 0, (scoped, result.stdout, result.stderr)
                    frame = json.loads(result.stdout)
                    assert frame["id"] == 1 and "result" in frame, (scoped, frame)
                    assert not server.calls, (scoped, server.calls)
                    assert "indexing project:" not in result.stdout
                finally:
                    if sentinel:
                        sentinel.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("--native-proxy-fixture", type=pathlib.Path)
    args = parser.parse_args()
    binary = args.binary.resolve()

    with tempfile.TemporaryDirectory(prefix="aimee-remote-exclusive-") as tmp:
        home = pathlib.Path(tmp)
        # Presence of a Claude config directory drives the ordinary startup
        # integration path that performs the delegate-availability probe.
        (home / ".claude").mkdir()
        sentinel = LocalUdsSentinel(home / "aimee-http.sock")
        sentinel.start()
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), RemoteHandler)
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)
        server_thread.start()
        try:
            port = int(server.server_address[1])
            result = run_client(binary, home, f"tcp:127.0.0.1:{port}", ["agent", "list"], True)
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert RemoteHandler.count() >= 2, RemoteHandler.count()
            assert sentinel.contacts_after_settle() == 0, (
                "configured remote also contacted local UDS"
            )

            # Each independently special-cased path must obey the same exclusive
            # selection rule as ordinary /v1 routing. Both hooks phases and every
            # optimize export subcommand share one dispatch change, so cover more
            # than one of each. Their semantic response rendering is not under
            # test here; the remote request and local sentinel are the transport
            # assertions.
            # `optimize baseline` needs --point before it dispatches; the point
            # need not exist, since only the transport is under assertion.
            reachable = (
                ["hooks", "pre"],
                ["hooks", "post"],
                ["optimize", "points"],
                ["optimize", "baseline", "--point", "router"],
            )
            for command in reachable:
                before = RemoteHandler.count()
                special = run_client(binary, home, f"tcp:127.0.0.1:{port}", command)
                assert RemoteHandler.count() > before, (command, special.stderr)
                assert sentinel.contacts_after_settle() == 0, f"{command} contacted local UDS"

            # SessionStart checks workspace registration on the selected remote.
            # Outside registered workspaces it must not provision a session or
            # assemble memory context, and must never fall back to the local UDS.
            before = RemoteHandler.count()
            RemoteHandler.workspaces = []
            scoped_start = run_client(binary, home, f"tcp:127.0.0.1:{port}", ["session-start"])
            assert scoped_start.returncode == 0, (scoped_start.stdout, scoped_start.stderr)
            paths = RemoteHandler.paths_since(before)
            assert "/v1/workspaces" in paths, paths
            assert all(path in ("/v1/cli/manifest", "/v1/workspaces") for path in paths), paths
            assert sentinel.contacts_after_settle() == 0, "session-start contacted local UDS"

            RemoteHandler.workspaces = ["/tmp"]
            for command in reachable:
                failed = run_client(binary, home, f"tcp:127.0.0.1:{unused_tcp_port()}", command)
                # hooks intentionally fail open when the policy server is
                # unavailable; that is not a local fallback and is therefore a
                # valid result for this transport-only assertion.
                if command[0] == "optimize":
                    assert failed.returncode != 0, (
                        f"{command}: unreachable remote unexpectedly succeeded"
                    )
                assert sentinel.contacts_after_settle() == 0, (
                    f"{command}: remote failure fell back to local UDS"
                )
        finally:
            server.shutdown()
            server.server_close()
            server_thread.join(timeout=1)
            sentinel.close()

    native_main_check(binary)
    network_authority_check(binary)
    network_mcp_bootstrap_check(binary)
    if args.native_proxy_fixture:
        subprocess.run([str(args.native_proxy_fixture.resolve()), str(binary)],
                       check=True, timeout=30)
    print("thin-client-remote-exclusive: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
