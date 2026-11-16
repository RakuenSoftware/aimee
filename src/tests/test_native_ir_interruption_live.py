"""Actual pinned SDK interruption and operator-denied continuation.

Called only by the required-containment owned-daemon crash harness. No source,
reservation, call, receipt, permission or grade row is manufactured by this test.
"""
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from test_native_ir_live import api
from test_native_ir_browser_control import ROOT_HTTP, assert_completed_parent



def sidecar_canonical_wire(value):
    """Match IR comparison decimals; this fixture's only numbers are integers.

    Comparison views encode nonzero integers as coefficient/exponent (7e0),
    unlike ordinary json.dumps. Original tool arguments remain untouched.
    """
    if value is None:
        return "null"
    if type(value) is bool:
        return "true" if value else "false"
    if type(value) is int:
        if not value:
            return "0"
        digits = str(value)
        coefficient = digits.rstrip("0")
        return coefficient + "e" + str(len(digits) - len(coefficient))
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    if isinstance(value, list):
        return "[" + ",".join(sidecar_canonical_wire(item) for item in value) + "]"
    if isinstance(value, dict):
        return "{" + ",".join(sidecar_canonical_wire(key) + ":" + sidecar_canonical_wire(value[key])
                              for key in sorted(value)) + "}"
    raise AssertionError("unexpected non-integer number in interruption fixture")


