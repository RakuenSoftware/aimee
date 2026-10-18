#!/usr/bin/env python3
"""Real native primary switching and durable/live canonical replay parity.

Called by test_integration.sh against its disposable server and PostgreSQL store.
Provider fixtures use native HTTP shapes; no subscription credentials are needed.
"""
import http.client
import json
import os
from pathlib import Path
import socket
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def main():
    home = Path(os.environ["AIMEE_HOME"])
    observed = []

    class Provider(BaseHTTPRequestHandler):
        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            observed.append(request)
            payload = {
                "id": "fixture", "choices": [{"message": {"role": "assistant", "content": "PRIMARY_OK"}, "finish_reason": "stop"}],
                "usage": {"prompt_tokens": 10, "completion_tokens": 2},
            }
            if self.path.endswith("/messages"):
                payload = {
                    "id": "native-b", "model": "ir-model-b", "role": "assistant",
                    "content": [{"type": "thinking", "thinking": "native reasoning", "signature": "fixture-signature"},
                                {"type": "text", "text": "PRIMARY_OK"}],
                    "stop_reason": "end_turn", "usage": {"input_tokens": 10, "output_tokens": 2},
                }
            data = json.dumps(payload).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *_args):
            pass

    class Local(http.client.HTTPConnection):
        def connect(self):
            self.sock = socket.socket(socket.AF_UNIX)
            self.sock.settimeout(60)
            self.sock.connect(str(home / "aimee-http.sock"))

    def api(path, body):
        connection = Local("localhost")
        try:
            connection.request("POST", path, json.dumps(body), {"Content-Type": "application/json"})
            response = connection.getresponse()
            data = response.read().decode()
            assert response.status == 200, (path, response.status, data)
            return data if path == "/v1/chat/stream" else json.loads(data)
        finally:
            connection.close()

    provider = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    models_path = home / "models.json"
    previous = models_path.read_bytes() if models_path.exists() else None
    sid = attachment = None
    try:
        models = [dict(name=name, model=model, provider=wire, auth_type="none",
                       endpoint=f"http://127.0.0.1:{provider.server_port}/v1", roles=["all"],
                       enabled=True, tools_enabled=True, context_window=8192, max_tokens=4096,
                       timeout_ms=10000, max_parallel=1, max_turns=2)
                  for name, model, wire in [("ir-live-a", "ir-model-a", "openai"), ("ir-live-b", "ir-model-b", "anthropic")]]
        models_path.write_text(json.dumps(dict(default_agent="ir-live-a", default_delegate="ir-live-a", fallback_chain=[], models=models)))
        sid = api("/v1/sessions/create", {"client_type": "ir-integration"})["session_id"]
        attachment = api(f"/v1/sessions/{sid}/attach", {"surface": "webchat", "owner": f"uid:{os.getuid()}"})["attach_id"]
        live = []
        sequence = 0
        for index, name in enumerate(["ir-live-a", "ir-live-b", "ir-live-a", "ir-live-b"]):
            api(f"/v1/sessions/{sid}/primary", {"agent": name})
            raw = api("/v1/chat/stream", {"attach_id": attachment, "message": f"IR_TURN_{index} say PRIMARY_OK",
                                        "aimee_session_id": sid, "cwd": str(home), "ir_version": 1})
            records = [json.loads(line) for line in raw.splitlines() if line.strip()]
            assert records and all(record.get("version") == 1 for record in records), raw
            assert records[-1]["event"] == {"kind": "execution", "payload": {"status": "completed"}}, raw
            for record in records:
                assert int(record["sequence"]) == sequence + 1, record
                assert record["actor"] == f"uid:{os.getuid()}", record
                sequence += 1
            live.extend(records)
            assert len(observed) == index + 1, observed
            assert observed[-1]["model"] == ("ir-model-a" if name == "ir-live-a" else "ir-model-b"), observed[-1]
            for earlier in range(index + 1):
                assert f"IR_TURN_{earlier}" in json.dumps(observed[-1]), observed[-1]
            if index == 3:
                assert "fixture-signature" in json.dumps(observed[-1]), observed[-1]
            assert any(record["event"].get("payload", {}).get("raw_stop_reason") in ("stop", "end_turn") for record in records), records

        reports = [record["event"]["payload"] for record in live if record["event"]["kind"] == "projection"]
        assert any(report == {"model": "ir-model-a", "boundary": "request", "omitted_reasoning_blocks": 1,
                              "omitted_opaque_state_blocks": 1} for report in reports), reports

        # Replay through the authenticated HTTP subscriber, not direct SQL or
        # the in-memory projection. Both initial replay and a resumed cursor
        # must return exactly the envelopes delivered while the turns ran.
        for cursor in (0, int(live[len(live) // 2]["sequence"])):
            connection = Local("localhost")
            response = None
            try:
                connection.request("GET", f"/v1/sessions/{sid}/events?format=ir&cursor={cursor}")
                response = connection.getresponse()
                assert response.status == 200, response.status
                replay = []
                while not replay or int(replay[-1]["sequence"]) < sequence:
                    line = response.readline()
                    assert line, "Replay ended before the committed cursor"
                    if line.startswith(b"data: "):
                        replay.append(json.loads(line[6:]))
                assert replay == [event for event in live if int(event["sequence"]) > cursor], (cursor, replay)
            finally:
                if response:
                    response.close()
                connection.close()
        print(f"canonical primary: A/B/A/B, {len(observed)} native calls, {sequence} durable events, live/replay and resumed-cursor parity passed")
    finally:
        cleanup_error = None
        if sid and attachment:
            try:
                api(f"/v1/sessions/{sid}/detach", {"attach_id": attachment})
            except (OSError, AssertionError) as error:
                cleanup_error = error
        if previous is None:
            models_path.unlink(missing_ok=True)
        else:
            models_path.write_bytes(previous)
        provider.shutdown()
        provider.server_close()
        thread.join(timeout=5)
        if cleanup_error:
            raise cleanup_error


if __name__ == "__main__":
    main()
