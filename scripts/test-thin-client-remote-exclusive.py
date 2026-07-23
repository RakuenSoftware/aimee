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
    requests = 0

    def _respond(self) -> None:
        type(self).requests += 1
        length = int(self.headers.get("Content-Length", "0"))
        if length:
            self.rfile.read(length)
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
        if command == ["hooks", "pre"]
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
            assert RemoteHandler.requests >= 2, RemoteHandler.requests
            assert sentinel.contacts_after_settle() == 0, (
                "configured remote also contacted local UDS"
            )

            # Each independently special-cased path must obey the same exclusive
            # selection rule as ordinary /v1 routing. Their semantic response
            # rendering is not under test here; the remote request and local
            # sentinel are the transport assertions.
            for command in (["hooks", "pre"], ["session-start"], ["optimize", "points"]):
                before = RemoteHandler.requests
                special = run_client(binary, home, f"tcp:127.0.0.1:{port}", command)
                assert RemoteHandler.requests > before, (command, special.stderr)
                assert sentinel.contacts_after_settle() == 0, f"{command} contacted local UDS"

            for command in (["hooks", "pre"], ["session-start"], ["optimize", "points"]):
                failed = run_client(binary, home, f"tcp:127.0.0.1:{unused_tcp_port()}", command)
                # hooks intentionally fail open when the policy server is
                # unavailable; that is not a local fallback and is therefore a
                # valid result for this transport-only assertion.
                if command == ["optimize", "points"]:
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
