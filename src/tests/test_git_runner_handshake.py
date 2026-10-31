#!/usr/bin/env python3
"""Delay runner registration and reject any Git dispatch before its ack.

Each CLI polls on a private channel ("runner:" + 32 hex) and names it on the
Git call (X-Aimee-Runner), so the server hands that CLI's ops to that CLI and
no other; a served CLI closes its channel when it is done."""
import http.server
import json
import os
import re
from pathlib import Path
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
CLI = Path(os.environ.get("AIMEE_TEST_CLI", ROOT / "aimee"))

def exercise(served, poll_response=None):
    with tempfile.TemporaryDirectory(prefix="git-runner-handshake-") as tmp:
        target = Path(tmp) / "checkout"
        target.mkdir()
        state = {"registered": False, "git_calls": 0, "early": False, "channels": set(),
                 "call_channel": None, "closed": False}

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def reply(self, value):
                data = json.dumps(value).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                if self.path == "/v1/workspaces":
                    self.reply({"workspaces": [{"path": str(target), "provider": "detached"}]})
                else:
                    self.reply({"manifest_version": 1, "commands": [], "tools": [],
                                "routes": [{"op": "mcp.call", "verb": "POST", "path": "/v1/mcp/call"}]})

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
                if self.path == "/v1/runner/poll":
                    channel = body["workspace_id"]
                    assert re.fullmatch(r"runner:[0-9a-f]{32}", channel), channel
                    state["channels"].add(channel)
                    if body.get("close"):
                        state["closed"] = True
                        self.reply({"ok": True, "closed": True})
                        return
                    time.sleep(0.15)
                    state["registered"] = served
                    self.reply(poll_response if poll_response is not None else
                               {"ok": True, "have_op": False, "served": served})
                elif body.get("tool") == "git_status":
                    state["git_calls"] += 1
                    state["call_channel"] = self.headers.get("X-Aimee-Runner")
                    state["early"] |= not state["registered"]
                    self.reply({"content": [{"type": "text", "text": "handshake verified"}]})
                else:
                    self.reply({"status": "error", "error": "unexpected request"})

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = os.environ.copy()
        env.update(AIMEE_HOME=tmp, AIMEE_API_ENDPOINT=f"tcp:127.0.0.1:{server.server_port}",
                   AIMEE_API_BEARER="test-only-bearer")
        try:
            result = subprocess.run([str(CLI), "git", "status", f"path={target}"],
                                    env=env, cwd=ROOT, capture_output=True, text=True, timeout=10)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
        assert not state["early"], "Git raced runner registration"
        assert len(state["channels"]) <= 1, f"one CLI polled on several channels: {state['channels']}"
        if served:
            assert result.returncode == 0 and state["git_calls"] == 1, result.stderr + result.stdout
            assert state["call_channel"] in state["channels"], "the Git call did not name its runner"
            assert state["closed"], "a served CLI left its runner channel open"
        else:
            assert result.returncode != 0 and state["git_calls"] == 0, result.stderr + result.stdout

exercise(True)
exercise(False)
exercise(False, {})
exercise(False, {"error": "runner unavailable"})
exercise(False, {"have_op": False, "served": "false"})
exercise(False, {"have_op": "false", "served": True})
exercise(True, {"have_op": False})  # Compatible legacy acknowledgment.
print("Git waits for runner registration; unserved workspaces fail before dispatch.")
