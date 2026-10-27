#!/usr/bin/env python3
"""Contract tests for the active embed-remote.py container client.

Local stub servers cover embedding requests, managed authentication and serving
identity without a model. The generic endpoint guard also applies to other scripts.
Run: python3 scripts/check-sidecar-clients.py
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent


def _serve(handler_cls) -> tuple[ThreadingHTTPServer, str]:
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler_cls)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    host, port = server.server_address
    return server, f"http://{host}:{port}"


def _run(script: str, env: dict[str, str], stdin: str) -> str:
    proc = subprocess.run(
        [sys.executable, str(SCRIPTS / script)],
        input=stdin,
        capture_output=True,
        text=True,
        env={**os.environ, **env},
        timeout=30,
    )
    if proc.returncode != 0:
        raise AssertionError(f"{script} exited {proc.returncode}: {proc.stderr[:300]}")
    return proc.stdout


def _run_args(script: str, env: dict[str, str], args: list[str]) -> str:
    """Run a sidecar with argv flags and no stdin (the probe form the kb execs)."""
    proc = subprocess.run(
        [sys.executable, str(SCRIPTS / script), *args],
        capture_output=True,
        text=True,
        env={**os.environ, **env},
        timeout=30,
    )
    if proc.returncode != 0:
        raise AssertionError(f"{script} {' '.join(args)} exited {proc.returncode}: "
                             f"{proc.stderr[:300]}")
    return proc.stdout


def _run_missing_managed_auth(script: str, endpoint_key: str, url: str, stdin: str) -> None:
    env = {
        **os.environ,
        endpoint_key: url,
        "SYNTHESIS_AUTH_REQUIRED": "1",
        "SYNTHESIS_API_KEY": "",
    }
    proc = subprocess.run(
        [sys.executable, str(SCRIPTS / script)],
        input=stdin,
        capture_output=True,
        text=True,
        env=env,
        timeout=30,
    )
    assert proc.returncode != 0, f"{script} accepted missing managed auth"
    assert "SYNTHESIS_API_KEY is empty" in proc.stderr, proc.stderr


def check_embed_remote() -> None:
    vector = [round(i * 0.001, 3) for i in range(384)]
    token = "kb-to-llm-test-token"

    class EmbedStub(BaseHTTPRequestHandler):
        def log_message(self, *a):  # quiet
            pass

        def do_POST(self):
            assert self.path.rstrip("/") == "/embed", self.path
            assert self.headers.get("authorization") == f"Bearer {token}"
            length = int(self.headers.get("content-length", "0") or "0")
            self.rfile.read(length)
            body = json.dumps(vector).encode()
            self.send_response(200)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server, url = _serve(EmbedStub)
    try:
        out = _run(
            "embed-remote.py",
            {
                "EMBEDDER_URL": url,
                "SYNTHESIS_API_KEY": token,
                "SYNTHESIS_AUTH_REQUIRED": "1",
            },
            "hello world",
        )
        parsed = json.loads(out)
        assert isinstance(parsed, list) and len(parsed) == 384, f"got {len(parsed)} dims"
        _run_missing_managed_auth("embed-remote.py", "EMBEDDER_URL", url, "hello")
    finally:
        server.shutdown()
    print("  embed-remote.py: ok")


def check_embed_remote_serving_id() -> None:
    """--serving-id is a load-bearing contract, not a convenience.

    The kb's vector-space guard cannot GET /health itself in the shipped container — it
    reaches the gateway through this script — so it execs `--serving-id` and treats the
    result as the identity to record against the corpus. Three behaviours matter, and
    each of them was a bug at some point:
      - a reported identity is printed verbatim (the guard compares it as a string);
      - an endpoint that reports NO identity exits 0 with empty output, so the guard
        stays a no-op instead of refusing on a value the endpoint never had;
      - it does not require status=ok, because the identity is registry data and must be
        answerable while the model is still loading.
    """
    state = {"status": "ok", "serving_id": "bekko-a25m/8721341054416418"}

    class HealthStub(BaseHTTPRequestHandler):
        def log_message(self, *a):  # quiet
            pass

        def do_GET(self):
            assert self.path.rstrip("/") == "/health", self.path
            payload = {k: v for k, v in state.items() if v is not None}
            body = json.dumps(payload).encode()
            self.send_response(200 if state["status"] == "ok" else 503)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server, url = _serve(HealthStub)
    try:
        env = {"EMBEDDER_URL": url}
        out = _run_args("embed-remote.py", env, ["--serving-id"])
        assert out.strip() == state["serving_id"], repr(out)

        # Still loading: the identity is registry data, so it must answer anyway.
        state["status"] = "loading"
        out = _run_args("embed-remote.py", env, ["--serving-id"])
        assert out.strip() == "bekko-a25m/8721341054416418", repr(out)

        # An endpoint predating the field: empty output, exit 0 -> guard inactive.
        state["status"] = "ok"
        state["serving_id"] = None
        out = _run_args("embed-remote.py", env, ["--serving-id"])
        assert out.strip() == "", repr(out)
    finally:
        server.shutdown()
    print("  embed-remote.py --serving-id: ok")


def check_no_baked_endpoint_defaults() -> None:
    """Reject baked-in remote defaults; endpoints must be configured or fail closed.

    Literal loopback remains valid because it addresses only this host.
    """
    host_literal = re.compile(
        r"""setdefault\(\s*["'](?:SYNTHESIS_ENDPOINT|LLM_BASE_URL)["']\s*,\s*["']([^"']+)["']"""
    )
    offenders = []
    self_name = Path(__file__).name
    for path in sorted(SCRIPTS.glob("*.py")):
        if path.name == self_name:
            continue  # this file quotes the banned pattern to describe it
        for lineno, line in enumerate(path.read_text().splitlines(), 1):
            m = host_literal.search(line)
            if not m:
                continue
            value = m.group(1)
            if "127.0.0.1" in value or "localhost" in value:
                continue
            offenders.append(f"{path.name}:{lineno}: baked endpoint default {value!r}")

    if offenders:
        for o in offenders:
            print(f"  FAIL {o}")
        raise AssertionError(
            "sidecar endpoints must come from the environment or fail closed; "
            "a baked address either breaks every deployment that does not own it "
            "or leaks user code to whoever does"
        )
    print("  no baked endpoint defaults: ok")


def main() -> int:
    print("sidecar-clients:")
    check_embed_remote()
    check_embed_remote_serving_id()
    check_no_baked_endpoint_defaults()
    print("sidecar-clients: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
