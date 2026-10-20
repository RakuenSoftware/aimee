"""Actual browser operator over the existing root-UDS assertion boundary.

Required by native CI. A non-root development machine needs passwordless sudo to
run this extra journey; ordinary native/Vault journeys require no elevation.
"""
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading


ROOT_HTTP = r'''
import http.client, json, socket, sys
request = json.load(sys.stdin)
connection = http.client.HTTPConnection("localhost", timeout=90)
connection.sock = socket.socket(socket.AF_UNIX)
connection.sock.settimeout(90)
connection.sock.connect(sys.argv[1])
body = request.get("body")
connection.request("GET" if body is None else "POST", request["path"],
    None if body is None else json.dumps(body),
    {"Content-Type": "application/json", "X-Aimee-Webuser": sys.argv[2],
     "X-Aimee-Context-Limits": json.dumps({"schema_version": 1, "max_request_bytes": 262144})})
response = connection.getresponse()
print(json.dumps({"status": response.status}), flush=True)
while True:
    line = response.readline()
    if not line:
        break
    sys.stdout.write(line.decode())
    sys.stdout.flush()
connection.close()
'''


def prepare_browser_workspace(api, home, source, client_name="native-browser-client"):
    """Register an explicit mirror, preserving the browser foreign-path guard."""
    client = Path(home).parent / client_name
    subprocess.run(["git", "clone", "--quiet", "--shared", str(source), str(client)], check=True)
    head = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    registered = api("/v1/workspaces", {"root": str(client), "provider": "mirror",
                                       "remote": str(source), "head": head})
    assert registered.get("status") == "ok", registered
    # The mirror must use the approved committed source, not open this foreign cwd.
    (client / "README.md").write_text("CLIENT_CWD_MUST_NOT_BE_READ\n")
    return client


def run_browser_control(api, home, workspace, marker):
    """Deliver a question on the live browser stream, then reply as its operator."""
    prefix = [] if os.geteuid() == 0 else ["sudo", "-n"]
    user = "ir-browser"
    actor = "webuser:" + user
    events = queue.Queue()
    stream_records = []
    stream_errors = []

    def operator(path, body=None, on_line=None):
        command = prefix + [sys.executable, "-u", "-c", ROOT_HTTP,
                            str(Path(home) / "aimee-http.sock"), user]
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        process.stdin.write(json.dumps({"path": path, "body": body}))
        process.stdin.close()
        first = process.stdout.readline()
        assert first, process.stderr.read()
        status = json.loads(first)["status"]
        lines = []
        for line in process.stdout:
            lines.append(line)
            if on_line:
                on_line(line)
        rc = process.wait(timeout=100)
        error = process.stderr.read()
        text = "".join(lines)
        assert rc == 0 and status == 200, (path, rc, status, text, error)
        return text if on_line else json.loads(text)

    api("/v1/vault/capability", {"action": "grant", "principal": actor})
    sid = operator("/v1/sessions/create", {"client_type": "native-browser-control"})["session_id"]
    attachment = operator(f"/v1/sessions/{sid}/attach", {"surface": "webchat", "owner": actor})["attach_id"]
    operator(f"/v1/sessions/{sid}/primary", {"agent": marker})

    def receive(line):
        if not line.strip():
            return
        record = json.loads(line)
        assert "event" in record, ("noncanonical browser stream frame", record)
        stream_records.append(record)
        if record["event"]["kind"] == "control_request":
            events.put(record["event"])

    def stream():
        try:
            operator("/v1/chat/stream", {"attach_id": attachment,
                "aimee_session_id": sid, "message": marker + "_FIRST",
                "cwd": str(workspace), "ir_version": 1}, receive)
            if not any(record["event"]["kind"] == "control_request" for record in stream_records):
                raise AssertionError(("browser stream ended before its question", stream_records[-5:]))
        except Exception as error:
            stream_errors.append(repr(error))
            events.put({"error": repr(error)})

    worker = threading.Thread(target=stream, daemon=True)
    worker.start()
    try:
        pending = events.get(timeout=45)
        assert "error" not in pending, pending
        assert pending["control_kind"] == "question" and "BROWSER_QUESTION" in json.dumps(pending), pending
        reply = {"session_id": sid, "request_id": pending["request_id"],
                 "reply": {"decision": "allow", "answers": {"answer": ["BROWSER_REPLY"]}}}
        # An ordinary authenticated UID connection cannot impersonate this operator.
        api("/v1/chat/control-reply", reply, expected=403)
        operator("/v1/chat/control-reply", reply)
        worker.join(timeout=95)
        assert not worker.is_alive() and not stream_errors, stream_errors
        assert stream_records[-1]["event"] == {"kind": "execution", "payload": {"status": "completed"}}, stream_records[-5:]
        assert any(record["event"]["kind"] == "control_response" and
                   record["event"]["request_id"] == pending["request_id"] for record in stream_records)
        assert marker + "_FINAL" in json.dumps(stream_records)
    finally:
        operator(f"/v1/sessions/{sid}/detach", {"attach_id": attachment})
    print(f"native integration: {marker} live browser question, scoped operator reply and durable completion pass", flush=True)
