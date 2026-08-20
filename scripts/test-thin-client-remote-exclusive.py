#!/usr/bin/env python3
"""Black-box regression test for exclusive thin-client transport selection."""

from __future__ import annotations

import argparse
import http.server
import json
import os
import pathlib
import socket
import subprocess
import tempfile
import threading


class RemoteHandler(http.server.BaseHTTPRequestHandler):
    # ThreadingHTTPServer serves each request on its own thread, so the request
    # counter is shared mutable state: `requests += 1` is a non-atomic
    # read-modify-write that can lose an increment under concurrency and make an
    # assertion flaky. Guard every access with a lock.
    requests = 0
    _lock = threading.Lock()

    @classmethod
    def count(cls) -> int:
        with cls._lock:
            return cls.requests

    def _respond(self) -> None:
        with type(self)._lock:
            type(self).requests += 1
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
                    ],
                }
            ).encode()
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
    for key in ("AIMEE_SERVER_URL", "AIMEE_SERVER_TOKEN", "AIMEE_TLS_INSECURE"):
        env.pop(key, None)
    return subprocess.run(
        [str(binary), "--json", *command],
        env=env,
        text=True,
        capture_output=True,
        input='{"tool_name":"Read","tool_input":{},"cwd":"/tmp"}\n'
        if command[:1] == ["hooks"]
        else "{}\n"
        if command == ["session-start"]
        else None,
        timeout=20,
        check=False,
    )


def unused_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=pathlib.Path)
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
                # session-start is deliberately NOT here. It no longer dispatches
                # to the server: the launcher owns session id, worktree and cwd
                # before the host starts, and session guidance is prepended at
                # model ingress instead of being assembled per client. What is
                # left is a local publish of the host session id, so "must reach
                # the remote" no longer describes it. The exclusivity half still
                # holds trivially -- it contacts nothing.
                ["optimize", "points"],
                ["optimize", "baseline", "--point", "router"],
            )
            for command in reachable:
                before = RemoteHandler.count()
                special = run_client(binary, home, f"tcp:127.0.0.1:{port}", command)
                assert RemoteHandler.count() > before, (command, special.stderr)
                assert sentinel.contacts_after_settle() == 0, f"{command} contacted local UDS"

            # session-start is now local-only, and that is a contract in its own
            # right: it must reach NEITHER the remote nor the local socket. A
            # regression that reintroduced per-client assembly here would show
            # up as a request to one of the two.
            before = RemoteHandler.count()
            local_only = run_client(binary, home, f"tcp:127.0.0.1:{port}", ["session-start"])
            assert local_only.returncode == 0, (local_only.stdout, local_only.stderr)
            assert RemoteHandler.count() == before, "session-start contacted the remote"
            assert sentinel.contacts_after_settle() == 0, "session-start contacted local UDS"

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

    print("thin-client-remote-exclusive: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
