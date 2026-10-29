#!/usr/bin/env python3
"""Delay runner registration and reject any Git dispatch before its ack."""
import http.server
import json
import os
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
        state = {"registered": False, "git_calls": 0, "early": False}

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
                    assert body["workspace_id"] == str(target)
                    time.sleep(0.15)
                    state["registered"] = served
                    self.reply(poll_response if poll_response is not None else
                               {"ok": True, "have_op": False, "served": served})
                elif body.get("tool") == "git_status":
                    state["git_calls"] += 1
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
        if served:
            assert result.returncode == 0 and state["git_calls"] == 1, result.stderr + result.stdout
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


def exercise_bootstrap(provider=None, register_status=200, poll_served=True,
                       valid_listing=True, is_repo=True, use_cwd=False, register_error=None,
                       registration_visible=True):
    """A real local repo needs no manual workspace add before its first Git call."""
    with tempfile.TemporaryDirectory(prefix="git-bootstrap-") as tmp:
        target = Path(tmp) / "checkout with spaces"
        target.mkdir()
        if is_repo:
            subprocess.run(["git", "init", "--quiet", str(target)], check=True)
        subdir = target / "nested"
        subdir.mkdir()
        state = {"events": [], "workspaces": [], "ready": False, "requests": []}
        if provider:
            state["workspaces"].append({"path": str(target), "provider": provider})

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def reply(self, value, status=200):
                data = json.dumps(value).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                state["requests"].append(self.path)
                if self.path == "/v1/workspaces":
                    self.reply({"workspaces": state["workspaces"]} if valid_listing else {})
                else:
                    self.reply({"manifest_version": 1, "commands": [], "tools": [],
                                "routes": [{"op": "mcp.call", "verb": "POST", "path": "/v1/mcp/call"}]})

            def do_POST(self):
                state["requests"].append(self.path)
                body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
                if self.path == "/v1/workspaces":
                    state["events"].append("register")
                    assert body["root"] == str(target), body
                    assert body["provider"] == "detached", body
                    if register_status == 200 and register_error is None:
                        if registration_visible:
                            state["workspaces"].append({"path": str(target), "provider": "detached"})
                        self.reply({"ok": True})
                    else:
                        self.reply(register_error if register_error is not None else
                                   {"error": "registration denied"}, register_status)
                elif self.path == "/v1/runner/poll":
                    assert body["workspace_id"] == str(target), body
                    state["ready"] = poll_served
                    state["events"].append("ready" if poll_served else "unserved")
                    self.reply({"ok": True, "have_op": False, "served": poll_served})
                elif body.get("tool") == "git_status":
                    state["events"].append("git")
                    if provider not in ("shared", "mirror"):
                        if not state["ready"]:
                            self.reply({"error": "requested git path is outside the session checkout"}, 403)
                            return
                    self.reply({"content": [{"type": "text", "text": "bootstrap verified"}]})
                else:
                    self.reply({"error": "unexpected request"}, 400)

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = os.environ.copy()
        env.update(AIMEE_HOME=str(Path(tmp) / "home"),
                   AIMEE_API_ENDPOINT=f"tcp:127.0.0.1:{server.server_port}",
                   AIMEE_API_BEARER="test-only-bearer")
        args = [str(CLI), "git", "status"]
        if not use_cwd:
            args.append(f"path={subdir}")
        try:
            result = subprocess.run(args, env=env, cwd=subdir if use_cwd else ROOT,
                                    capture_output=True, text=True, timeout=10)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
        events = state["events"]
        if provider in ("shared", "mirror"):
            assert result.returncode == 0 and events == ["git"], (events, result.stderr)
        elif not valid_listing:
            assert result.returncode != 0 and events == [], (events, result.stderr)
        elif not is_repo:
            assert "register" not in events, events
        elif register_status != 200 or register_error is not None or not registration_visible:
            assert result.returncode != 0 and events == ["register"], (events, result.stderr)
        elif not poll_served:
            assert result.returncode != 0 and "register" in events and "git" not in events, (events, result.stderr)
        else:
            assert result.returncode == 0 and "git" in events, (events, result.stdout, result.stderr)
            assert events.index("register") < events.index("ready") < events.index("git"), events
        assert not any("/index/" in path for path in state["requests"]), state["requests"]


exercise_bootstrap()
exercise_bootstrap(use_cwd=True)
exercise_bootstrap(provider="shared")
exercise_bootstrap(provider="mirror")
exercise_bootstrap(register_status=403)
exercise_bootstrap(register_status=500)
exercise_bootstrap(register_error={"error": "registration denied"})
exercise_bootstrap(register_error={"status": "error"})
exercise_bootstrap(register_error={"ok": False})
exercise_bootstrap(registration_visible=False)
exercise_bootstrap(poll_served=False)
exercise_bootstrap(valid_listing=False)
exercise_bootstrap(is_repo=False)
print("Fresh local Git checkouts register without indexing; configured providers and refusal boundaries are preserved.")
