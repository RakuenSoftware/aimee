#!/usr/bin/env python3
"""Actual native SDKs against a disposable server, Vault and durable session log.

Invoked by test_integration.sh when both explicit registered SDK command paths are set.
Local synthetic providers exercise all model wires and Vault credential kinds;
no live subscription or KB is used.
"""
import base64
import http.client
import json
import os
from pathlib import Path
import socket
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Local(http.client.HTTPConnection):
    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(90)
        self.sock.connect(str(Path(os.environ["AIMEE_HOME"]) / "aimee-http.sock"))


def api(path, body=None, expected=200, limits=None):
    if limits is None:
        limits = {"schema_version": 1, "max_request_bytes": 262144}
    connection = Local("localhost")
    try:
        connection.request("GET" if body is None else "POST", path,
                           None if body is None else json.dumps(body),
                           {"Content-Type": "application/json",
                            **({"X-Aimee-Context-Limits": json.dumps(limits)} if limits is not None else {})})
        response = connection.getresponse()
        text = response.read().decode()
        assert response.status == expected, (path, response.status, text)
        return text if path == "/v1/chat/stream" or path.endswith("/events") else json.loads(text)
    finally:
        connection.close()


def main():
    home = Path(os.environ["AIMEE_HOME"])
    workspace = Path(os.environ["AIMEE_IR_DELEGATE_WORKSPACE"])
    from test_native_ir_browser_control import prepare_browser_workspace
    workspace = prepare_browser_workspace(api, home, workspace)
    binaries = {kind: str(Path(os.environ["AIMEE_TEST_NATIVE_" + kind.upper()]).resolve(strict=True))
                for kind in ("codex", "claude")}
    for kind, executable in binaries.items():
        with open(executable, "rb") as stream:
            native = stream.read(4) == b"\x7fELF"
        assert native or (kind == "codex" and executable.endswith("/bin/codex.js")), executable
    cases = [(kind, wire, "api_key") for kind, wire in
             [("codex", "chatgpt"), ("codex", "anthropic"), ("codex", "openai"),
              ("claude", "anthropic"), ("claude", "chatgpt"), ("claude", "openai")]]
    cases += [("codex", "chatgpt", "codex-oauth"), ("claude", "anthropic", "claude-oauth")]
    authorizations = {}
    documents = {}
    forbidden_credentials = {"native-refresh-only"}

    def case_name(kind, wire, auth):
        return "native-" + kind + "-" + wire + ("-oauth" if auth != "api_key" else "")
    active = None
    async_case = False
    browser_question = False
    observed = []
    errors = []
    count = 0

    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            nonlocal count
            try:
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                assert active is not None
                kind, wire, marker, call_id = active
                observed.append(request)
                (home / "native-provider-requests.json").write_text(json.dumps(observed, indent=2))
                if self.path.endswith("/count_tokens"):
                    self.send_payload({"input_tokens": 100})
                    return
                count += 1
                assert count <= 3, (active, count)
                encoded = json.dumps(request, ensure_ascii=False)
                assert marker + "_FIRST" in encoded, encoded[:2000]
                assert request["model"] == marker, request.get("model")
                assert "Continue the server-owned canonical turn." not in encoded, encoded[:2000]
                if async_case:
                    assert "NATIVE_RUN_PRIOR" in encoded and "NATIVE_RUN_END" in encoded
                    assert "RUN_INSTRUCTIONS" in encoded and "iVBORw0KGgo" in encoded
                    if wire != "chatgpt":
                        assert request["max_tokens"] == 128 and request["temperature"] == 0.25, request
                auth, token = authorizations[marker]
                assert all(secret not in encoded for secret in forbidden_credentials)
                if wire == "anthropic" and auth == "api_key":
                    assert self.headers.get("x-api-key") == token
                else:
                    assert self.headers.get("Authorization") == "Bearer " + token
                if auth == "codex-oauth":
                    assert self.headers.get("chatgpt-account-id") == "native-account"
                if auth == "claude-oauth":
                    assert self.headers.get("x-api-key") is None
                    assert "oauth" in self.headers.get("anthropic-beta", "")
                tool_name = "request_input" if browser_question else "read_file"
                logical = "mcp__aimee__" + tool_name if kind == "claude" else tool_name
                tools = request.get("tools", [])
                names = {tool.get("name", tool.get("function", {}).get("name")) for tool in tools}
                assert logical in names, (logical, names)
                if count > 1:
                    assert call_id in encoded and ("BROWSER_REPLY" if browser_question else "canonical delegate fixture") in encoded, json.dumps(request.get("input", request.get("messages", [])), ensure_ascii=False)[-4000:]
                if count == 3:
                    assert marker + "_FOLLOWUP" in encoded
                text = marker + ("_FINAL" if count == 2 else "_RESUMED")
                arguments = json.dumps({"question": "BROWSER_QUESTION"} if browser_question else {"path": "README.md"})
                if wire == "anthropic":
                    assert self.path.endswith("/messages"), self.path
                    content = [{"type": "tool_use", "id": call_id, "name": logical,
                                "input": json.loads(arguments)}] if count == 1 else [{"type": "text", "text": text}]
                    payload = {"id": "msg_" + marker + str(count), "type": "message", "role": "assistant",
                               "model": marker, "content": content,
                               "stop_reason": "tool_use" if count == 1 else "end_turn",
                               "stop_sequence": None, "usage": {"input_tokens": 100, "output_tokens": 10}}
                    frames = [("message_start", {"type": "message_start", "message": {
                        **payload, "content": [], "stop_reason": None}})]
                    block = dict(content[0])
                    if count == 1:
                        block["input"] = {}
                    else:
                        block["text"] = ""
                    frames += [("content_block_start", {"type": "content_block_start", "index": 0, "content_block": block}),
                               ("content_block_delta", {"type": "content_block_delta", "index": 0,
                                "delta": {"type": "input_json_delta", "partial_json": arguments} if count == 1 else
                                         {"type": "text_delta", "text": text}}),
                               ("content_block_stop", {"type": "content_block_stop", "index": 0}),
                               ("message_delta", {"type": "message_delta", "delta": {
                                   "stop_reason": payload["stop_reason"], "stop_sequence": None}, "usage": {"output_tokens": 10}}),
                               ("message_stop", {"type": "message_stop"})]
                elif wire == "chatgpt":
                    assert self.path.endswith("/responses"), self.path
                    item = {"type": "function_call", "id": "fc_" + marker, "call_id": call_id,
                            "name": logical, "arguments": arguments, "status": "completed"} if count == 1 else {
                            "type": "message", "id": "msg_" + marker + str(count), "role": "assistant",
                            "status": "completed", "content": [{"type": "output_text", "text": text, "annotations": []}]}
                    payload = {"id": "resp_" + marker + str(count), "object": "response", "status": "completed",
                               "model": marker, "output": [item], "usage": {"input_tokens": 100, "output_tokens": 10, "total_tokens": 110}}
                    frames = [("response.created", {"type": "response.created", "response": {**payload, "status": "in_progress", "output": []}}),
                              ("response.output_item.added", {"type": "response.output_item.added", "output_index": 0,
                               "item": {**item, "status": "in_progress", **({"arguments": ""} if count == 1 else {"content": []})}})]
                    frames += [("response.function_call_arguments.delta", {"type": "response.function_call_arguments.delta",
                               "output_index": 0, "item_id": item["id"], "delta": arguments})] if count == 1 else [
                              ("response.output_text.delta", {"type": "response.output_text.delta", "output_index": 0,
                               "content_index": 0, "item_id": item["id"], "delta": text})]
                    frames += [("response.output_item.done", {"type": "response.output_item.done", "output_index": 0, "item": item}),
                               ("response.completed", {"type": "response.completed", "response": payload})]
                else:
                    assert self.path.endswith("/chat/completions"), self.path
                    delta = {"tool_calls": [{"index": 0, "id": call_id, "type": "function",
                              "function": {"name": logical, "arguments": arguments}}]} if count == 1 else {"content": text}
                    payload = {"id": "chat_" + marker + str(count), "object": "chat.completion.chunk", "model": marker,
                               "choices": [{"index": 0, "delta": delta, "finish_reason": None}]}
                    frames = [(None, payload), (None, {**payload, "choices": [{"index": 0, "delta": {},
                        "finish_reason": "tool_calls" if count == 1 else "stop"}],
                        "usage": {"prompt_tokens": 100, "completion_tokens": 10}})]
                if request.get("stream"):
                    data = "".join(("event: " + name + "\n" if name else "") + "data: " + json.dumps(value) + "\n\n"
                                   for name, value in frames).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                elif wire != "openai":
                    self.send_payload(payload)
                else:
                    message = {"role": "assistant", "content": text if count != 1 else None}
                    if count == 1:
                        message["tool_calls"] = [{"id": call_id, "type": "function", "function": {"name": logical, "arguments": arguments}}]
                    self.send_payload({"id": "chat_" + marker + str(count), "object": "chat.completion", "model": marker,
                        "choices": [{"message": message, "finish_reason": "tool_calls" if count == 1 else "stop"}],
                        "usage": {"prompt_tokens": 100, "completion_tokens": 10}})
            except Exception as error:
                errors.append(repr(error))
                print("native provider failure:", active, count, repr(error), flush=True)
                traceback.print_exc()
                self.send_error(500)

        def send_payload(self, payload):
            data = json.dumps(payload).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    models_path = home / "models.json"
    previous = models_path.read_bytes() if models_path.exists() else None
    try:
        actor = "uid:" + str(os.getuid())
        api("/v1/vault/capability", {"action": "grant", "principal": actor})
        models = []
        for kind, wire, auth in cases:
            name = case_name(kind, wire, auth)
            models.append(dict(name=name, model=name, provider=wire, auth_type=auth,
                endpoint=f"http://127.0.0.1:{server.server_port}/v1", backend="provider-cli", cli_kind=kind,
                cli_cmd=binaries[kind], roles=["all"], enabled=True, tools_enabled=True,
                context_window=131072, max_tokens=4096, timeout_ms=15000, cli_idle_timeout_ms=15000,
                max_parallel=1, max_turns=4))
            token = "native-fixture-only"
            document = None
            if auth == "codex-oauth":
                payload = base64.urlsafe_b64encode(json.dumps({"exp": int(time.time()) + 86400}).encode()).decode().rstrip("=")
                token = "e30." + payload + ".fixture"
                document = {"tokens": {"access_token": token, "refresh_token": "native-refresh-only", "account_id": "native-account"}}
            elif auth == "claude-oauth":
                document = {"claudeAiOauth": {"accessToken": token, "refreshToken": "native-refresh-only",
                    "expiresAt": (int(time.time()) + 86400) * 1000, "scopes": ["user:inference", "user:profile"]}}
            authorizations[name] = (auth, token)
            documents[name] = document
            forbidden_credentials.add(token)
            api("/v1/vault/set_server", {"agent": name, "cred": "oauth" if document else "api_key",
                                        "secret": json.dumps(document) if document else token})
        models_path.write_text(json.dumps(dict(default_agent=models[0]["name"], default_delegate=models[0]["name"],
                                               fallback_chain=[], models=models)))
        for kind, wire, auth in cases:
            async_case = False
            marker = case_name(kind, wire, auth)
            active = (kind, wire, marker, "toolu_" + marker.replace("-", "_"))
            count = 0
            sid = api("/v1/sessions/create", {"client_type": "native-ir-integration"})["session_id"]
            attachment = api(f"/v1/sessions/{sid}/attach", {"surface": "webchat", "owner": actor})["attach_id"]
            api(f"/v1/sessions/{sid}/primary", {"agent": marker})
            try:
                for label in ("FIRST", "FOLLOWUP"):
                    if label == "FOLLOWUP":
                        if auth != "api_key":
                            document = documents[marker]
                            old_token = authorizations[marker][1]
                            token = old_token.rsplit(".", 1)[0] + ".rotated" if auth == "codex-oauth" else "native-fixture-rotated"
                            if auth == "codex-oauth":
                                document["tokens"]["access_token"] = token
                            else:
                                document["claudeAiOauth"]["accessToken"] = token
                            forbidden_credentials.add(token)
                            authorizations[marker] = (auth, token)
                            api("/v1/vault/set_server", {"agent": marker, "cred": "oauth", "secret": json.dumps(document)})
                        compact = api("/v1/chat/stream", {"attach_id": attachment,
                            "aimee_session_id": sid, "provider_session_id": "untrusted-sdk-handle",
                            "compact": True, "ir_version": 1})
                        checkpoint = [json.loads(line)["event"] for line in compact.splitlines()]
                        terminal = checkpoint[-1]
                        assert terminal == {"kind": "execution", "payload": {"status": "completed"}}, compact
                        assert all(event["kind"] in ("execution", "history") for event in checkpoint), compact
                        assert any(event["kind"] == "history" and event["payload"]["phase"] == "commit"
                                   for event in checkpoint), compact
                        assert count == 2, ("compaction launched a model", active, count)
                    raw = api("/v1/chat/stream", {"attach_id": attachment, "message": marker + "_" + label,
                              "aimee_session_id": sid, "cwd": str(workspace), "ir_version": 1})
                    records = [json.loads(line) for line in raw.splitlines() if line.strip()]
                    assert not errors, errors
                    assert records and records[-1]["event"] == {"kind": "execution", "payload": {"status": "completed"}}, (active, count, raw[-8000:])
                    expected = marker + ("_FINAL" if label == "FIRST" else "_RESUMED")
                    assert expected in raw, raw[-8000:]
                assert count == 3, (active, count)
                connection = Local("localhost")
                try:
                    connection.request("GET", f"/v1/sessions/{sid}/events?format=ir&cursor=0")
                    response = connection.getresponse()
                    assert response.status == 200
                    replayed = []
                    target = int(records[-1]["sequence"])
                    while not replayed or int(replayed[-1]["sequence"]) < target:
                        line = response.readline().decode()
                        assert line, (sid, target, replayed)
                        if line.startswith("data: "):
                            replayed.append(json.loads(line[6:]))
                    replay = json.dumps(replayed, ensure_ascii=False)
                finally:
                    connection.close()
                assert marker + "_FIRST" in replay and marker + "_FOLLOWUP" in replay
                assert "canonical delegate fixture" in replay and active[3] in replay
                assert "Continue the server-owned canonical turn." not in replay
                print("native integration:", kind, wire, auth, "original tool/result, canonical compaction, durable replay and resumed turn pass", flush=True)
            finally:
                api(f"/v1/sessions/{sid}/detach", {"attach_id": attachment})
            async_case = True
            count = 0
            api("/v1/runs", {"model": marker, "input": marker + "_FIRST"}, expected=400)
            image = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jr1sAAAAASUVORK5CYII="
            request = {"model": marker, "cwd": str(workspace), "instructions": "RUN_INSTRUCTIONS",
                       "input": [marker + "_FIRST", {"role": "assistant", "content": "NATIVE_RUN_PRIOR"},
                                 {"role": "user", "content": [
                                     {"type": "input_text", "text": "x" * 20000 + "NATIVE_RUN_END"},
                                     {"type": "input_image", "image_url": image}]}]}
            if wire != "chatgpt":
                request.update(max_tokens=128, temperature=0.25)
            run = api("/v1/runs", request)
            deadline = time.monotonic() + 75
            while True:
                finished = api(f"/v1/runs/{run['id']}")
                if finished["status"] in ("completed", "incomplete", "failed", "cancelled"):
                    break
                assert time.monotonic() < deadline, finished
                time.sleep(0.05)
            assert not errors, errors
            assert finished["status"] == "completed" and marker + "_FINAL" in json.dumps(finished), finished
            assert count == 2, (active, count)
            assert finished["usage"]["input_tokens"] == 200 and finished["usage"]["output_tokens"] == 20, finished
            saved = api("/v1/trajectory/export", {"session_id": finished["canonical_session_id"], "compress": False})["trajectory"]
            encoded = json.dumps(saved, ensure_ascii=False)
            assert "NATIVE_RUN_PRIOR" in encoded and "NATIVE_RUN_END" in encoded
            assert "canonical delegate fixture" in encoded and "iVBORw0KGgo" in encoded
            assert "Continue the server-owned canonical turn." not in encoded
            print("native integration:", kind, wire, auth, "typed seeded run, real tool, original response and durable source pass", flush=True)
        if os.environ.get("AIMEE_TEST_NATIVE_BROWSER_OPERATOR") == "1":
            from test_native_ir_browser_control import run_browser_control
            browser_question = True
            async_case = False
            for kind, wire in (("codex", "chatgpt"), ("claude", "anthropic")):
                marker = case_name(kind, wire, "api_key")
                active = (kind, wire, marker, "toolu_browser_" + kind)
                count = 0
                run_browser_control(api, home, workspace, marker)
                assert count == 2 and not errors, (active, count, errors)
            browser_question = False
        # Qualified SDKs must still stop at the actual host wire boundary when
        # the inherited cap admits no bytes. A live SDK is not a budget bypass.
        for kind, wire in (("codex", "chatgpt"), ("claude", "anthropic")):
            marker = case_name(kind, wire, "api_key")
            before = len(observed)
            run = api("/v1/runs", dict(model=marker, input="QUALIFIED_ZERO_BUDGET", cwd=str(workspace)),
                      limits=dict(schema_version=1, max_request_bytes=0))
            deadline = time.monotonic() + 45
            while True:
                finished = api(f"/v1/runs/{run['id']}")
                if finished["status"] in ("completed", "incomplete", "failed", "cancelled"):
                    break
                assert time.monotonic() < deadline, finished
                time.sleep(0.05)
            assert finished["status"] == "failed" and finished.get("canonical_session_id"), finished
            assert len(observed) == before, (kind, observed[before:])
            print("native execution fence:", kind, "qualified SDK retains inherited zero-byte refusal without provider send", flush=True)
        # The deployment fence must reach a real queued worker with the native
        # cwd contract satisfied, then refuse before any unobservable send.
        native_roster = models_path.read_bytes()
        try:
            marker = case_name("claude", "anthropic", "api_key")
            for backend, cli_kind in (("tmux-cli", "acp"), ("provider-cli", "acp")):
                roster = json.loads(native_roster)
                selected = next(model for model in roster["models"] if model["name"] == marker)
                selected.update(backend=backend, cli_kind=cli_kind, cli_cmd="/bin/false",
                                primary_only=False, is_server_hosted=True)
                models_path.write_text(json.dumps(roster))
                before = len(observed)
                run = api("/v1/runs", dict(model=marker, input="UNOBSERVABLE_BUDGET_FENCE", cwd=str(workspace)),
                          limits=dict(schema_version=1, max_request_bytes=0))
                assert run.get("id"), run
                deadline = time.monotonic() + 30
                while True:
                    finished = api(f"/v1/runs/{run['id']}")
                    if finished["status"] in ("completed", "incomplete", "failed", "cancelled"):
                        break
                    assert time.monotonic() < deadline, finished
                    time.sleep(0.05)
                assert finished["status"] == "failed" and "request_budget_unavailable" in json.dumps(finished), finished
                assert len(observed) == before, (backend, observed[before:])
                print("native execution fence:", backend, "owned cwd admits worker, budget refusal prevents provider send", flush=True)
        finally:
            models_path.write_bytes(native_roster)
    finally:
        if previous is None:
            models_path.unlink(missing_ok=True)
        else:
            models_path.write_bytes(previous)
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
