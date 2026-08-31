#!/usr/bin/env python3
"""Prove a command with ARGUMENTS, unknown to this client, runs from served data alone.

The earlier proof (`aimee brandnew thing`) covered a command that takes no
arguments. This covers the harder half: a command whose flags become fields the
client has never heard of. The stub serves a route, a dispatch row and an
argument spec for a method that appears nowhere in the client binary, then
asserts the client POSTed the right path with the right BODY -- because a spec
that produces the wrong body would still "work" against a permissive server.
"""
import http.server
import json
import os
import subprocess
import sys
import tempfile
import threading

METHOD = "widget.tune"
SEEN = []

MANIFEST = {
    "manifest_version": 1,
    "routes": [{"op": METHOD, "verb": "POST", "path": "/v1/widget/tune"}],
    "commands": [{"name": "widget", "summary": "tune a widget", "subcommands": [{"name": "tune"}]}],
    "dispatch": [{"cmd": "widget", "sub": "tune", "method": METHOD}],
    "marshal": [{
        "method": METHOD,
        "args": {
            "bool_flags": ["dry-run"],
            "fields": [
                {"json": "widget_id", "from": "positional_or_flag", "index": 0,
                 "flag": "id", "required": True},
                {"json": "torque", "from": "flag", "flag": "torque", "type": "number"},
                {"json": "dry_run", "from": "flag", "flag": "dry-run", "type": "true_if_set"},
            ],
            "usage": "usage: aimee widget tune <id> [--torque N] [--dry-run]",
        },
    }],
}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, obj):
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/v1/cli/manifest"):
            self._send(MANIFEST)
        else:
            self._send({"status": "ok"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        try:
            body = json.loads(raw or b"{}")
        except ValueError:
            body = {"__unparseable__": raw.decode("utf-8", "replace")}
        SEEN.append((self.path, body))
        self._send({"status": "ok"})


def main():
    aimee = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "aimee")
    if not os.access(aimee, os.X_OK):
        print("missing prerequisite: ./aimee")
        return 1

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    port = srv.server_address[1]

    home = tempfile.mkdtemp(prefix="aimee-argspec-e2e-")
    env = dict(os.environ)
    env["HOME"] = home
    env["AIMEE_HOME"] = os.path.join(home, ".config", "aimee")
    env["AIMEE_API_ENDPOINT"] = f"tcp:127.0.0.1:{port}"
    env.pop("AIMEE_PROFILE", None)
    os.makedirs(env["AIMEE_HOME"], exist_ok=True)

    rc = subprocess.run(
        [aimee, "widget", "tune", "w-42", "--torque", "7", "--dry-run"],
        env=env, capture_output=True, text=True, timeout=90)

    posts = [(p, b) for p, b in SEEN if p == "/v1/widget/tune"]
    print(f"exit: {rc.returncode}")
    print(f"stderr: {rc.stderr.strip()[:300]}")
    print(f"server saw: {posts}")

    ok = rc.returncode == 0 and len(posts) == 1
    if ok:
        body = posts[0][1]
        expected = {"method": METHOD, "protocol_version": 1,
                    "widget_id": "w-42", "torque": 7, "dry_run": True}
        # Compare the fields the spec describes. A missing or renamed field here
        # is the failure this whole exercise is about: the request would still
        # be valid JSON and the server would still answer it.
        for k, v in expected.items():
            if body.get(k) != v:
                print(f"MISMATCH {k}: expected {v!r}, got {body.get(k)!r}")
                ok = False

    # And the refusal: with no id, the client must print the SPEC's usage line
    # and send nothing. A served command has to misuse as loudly as a compiled
    # one, or an operator cannot tell a typo from an outage.
    before = len(SEEN)
    rc2 = subprocess.run([aimee, "widget", "tune"], env=env,
                         capture_output=True, text=True, timeout=90)
    said_usage = "aimee widget tune <id>" in (rc2.stderr + rc2.stdout)
    sent_nothing = len(SEEN) == before
    print(f"missing-arg: exit={rc2.returncode} usage_shown={said_usage} sent_nothing={sent_nothing}")
    if not (said_usage and sent_nothing and rc2.returncode != 0):
        ok = False

    print(f"\nRESULT: a command with arguments, absent from this build, ran end to end: {ok}")
    srv.shutdown()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
