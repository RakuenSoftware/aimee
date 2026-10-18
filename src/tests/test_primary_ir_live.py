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
            last_content = request.get("messages", [{}])[-1].get("content", "")
            if self.path.endswith("/messages") and isinstance(last_content, list) and any(
                    part.get("type") == "text" and "IR_RESPONSE_TOOLS" in part.get("text", "") for part in last_content):
                payload["content"].extend(dict(type="tool_use", id=f"call-{index}-" + "i" * 500,
                                               name="Read", input={"path": "p" * 8192, "index": index}) for index in range(17))
                payload["stop_reason"] = "tool_use"
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

    def api(path, body, headers=None, expected=200):
        connection = Local("localhost")
        try:
            connection.request("POST", path, json.dumps(body), {"Content-Type": "application/json", **(headers or {})})
            response = connection.getresponse()
            data = response.read().decode()
            assert response.status == expected, (path, response.status, data)
            if expected != 200:
                return data
            return data if path == "/v1/chat/stream" or body.get("stream") else json.loads(data)
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

        # A real Responses continuation: canonical signed thinking and every
        # tool id/argument survive the durable checkpoint and a native provider switch.
        headers = {"aimee-session-id": sid}
        first = api("/v1/responses", dict(model="ir-live-b", input="IR_RESPONSE_TOOLS",
                    instructions="RESPONSE_POLICY", reasoning={"effort": "high"},
                    text={"format": {"type": "json_schema", "name": "fixture", "strict": True,
                                     "schema": {"type": "object"}}},
                    tools=[{"type": "function", "name": "Read", "strict": True,
                            "parameters": {"type": "object", "properties": {"path": {"type": "string"}}}}]), headers)
        calls = [item for item in first["output"] if item["type"] == "function_call"]
        assert len(calls) == 17 and all(len(item["call_id"]) > 500 for item in calls), first
        assert observed[-1]["output_config"]["format"]["schema"] == {"type": "object"}, observed[-1]
        assert observed[-1]["output_config"]["effort"] == "high", observed[-1]
        assert all(len(json.loads(item["arguments"])["path"]) == 8192 for item in calls), calls
        followup = dict(model="ir-live-b", previous_response_id=first["id"], stream=True,
                        instructions="RESPONSE_POLICY",
                        tools=[{"type": "function", "name": "Read", "strict": True,
                                "parameters": {"type": "object", "properties": {"path": {"type": "string"}}}}],
                        input=[dict(type="function_call_output", call_id=item["call_id"], output=f"RESULT_{index}")
                               for index, item in enumerate(calls)])
        raw = api("/v1/responses", followup, headers)
        completed = [json.loads(line[6:])["response"] for line in raw.splitlines()
                     if line.startswith("data: ") and json.loads(line[6:]).get("type") == "response.completed"]
        assert len(completed) == 1 and completed[0]["status"] == "completed", raw
        sent = json.dumps(observed[-1])
        assert "IR_RESPONSE_TOOLS" in sent and "fixture-signature" in sent, observed[-1]
        for index, item in enumerate(calls):
            assert item["call_id"] in sent and f"RESULT_{index}" in sent, observed[-1]
        # Responses does not carry prior instructions/catalog implicitly. An
        # actual prefix edit omits incompatible native thinking at egress while
        # the durable checkpoint remains available for unchanged-prefix replay.
        changed = {key: value for key, value in followup.items() if key not in ("instructions", "tools")}
        api("/v1/responses", {**changed, "stream": False}, headers)
        assert "fixture-signature" not in json.dumps(observed[-1]), observed[-1]
        api("/v1/responses", {**followup, "stream": False}, headers)
        assert "fixture-signature" in json.dumps(observed[-1]), observed[-1]
        count = len(observed)
        api("/v1/responses", {**followup, "stream": False}, {"aimee-session-id": "another-scope"}, expected=400)
        assert len(observed) == count, "Cross-scope continuation reached the provider"
        print("canonical Responses: 17 long-ID/large-argument tools, native signed replay, durable continuation and cross-scope refusal passed")
        api("/v1/messages", dict(model="ir-live-b", max_tokens=64,
            messages=[dict(role="user", content="IR_NATIVE_ALIAS")]))
        assert observed[-1]["model"] == "ir-model-b", observed[-1]
        print("native Anthropic registered alias resolves to its provider model ID")

        for streaming in (False, True):
            request = dict(model="ir-live-a", stream=streaming, preserve_messages=True,
                messages=[dict(role="system", content="IR_SYSTEM_BOUNDARY"),
                          dict(role="user", content="IR_FIRST_USER"),
                          dict(role="assistant", content="IR_PREVIOUS_ASSISTANT"),
                          dict(role="user", content=[dict(type="text", text="IR_LAST_USER"),
                              dict(type="image_url", image_url=dict(url="https://example.invalid/image.png"))])])
            reply = api("/v1/chat/completions", request)
            sent = observed[-1]
            assert [message["role"] for message in sent["messages"]] == ["system", "user", "assistant", "user"], sent
            assert sent["model"] == "ir-model-a" and "IR_FIRST_USER" in json.dumps(sent["messages"][1]), sent
            assert "IR_PREVIOUS_ASSISTANT" in json.dumps(sent["messages"][2]), sent
            assert "image.png" in json.dumps(sent["messages"][3]), sent
            assert "PRIMARY_OK" in (reply if isinstance(reply, str) else json.dumps(reply)), reply
            reply = api("/v1/completions", dict(model="ir-live-a", prompt="IR_TEXT_PROMPT", max_tokens=77,
                temperature=0.2, top_p=0.9, stop=["IR_STOP"], stream=streaming))
            sent = observed[-1]
            assert sent["model"] == "ir-model-a" and "IR_TEXT_PROMPT" in json.dumps(sent["messages"]), sent
            assert sent["max_tokens"] == 77 and sent["temperature"] == 0.2 and sent["top_p"] == 0.9 and sent["stop"] in (["IR_STOP"], "IR_STOP"), sent
            assert "PRIMARY_OK" in (reply if isinstance(reply, str) else json.dumps(reply)), reply
        print("tool-free Chat retains roles/media; text completion enters directly as IR, buffered and streaming")


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