def run_interruption(home, workspace, probe, remember_boundary):
    # Import lazily: the crash fixture calls us after defining its kernel helpers.
    from test_native_ir_crash_live import (
        lifecycle, freeze_boundary, boundary_values, populated, leaf_path, assert_recovered,
    )
    assert os.environ.get("AIMEE_TEST_NATIVE_REQUIRE_CONTAINMENT") == "1"
    prefix = [] if os.geteuid() == 0 else ["sudo", "-n"]
    user = "ir-native-interruption"
    actor = "webuser:" + user
    api("/v1/vault/capability", dict(action="grant", principal=actor))
    active = {}
    errors = []
    processes = []
    models_path = home / "models.json"
    previous = models_path.read_bytes()

    def operator(path, body=None, receive=None):
        program = ROOT_HTTP
        if "/events?" in path:
            # Stop at the transport readiness boundary rather than waiting for
            # the intentional long-lived subscriber connection to end.
            program = program.replace("    sys.stdout.flush()\nconnection.close()",
                '    sys.stdout.flush()\n    if line.startswith(b"event: replay_complete"):\n'
                '        sys.stdout.write(response.readline().decode())\n'
                '        sys.stdout.flush()\n        break\nconnection.close()')
        process = subprocess.Popen(prefix + [sys.executable, "-u", "-c", program,
            str(home / "aimee-http.sock"), user], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        processes.append(process)
        process.stdin.write(json.dumps(dict(path=path, body=body)))
        process.stdin.close()
        first = process.stdout.readline()
        assert first, process.stderr.read()
        status = json.loads(first)["status"]
        lines = []
        for line in process.stdout:
            lines.append(line)
            if receive:
                receive(line)
        rc = process.wait(timeout=100)
        text = "".join(lines)
        assert not rc and status == 200, (path, status, rc, text, process.stderr.read())
        return text if receive else json.loads(text)

    def read(sid, kind):
        result = subprocess.run([str(probe), sid, actor, kind], capture_output=True,
                                text=True, timeout=20)
        assert not result.returncode, result.stderr
        return json.loads(result.stdout)

    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            try:
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                assert request["model"] == active["marker"], request
                assert self.headers.get("Authorization") == "Bearer native-interruption-only"
                if self.path.endswith("/count_tokens"):
                    body = json.dumps(dict(input_tokens=100)).encode()
                    content_type = "application/json"
                else:
                    assert self.path.endswith("/chat/completions"), self.path
                    active["requests"].append(request)
                    count = len(active["requests"])
                    encoded = json.dumps(request)
                    if active["phase"] == "question":
                        assert count == 1, ("unanswered real question unexpectedly resumed", request)
                        name, arguments, call = "request_input", {"question": "INTERRUPTION_PENDING_QUESTION"}, "interrupt_original_question"
                    elif count <= 2:
                        assert "INTERRUPTION_FRESH" in encoded, encoded[-8000:]
                        assert "effects are unknown" in encoded, encoded[-8000:]
                        if count == 2:
                            assert "explicit permission required" in encoded.lower(), encoded[-8000:]
                        name, arguments, call = "write_file", {"path": active["forbidden"], "content": "MUST_NOT_BE_WRITTEN"}, "interrupt_denied_write_" + str(count)
                    else:
                        assert count == 3 and "explicit permission required" in encoded.lower(), encoded[-8000:]
                        name, arguments, call = None, None, None
                    logical = ("mcp__aimee__" if active["kind"] == "claude" else "") + name if name else None
                    names = {t.get("name", t.get("function", {}).get("name")) for t in request.get("tools", [])}
                    assert not logical or logical in names, (logical, names)
                    message = dict(role="assistant", content=None if logical else "INTERRUPTION_DENIED_WITHOUT_EFFECT")
                    delta = dict(content=message["content"])
                    if logical:
                        function = dict(name=logical, arguments=json.dumps(arguments))
                        message["tool_calls"] = [dict(id=call, type="function", function=function)]
                        delta = dict(tool_calls=[dict(index=0, **message["tool_calls"][0])])
                    finish = "tool_calls" if logical else "stop"
                    if request.get("stream"):
                        chunk = dict(id="interruption-response-" + str(count), object="chat.completion.chunk",
                            model=active["marker"], choices=[dict(index=0, delta=delta, finish_reason=None)])
                        terminal = {**chunk, "choices": [dict(index=0, delta={}, finish_reason=finish)],
                                    "usage": dict(prompt_tokens=100, completion_tokens=10)}
                        body = ("data: " + json.dumps(chunk) + "\n\n" + "data: " + json.dumps(terminal) + "\n\n").encode()
                        content_type = "text/event-stream"
                    else:
                        body = json.dumps(dict(id="interruption-response-" + str(count), object="chat.completion",
                            model=active["marker"], choices=[dict(index=0, message=message, finish_reason=finish)],
                            usage=dict(prompt_tokens=100, completion_tokens=10))).encode()
                        content_type = "application/json"
                self.send_response(200)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except Exception as error:
                errors.append(repr(error))
                self.send_error(500)

    provider = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    provider.daemon_threads = True
    provider_worker = threading.Thread(target=provider.serve_forever, daemon=True)
    provider_worker.start()
    try:
        for kind in ("codex", "claude"):
            marker = "native-interruption-" + kind
            active.update(kind=kind, marker=marker, phase="question", requests=[],
                          forbidden="NATIVE_INTERRUPTION_DENIED_" + kind)
            model = dict(name=marker, model=marker, provider="openai", auth_type="api_key",
                endpoint=f"http://127.0.0.1:{provider.server_port}/v1", backend="provider-cli",
                cli_kind=kind, cli_cmd=os.environ["AIMEE_TEST_NATIVE_" + kind.upper()],
                roles=["all"], enabled=True, tools_enabled=True, max_tokens=4096,
                timeout_ms=150000, cli_idle_timeout_ms=150000, context_window=131072,
                max_parallel=1, max_turns=5)
            models_path.write_text(json.dumps(dict(default_agent=marker, default_delegate=marker,
                                                   fallback_chain=[], models=[model])))
            api("/v1/vault/set_server", dict(agent=marker, cred="api_key", secret="native-interruption-only"))
            sid = operator("/v1/sessions/create", dict(client_type="native-interruption-integration"))["session_id"]
            attachment = operator(f"/v1/sessions/{sid}/attach", dict(surface="webchat", owner=actor))["attach_id"]
            operator(f"/v1/sessions/{sid}/primary", dict(agent=marker))
            records, stream_errors, controls = [], [], queue.Queue()

            def receive(line):
                if line.strip():
                    record = json.loads(line)
                    assert isinstance(record.get("event"), dict), record
                    records.append(record)
                    if record["event"]["kind"] == "control_request":
                        controls.put(record)

            def stream(message):
                try:
                    operator("/v1/chat/stream", dict(attach_id=attachment, aimee_session_id=sid,
                        message=message, cwd=str(workspace), ir_version=1), receive)
                except Exception as error:
                    stream_errors.append(repr(error))
                    controls.put(dict(error=repr(error)))

            original_worker = threading.Thread(target=stream, args=("INTERRUPTION_FIRST: ask the operator the pending question.",), daemon=True)
            original_worker.start()
            pending = controls.get(timeout=90)
            assert "error" not in pending and pending["event"].get("control_kind") == "question", (pending, errors)
            before = read(sid, kind)
            assert len(before["reservations"]) == 1, ("required containment fell back", before)
            old = before["reservations"][0]
            assert old["state"] == "active" and old["claim_id"], before
            protected = [e for e in before["events"] if e["turn_id"] == old["turn_id"] and
                         e["event"].get("native_owner", {}).get("owner_id") == old["owner_id"] and
                         any(b.get("type") == "tool_use" and b.get("tool_id") == "interrupt_original_question"
                             for b in e["event"].get("payload", {}).get("blocks", []))]
            # Buffered provider responses publish a complete protected message;
            # streamed responses may publish a protected partial prefix first.
            # Both must bind the original call before the host question runs.
            assert protected and all(e["event"]["kind"] == "message" and
                                     type(e["event"].get("partial", False)) is bool
                                     for e in protected), before
            source = protected[0]
            assert source["event"]["native_owner"] == dict(version=1, harness=kind,
                epoch=old["epoch"], owner_id=old["owner_id"], binding_sha256=old["binding_sha256"],
                containment_sha256=old["containment_sha256"]), (source, old)
            assert int(source["sequence"]) < int(pending["sequence"]), (source, pending)
            original_leaf = leaf_path(old)
            boundary = remember_boundary(old)
            assert boundary is not None
            freeze_boundary(boundary)
            lifecycle(home, "pause-owned-server")
            lifecycle(home, "kill-owned-server")
            assert populated(original_leaf) and boundary_values(boundary)["frozen"] == "1"
            original_worker.join(15)
            assert not original_worker.is_alive(), "original question stream did not disconnect"
            lifecycle(home, "restart-owned-server")
            active.update(phase="fresh", requests=[])
            records.clear()
            stream_errors.clear()
            while not controls.empty():
                controls.get_nowait()
            attachment = operator(f"/v1/sessions/{sid}/attach", dict(surface="webchat", owner=actor))["attach_id"]
            fresh_worker = threading.Thread(target=stream, args=("INTERRUPTION_FRESH: inspect history and request the specified new file write.",), daemon=True)
            fresh_worker.start()
            permission = controls.get(timeout=90)
            assert "error" not in permission, (permission, stream_errors, read(sid, kind))
            event = permission["event"]
            assert event["control_kind"] == "permission", permission
            payload = event["payload"]
            assert payload["tool"] == "write_file" and "Allow this requested action once?" in payload["description"], event
            action = payload["input"]
            assert action["session_id"] == sid and action["arguments"]["path"] == active["forbidden"], action
            summary = action["unknown_receipts"]
            assert set(summary) == {"sha256", "count", "sources", "displayed"}, summary
            assert summary["count"] == summary["displayed"] == len(summary["sources"]) == 1, summary
            assert len(summary["sha256"]) == 64 and all(c in "0123456789abcdef" for c in summary["sha256"]), summary
            operator("/v1/chat/control-reply", dict(session_id=sid, request_id=event["request_id"], reply=dict(decision="deny")))
            fresh_worker.join(95)
            assert not fresh_worker.is_alive() and not stream_errors and not errors, (stream_errors, errors, records[-10:])
            assert_completed_parent(records)
            assert len(active["requests"]) == 3 and controls.empty(), (active, list(controls.queue))
            assert sum(r["event"]["kind"] == "control_request" for r in records) == 1, records
            deadline = time.monotonic() + 10
            while True:
                after = read(sid, kind)
                if len(after["reservations"]) == 2 and any(r["state"] == "finished" for r in after["reservations"]):
                    break
                assert time.monotonic() < deadline, after
                time.sleep(0.05)
            recovered = assert_recovered(after, old, actor)
            receipts = [e for e in after["events"] if e["event"]["kind"] == "tool_interruption"]
            assert len(receipts) == 1, after
            receipt = receipts[0]["event"]["payload"]
            assert receipt["source"]["sequence"] == str(source["sequence"]), (source, receipt)
            assert receipt["source"]["tool_id"] == "interrupt_original_question", receipt
            assert receipt["terminal_sequence"] == str(recovered["terminal_sequence"]), receipt
            assert receipt["owner"]["owner_id"] == old["owner_id"] and receipt["outcome"] == "unknown", receipt
            assert not any(b.get("type") == "tool_result" and b.get("tool_id") == "interrupt_original_question"
                for e in after["events"] if e["turn_id"] == old["turn_id"]
                for b in e["event"].get("payload", {}).get("blocks", [])), after
            assert not original_leaf.exists() or not populated(original_leaf), original_leaf
            fresh_results = [b for e in after["events"] if e["turn_id"] != old["turn_id"]
                for b in e["event"].get("payload", {}).get("blocks", []) if b.get("type") == "tool_result"]
            assert len(fresh_results) == 2 and all(b.get("is_error") is True for b in fresh_results), after
            assert all("explicit permission required" in json.dumps(b).lower() for b in fresh_results), fresh_results
            for reservation in after["reservations"]:
                remember_boundary(reservation)
            # Reconnect is a read-only canonical replay; it cannot emit a new
            # permission/question or repeat the interrupted tool's effect.
            replay = operator(f"/v1/sessions/{sid}/events?format=ir&cursor=0", receive=lambda _line: None)
            replayed = [json.loads(line[6:]) for line in replay.splitlines() if line.startswith("data: ")]
            assert replayed and replayed[-1] == dict(session_id=sid, cursor=str(after["events"][-1]["sequence"])), replay
            assert sum(r.get("event", {}).get("kind") == "tool_interruption" for r in replayed) == 1, replay
            assert sum(r.get("event", {}).get("kind") == "control_request" and r["event"].get("control_kind") == "permission" for r in replayed) == 1, replay
            original_source = [r for r in replayed if str(r.get("sequence")) == receipt["source"]["sequence"]]
            assert len(original_source) == 1 and original_source[0]["event_id"] == receipt["source"]["event_id"], (receipt, original_source)
            assert original_source[0]["event"]["native_owner"]["owner_id"] == receipt["owner"]["owner_id"], original_source
            receipt_record = next(r for r in replayed if r.get("event", {}).get("kind") == "tool_interruption")
            original_call = original_source[0]["event"]["payload"]["blocks"][receipt["source"]["block_index"]]
            complete_sidecar = [{"event_id": receipt_record["event_id"], "receipt": receipt, "call": original_call}]
            canonical_sidecar = sidecar_canonical_wire(complete_sidecar)
            assert summary["sha256"] == hashlib.sha256(canonical_sidecar.encode()).hexdigest(), (summary, complete_sidecar)
            expected_ref = {key: receipt["source"][key] for key in ("session_id", "turn_id", "sequence", "tool_id", "name")}
            expected_ref["event_id"] = receipt_record["event_id"]
            assert summary["sources"] == [expected_ref], (summary, expected_ref)
            assert len(active["requests"]) == 3 and read(sid, kind)["events"] == after["events"]
            operator(f"/v1/sessions/{sid}/detach", dict(attach_id=attachment))
            print("native interruption recovery:", kind, "real protected question prefix survives daemon death; exact unknown receipt enters next model context; one denied mutation permission suppresses a second attempt without effects or replay prompts", flush=True)
    finally:
        provider.shutdown()
        provider.server_close()
        provider_worker.join()
        for process in processes:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
        models_path.write_bytes(previous)
