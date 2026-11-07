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
import time


def assert_completed_parent(records):
    """Cache checkpoints and child notices may follow the parent's terminal."""
    starts = [record for record in records if record["event"]["kind"] == "execution" and
              record["event"]["payload"]["status"] == "running"]
    assert starts, "canonical parent execution never started"
    parent = starts[0]["turn_id"]
    statuses = [record["event"]["payload"]["status"] for record in records
                if record["turn_id"] == parent and record["event"]["kind"] == "execution"]
    assert statuses == ["running", "completed"], (parent, statuses)


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
        assert_completed_parent(stream_records)
        assert any(record["event"]["kind"] == "control_response" and
                   record["event"]["request_id"] == pending["request_id"] for record in stream_records)
        assert marker + "_FINAL" in json.dumps(stream_records)
    finally:
        operator(f"/v1/sessions/{sid}/detach", {"attach_id": attachment})
    print(f"native integration: {marker} live browser question, scoped operator reply and durable completion pass", flush=True)


def run_browser_delegate_control(api, home, workspace, marker, release):
    """A real asynchronous child asks after its native parent has finished."""
    prefix = [] if os.geteuid() == 0 else ["sudo", "-n"]
    user, actor = "ir-browser-delegate", "webuser:ir-browser-delegate"
    requests, stream_errors, child_records = queue.Queue(), [], []
    processes = []

    def operator(path, body=None, receive=None, replay=False):
        program = ROOT_HTTP
        if replay:
            program = program.replace("    sys.stdout.flush()\nconnection.close()",
                '    sys.stdout.flush()\n    if line.startswith(b"event: replay_complete"):\n'
                '        sys.stdout.write(response.readline().decode())\n        break\nconnection.close()')
        elif "/events?" in path:
            program = program.replace("    sys.stdout.flush()\nconnection.close()",
                '    sys.stdout.flush()\n    if line.startswith(b"data: "):\n'
                '        event = json.loads(line[6:]).get("event", {})\n'
                '        if event.get("kind") == "execution" and event["payload"]["status"] in ("completed", "failed", "cancelled"):\n'
                '            break\nconnection.close()')
        process = subprocess.Popen(prefix + [sys.executable, "-u", "-c", program,
            str(Path(home) / "aimee-http.sock"), user], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        processes.append(process)
        process.stdin.write(json.dumps(dict(path=path, body=body)))
        process.stdin.close()
        first = process.stdout.readline()
        assert first, process.stderr.read()
        status = json.loads(first)["status"]
        lines = []
        try:
            for line in process.stdout:
                lines.append(line)
                if receive:
                    receive(line)
            rc = process.wait(timeout=100)
            text = "".join(lines)
            assert not rc and status == 200, (path, status, rc, text, process.stderr.read())
            return text if receive or replay else json.loads(text)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)

    api("/v1/vault/capability", dict(action="grant", principal=actor))
    sid = operator("/v1/sessions/create", dict(client_type="native-browser-child-control"))["session_id"]
    attachment = operator(f"/v1/sessions/{sid}/attach", dict(surface="webchat", owner=actor))["attach_id"]
    operator(f"/v1/sessions/{sid}/primary", dict(agent=marker))
    release.clear()
    try:
        raw = operator("/v1/chat/stream", dict(attach_id=attachment, aimee_session_id=sid,
            message=marker + "_FIRST", cwd=str(workspace), ir_version=1), receive=lambda _line: None)
        parent = [json.loads(line) for line in raw.splitlines() if line.strip()]
        assert_completed_parent(parent)
        # A child may publish its running reference just after the parent stream
        # closes. Read only its real durable parent log; never infer a child ID.
        deadline = time.monotonic() + 30
        while True:
            replay = operator(f"/v1/sessions/{sid}/events?format=ir&cursor=0", replay=True)
            parent_history = [json.loads(line[6:]) for line in replay.splitlines() if line.startswith("data: ")]
            children = [r["event"]["payload"] for r in parent_history if r.get("event", {}).get("kind") == "child_execution" and r["event"]["payload"]["status"] == "running"]
            if children:
                break
            assert time.monotonic() < deadline, parent_history
            time.sleep(0.05)
        assert len(children) == 1, children
        child = children[0]
        assert child["session_id"] != sid

        def receive(line):
            if line.startswith("data: "):
                record = json.loads(line[6:])
                if "event" not in record:
                    return
                assert record["session_id"] == child["session_id"] and record["actor"] == actor, record
                child_records.append(record)
                if record["event"]["kind"] == "control_request":
                    requests.put(record)

        def stream():
            try:
                operator(f"/v1/sessions/{child['session_id']}/events?format=ir&cursor=0", receive=receive)
            except Exception as error:
                stream_errors.append(repr(error))
                requests.put(dict(error=repr(error)))

        worker = threading.Thread(target=stream, daemon=True)
        worker.start()
        release.set()
        pending = requests.get(timeout=60)
        assert "error" not in pending and pending["event"]["control_kind"] == "question", pending
        assert "DELEGATE_BROWSER_QUESTION" in json.dumps(pending), pending
        reply = dict(session_id=child["session_id"], request_id=pending["event"]["request_id"],
                     reply=dict(decision="allow", answers={"answer": ["DELEGATE_BROWSER_REPLY"]}))
        api("/v1/chat/control-reply", reply, expected=403)
        # Correct actor with wrong durable destination must also refuse.
        refused = {**reply, "session_id": sid}
        command = prefix + [sys.executable, "-u", "-c", ROOT_HTTP, str(Path(home) / "aimee-http.sock"), user]
        result = subprocess.run(command, input=json.dumps(dict(path="/v1/chat/control-reply", body=refused)), capture_output=True, text=True, timeout=100)
        assert not result.returncode and json.loads(result.stdout.splitlines()[0])["status"] == 409, result.stdout
        operator("/v1/chat/control-reply", reply)
        worker.join(timeout=95)
        assert not worker.is_alive() and not stream_errors, stream_errors
        assert_completed_parent(child_records)
        assert any(r["event"]["kind"] == "control_response" and r["event"]["request_id"] == reply["request_id"] for r in child_records), child_records
        assert "CHILD_BROWSER_QUESTION_OK" in json.dumps(child_records), child_records
        replay = operator(f"/v1/sessions/{child['session_id']}/events?format=ir&cursor=0", replay=True)
        durable = [json.loads(line[6:]) for line in replay.splitlines() if line.startswith("data: ") and '"event"' in line]
        assert sum(r["event"]["kind"] == "control_request" for r in durable) == 1, durable
        assert sum(r["event"]["kind"] == "control_response" for r in durable) == 1, durable
        late = subprocess.run(command, input=json.dumps(dict(path="/v1/chat/control-reply", body=reply)), capture_output=True, text=True, timeout=100)
        assert not late.returncode and json.loads(late.stdout.splitlines()[0])["status"] == 409, late.stdout
        print(f"native integration: {marker} completed native parent, private child question, exact child operator reply, durable replay and late reply refusal pass", flush=True)
    finally:
        release.set()
        for process in processes:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
        operator(f"/v1/sessions/{sid}/detach", dict(attach_id=attachment))
