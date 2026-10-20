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
import shutil
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def main():
    home = Path(os.environ["AIMEE_HOME"])
    observed = []
    execution_case = execution_requests = 0
    extra_attachments = []
    run_started, run_release = threading.Event(), threading.Event()
    identity_path = home / "identity-read.txt"
    identity_path.write_text("IDENTITY_READ_CONTROL")

    class Provider(BaseHTTPRequestHandler):
        def do_POST(self):
            nonlocal execution_requests
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
            if self.path.endswith("/responses"):
                assert execution_case in (1, 2, 3)
                execution_requests += 1
                assert execution_requests <= 2
                output = [dict(type="message", role="assistant", content=[
                    dict(type="output_text", text="IDENTITY_EXECUTION_OK")])]
                if execution_requests == 1:
                    call = dict(type="function_call", call_id="identity-call",
                                name="unadvertised" if execution_case == 3 else "read_file",
                                arguments=json.dumps(dict(path=str(identity_path))))
                    if execution_case == 1:
                        call["namespace"] = "foreign"
                    output = [call]
                payload = dict(id="identity-response", object="response", status="completed",
                               model="ir-model-c", output=output)
            if request.get("model") == "ir-model-run":
                sent = json.dumps(request)
                if "IR_RUN_CANCEL" in sent:
                    run_started.set()
                    assert run_release.wait(15), "cancellation fixture was not released"
                if "IR_RUN_TYPED_TOOL" in sent:
                    results = [part for message in request["messages"]
                               if isinstance(message.get("content"), list)
                               for part in message["content"] if part.get("type") == "tool_result"]
                    if not results:
                        payload["content"] = [dict(type="tool_use", id="run-call-" + "q" * 400,
                            name="read_file", input=dict(path=str(identity_path)))]
                        payload["stop_reason"] = "tool_use"
                    else:
                        assert "IDENTITY_READ_CONTROL" in json.dumps(results), results
                        payload["content"] = [dict(type="text", text="RUN_TOOL_OK 雪")]
                if "IR_RUN_TRUNCATED" in sent:
                    payload["content"] = [dict(type="text", text="RUN_PARTIAL")]
                    payload["stop_reason"] = "max_tokens"
            if request.get("model") == "ir-model-a" and "IR_MIRROR_DELEGATE_READ" in json.dumps(request):
                results = [message for message in request.get("messages", []) if message.get("role") == "tool"]
                if not results:
                    payload["choices"][0]["message"] = dict(role="assistant", tool_calls=[dict(
                        id="mirror-delegate-read", type="function", function=dict(
                            name="read_file", arguments=json.dumps(dict(path="README.md"))))])
                    payload["choices"][0]["finish_reason"] = "tool_calls"
                else:
                    assert "canonical delegate fixture" in json.dumps(results), results
                    assert "CLIENT_CWD_MUST_NOT_BE_READ" not in json.dumps(results), results
                    payload["choices"][0]["message"]["content"] = "MIRROR_DELEGATE_OK"
            data = json.dumps(payload).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                assert run_release.is_set(), "unexpected provider disconnect"

        def log_message(self, *_args):
            pass

    class Local(http.client.HTTPConnection):
        def connect(self):
            self.sock = socket.socket(socket.AF_UNIX)
            self.sock.settimeout(60)
            self.sock.connect(str(home / "aimee-http.sock"))

    def api(path, body=None, headers=None, expected=200):
        connection = Local("localhost")
        try:
            connection.request("GET" if body is None else "POST", path, None if body is None else json.dumps(body), {"Content-Type": "application/json", **(headers or {})})
            response = connection.getresponse()
            data = response.read().decode()
            assert response.status == expected, (path, response.status, data)
            if expected != 200:
                return data
            return data if path == "/v1/chat/stream" or (body and body.get("stream")) or path.endswith("/events") else json.loads(data)
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
                  for name, model, wire in [("ir-live-a", "ir-model-a", "openai"), ("ir-live-b", "ir-model-b", "anthropic"),
                                           ("ir-live-c", "ir-model-c", "chatgpt"), ("ir-live-run", "ir-model-run", "anthropic")]]
        next(model for model in models if model["name"] == "ir-live-run")["context_window"] = 131072
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

        # Verify the actual standalone observer and its persisted source binding
        # before any later fixture task adds more terminal events to this session.
        verification = subprocess.run([os.environ.get("GO", "go"), "-C",
            str(Path(__file__).resolve().parents[2] / "server-go"), "test", "./modules/learning",
            "-run", "^TestExecutionObservationLiveCanonicalCapture$", "-count=1"],
            env={**os.environ, "AIMEE_LEARNING_LIVE_SESSION": sid,
                 "AIMEE_LEARNING_LIVE_ACTOR": f"uid:{os.getuid()}",
                 "AIMEE_LEARNING_LIVE_DROP_PROJECTION": "1"},
            capture_output=True, text=True, timeout=120)
        assert verification.returncode == 0, verification.stdout + verification.stderr
        print("standalone learning: four real canonical task outcomes persisted locally without KB or fabricated grades")

        reports = [record["event"]["payload"] for record in live if record["event"]["kind"] == "projection"]
        assert any(report == {"model": "ir-model-a", "boundary": "request", "omitted_reasoning_blocks": 1,
                              "omitted_opaque_state_blocks": 1} for report in reports), reports

        # Export exercises transport-attested ownership through the actual HTTP
        # dispatch hop, not ambient worker state or an interaction-event fixture.
        exported = api("/v1/trajectory/export", {"session_id": sid, "compress": False})
        artifact = exported["trajectory"]
        assert artifact["schema"] == "aimee.trajectory.v2" and artifact["session_id"] == sid, exported
        assert artifact["source"] == "canonical_session_log" and "outcome" not in artifact, exported
        assert "governed_actions" not in artifact and "steps" not in artifact, exported
        assert len(artifact["messages"]) == 8, artifact
        assert all("blocks" in message for message in artifact["messages"]), artifact
        assert "fixture-signature" in json.dumps(artifact), artifact
        for earlier in range(4):
            assert f"IR_TURN_{earlier}" in json.dumps(artifact), artifact
        print("canonical trajectory: real HTTP owner identity, complete typed primary history and native signature retained")

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
        # The first verification removed one derived row. Actual owner-checked
        # replay must restore it, and repeated/resumed replay must not duplicate.
        recovery = subprocess.run(verification.args,
            env={**os.environ, "AIMEE_LEARNING_LIVE_SESSION": sid,
                 "AIMEE_LEARNING_LIVE_ACTOR": f"uid:{os.getuid()}"},
            capture_output=True, text=True, timeout=120)
        assert recovery.returncode == 0, recovery.stdout + recovery.stderr
        print("standalone learning: canonical replay recovered a lost projection without duplicate outcomes")
        print(f"canonical primary: A/B/A/B, {len(observed)} native calls, {sequence} durable events, live/replay and resumed-cursor parity passed")

        # Repeated cache-eligible prompts still produce private canonical
        # executions. A flat role-cache hit must not bypass the durable owner.
        tasks = home / "ir-trajectory-tasks.jsonl"
        tasks.write_text("\n".join(json.dumps(dict(name=f"repeated-{i}", role="summarize",
            prompt="IR_TRAJECTORY_BATCH", success_check={"type": "contains", "value": "PRIMARY_OK"}))
            for i in range(2)) + "\n")
        before_batch = len(observed)
        batch = api("/v1/trajectory/batch", dict(session_id=sid, tasks_path=str(tasks),
            out_dir=str(home / "ir-trajectories"), toolset_dist="research", compress=False))
        assert batch["failures"] == 0 and len(batch["written"]) == 2, batch
        assert len(observed) == before_batch + 2, "A repeated prompt bypassed canonical execution"
        child_ids = {item["session_id"] for item in batch["written"]}
        assert len(child_ids) == 2 and sid not in child_ids, batch
        for item in batch["written"]:
            child = json.loads(Path(item["path"]).read_text())
            assert child["schema"] == "aimee.trajectory.v2" and child["session_id"] == item["session_id"], child
            assert "IR_TRAJECTORY_BATCH" in json.dumps(child) and "PRIMARY_OK" in json.dumps(child), child
            assert "IR_TURN_0" not in json.dumps(child), "Parent history leaked into child export"
        print("canonical batch: two real private executions, repeated-prompt cache bypass and source references passed")


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

        # Real local tool execution with the Go execution-policy owner attached.
        # The same leaf in a foreign namespace must never reach that executor.
        for execution_case in (1, 2, 3):
            execution_requests = 0
            identity_sid = api("/v1/sessions/create", {"client_type": "ir-identity"})["session_id"]
            identity_attach = api(f"/v1/sessions/{identity_sid}/attach", dict(
                surface="webchat", owner=f"uid:{os.getuid()}"))["attach_id"]
            extra_attachments.append((identity_sid, identity_attach))
            api(f"/v1/sessions/{identity_sid}/primary", {"agent": "ir-live-c"})
            raw = api("/v1/chat/stream", dict(attach_id=identity_attach, message="IR_IDENTITY_CALL",
                      aimee_session_id=identity_sid, cwd=str(home), ir_version=1))
            records = [json.loads(line) for line in raw.splitlines() if line.strip()]
            assert records[-1]["event"] == {"kind": "execution", "payload": {"status": "completed"}}, raw
            assert execution_requests == 2
            blocks = [block for record in records if record["event"]["kind"] == "message"
                      for block in record["event"]["payload"]["blocks"]]
            calls = [block for block in blocks if block["type"] == "tool_use"]
            results = [block for block in blocks if block["type"] == "tool_result"]
            assert len(calls) == len(results) == 1, records
            result = results[0]
            # Host event/message identity is separate from model tool identity.
            # The durable event and provider replay retain the exact native ID.
            assert result["tool_id"] == calls[0]["tool_id"], (calls, results)
            assert result["tool_id"] == "identity-call", result
            if execution_case == 2:
                assert result["is_error"] is False and "IDENTITY_READ_CONTROL" in json.dumps(result), result
            else:
                assert result["is_error"] is True and "not advertised" in json.dumps(result), result
                assert "IDENTITY_READ_CONTROL" not in json.dumps(result), result
            if execution_case == 1:
                assert calls[0]["namespace"] == "foreign", calls
            native_results = [item for item in observed[-1]["input"]
                              if item.get("type") == "function_call_output"]
            assert len(native_results) == 1 and native_results[0]["call_id"] == "identity-call", observed[-1]
            sent = json.dumps(observed[-1])
            assert ("IDENTITY_READ_CONTROL" in sent) == (execution_case == 2), observed[-1]
        execution_case = 0
        for missing in (False, True):
            tool_reply = api("/v1/tools/execute", dict(tool="read_file", cwd=str(home),
                session_id=identity_sid, arguments=json.dumps(dict(path=str(identity_path) + (".missing" if missing else "")))))
            assert tool_reply["is_error"] is missing, tool_reply
            assert ("IDENTITY_READ_CONTROL" in tool_reply["result"]) is not missing, tool_reply
        print("canonical dispatch: exact advertised read executes; foreign namespace and unknown identity return durable errors")

        # Runs use the requested registration even though the role's default
        # delegate is ir-live-a. Its full ordered input is canonical, not a text
        # transcript, and actual tools execute under the captured caller.
        def await_run(run_id, timeout=45):
            deadline = time.monotonic() + timeout
            while True:
                result = api(f"/v1/runs/{run_id}")
                if result["status"] in ("completed", "incomplete", "failed", "cancelled"):
                    return result
                assert time.monotonic() < deadline, result
                time.sleep(0.05)

        original = "IR_RUN_TYPED_TOOL before-" + "x" * 20000 + "-after雪"
        image = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jr1sAAAAASUVORK5CYII="
        before_run = len(observed)
        run = api("/v1/runs", dict(model="ir-live-run", instructions="RUN_SYSTEM_INSTRUCTIONS",
            input=["IR_RUN_LEGACY_STRING", dict(role="assistant", content="IR_RUN_PRIOR_ASSISTANT"),
                   dict(role="user", content=[dict(type="input_text", text=original),
                                             dict(type="input_image", image_url=image)])],
            temperature=0.25, max_tokens=1024))
        finished = await_run(run["id"])
        assert finished["status"] == "completed" and "RUN_TOOL_OK" in json.dumps(finished), finished
        calls = observed[before_run:]
        assert len(calls) == 2 and all(call["model"] == "ir-model-run" for call in calls), calls
        assert original in json.dumps(calls[0], ensure_ascii=False) and "IR_RUN_PRIOR_ASSISTANT" in json.dumps(calls[0]), calls[0]
        assert "IR_RUN_LEGACY_STRING" in json.dumps(calls[0]) and "RUN_SYSTEM_INSTRUCTIONS" in json.dumps(calls[0]), calls[0]
        assert '"type": "image"' in json.dumps(calls[0]), calls[0]
        assert calls[0]["max_tokens"] == 1024 and calls[0]["temperature"] == 0.25, calls[0]
        assert finished["usage"]["input_tokens"] == 20 and finished["usage"]["output_tokens"] == 4, finished
        source = finished["canonical_session_id"]
        saved = api("/v1/trajectory/export", dict(session_id=source, compress=False))["trajectory"]
        assert original in json.dumps(saved, ensure_ascii=False) and "q" * 400 in json.dumps(saved), saved
        assert "IDENTITY_READ_CONTROL" in json.dumps(saved) and '"type": "image"' in json.dumps(saved), saved
        events = api(f"/v1/runs/{run['id']}/events")
        assert "tool_call.started" in events and "message.completed" in events and "RUN_TOOL_OK" in events, events
        partial = api("/v1/runs", dict(model="ir-live-run", input="IR_RUN_TRUNCATED"))
        partial = await_run(partial["id"])
        assert partial["status"] == "incomplete" and partial["incomplete_details"]["reason"] == "max_output_tokens", partial
        cancelled = api("/v1/runs", dict(model="ir-live-run", input="IR_RUN_CANCEL"))
        try:
            assert run_started.wait(10), "run did not reach the real provider"
            api(f"/v1/runs/{cancelled['id']}/stop", {})
            assert await_run(cancelled["id"], 10)["status"] == "cancelled"
        finally:
            run_release.set()
        for invalid in (dict(input=""), dict(input=[]), dict(input="x", max_tokens=1.5),
                        dict(input="x", max_tokens=0), dict(input="x", temperature=3),
                        dict(input="x", max_tokens=4, max_output_tokens=8),
                        dict(input="x", tools=[dict(type="function", name="foreign", parameters={})])):
            api("/v1/runs", dict(model="ir-live-run", **invalid), expected=400)
        print("canonical runs: full roles/media, requested model, governed tool, exact usage, durable source, typed truncation and live cancellation passed")

        # Always exercise workspace/owner/worktree setup, including when Docker
        # is absent locally. Only real sandboxed completion can qualify readback.
        workspace = Path(os.environ["AIMEE_IR_DELEGATE_WORKSPACE"])
        assert (workspace / ".git").is_dir(), "registered integration checkout is missing"

        def learning_task(run):
            launched = api("/v1/delegate/run", dict(role="format", persona="engineer", via="ir-live-a", tools=False,
                prompt=f"IR_LEARNING_RECEIPT_{run} return PRIMARY_OK", session_id=sid,
                cwd=str(workspace), max_turns=2))
            assert launched.get("job_id", 0) > 0, launched
            deadline = time.monotonic() + 120
            while True:
                status = api("/v1/delegate/status", dict(job_id=launched["job_id"], full_result=True))
                if status.get("job_status") in ("done", "failed", "partial", "cancelled"):
                    return status
                assert time.monotonic() < deadline, status
                time.sleep(0.05)

        docker = shutil.which("docker")
        sandbox_ready = docker and subprocess.run([docker, "version"], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, timeout=15, check=False).returncode == 0
        if not sandbox_ready:
            before = len(observed)
            status = learning_task("sandbox-refusal")
            assert status["job_status"] == "failed" and "could not be given a sandboxed container" in status["result"], status
            assert len(observed) == before, "unavailable sandbox must not execute a model task"
            print("delegate setup: registered workspace and dedicated worktree reach mandatory sandbox refusal")
            print("SKIP: delegate learning completion/readback (real Docker sandbox unavailable)")
        else:
            # Completion must follow capture, not race its next-run prompt read.
            lesson = "Delegate succeeded in role 'format'"
            for run in range(2):
                before = len(observed)
                status = learning_task(run)
                assert status["job_status"] == "done" and "PRIMARY_OK" in status["result"], status
                assert len(observed) > before, status
                assert (lesson in json.dumps(observed[before])) == (run == 1), observed[before]
            print("delegate learning: terminal completion precedes durable learning readback into the next actual execution")

        from test_native_ir_browser_control import prepare_browser_workspace
        mirror = prepare_browser_workspace(api, home, workspace, "queued-delegate-client")
        before = len(observed)
        launched = api("/v1/delegate/run", dict(role="diagnose", persona="engineer", via="ir-live-a", tools=True,
            prompt="IR_MIRROR_DELEGATE_READ read README.md and report its committed source", session_id=sid,
            cwd=str(mirror), max_turns=3))
        assert launched.get("job_id", 0) > 0, launched
        deadline = time.monotonic() + 120
        while True:
            status = api("/v1/delegate/status", dict(job_id=launched["job_id"], full_result=True))
            if status.get("job_status") in ("done", "failed", "partial", "cancelled"):
                break
            assert time.monotonic() < deadline, status
            time.sleep(0.05)
        if sandbox_ready:
            assert status["job_status"] == "done" and "MIRROR_DELEGATE_OK" in status["result"], status
            assert len(observed) == before + 2, observed[before:]
            print("delegate mirror: approved committed reconstruction, dedicated sandbox and real read pass")
        else:
            assert status["job_status"] == "failed" and "could not be given a sandboxed container" in status["result"], status
            assert len(observed) == before, "missing sandbox must not reach the model"
            print("delegate mirror: registered reconstruction and dedicated child reach mandatory sandbox refusal")



    finally:
        run_release.set()
        cleanup_error = None
        for extra_sid, extra_attach in extra_attachments:
            try:
                api(f"/v1/sessions/{extra_sid}/detach", {"attach_id": extra_attach})
            except (OSError, AssertionError) as error:
                cleanup_error = error
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
