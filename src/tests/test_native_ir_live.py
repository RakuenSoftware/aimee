#!/usr/bin/env python3
"""Actual native SDKs against a disposable server, Vault and durable session log.

Invoked by test_integration.sh when both explicit registered SDK command paths are set.
Local synthetic providers exercise all model wires and Vault credential kinds;
no live subscription or KB is used.
"""
import base64
import http.client
import hashlib
import json
import os
from pathlib import Path
import socket
import shutil
import subprocess
import threading
import tempfile
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit


class NativeCatalogObservations:
    """Full observed declarations, separate from transport/user/auth data."""
    def __init__(self, directory=None):
        self.directory = Path(directory) if directory else None
        self.references = {}
        self.records = []
        self.lock = threading.Lock()

    def observe(self, kind, wire, scope, tools):
        with self.lock:
            return self._observe(kind, wire, scope, tools)

    def _observe(self, kind, wire, scope, tools):
        assert isinstance(tools, list), "native request has no tool array"
        encoded = json.dumps(tools, sort_keys=True, ensure_ascii=False, separators=(",", ":"))
        key = (kind, wire, scope)
        previous = self.references.setdefault(key, encoded)
        assert previous == encoded, ("native catalog changed across auth/resume", key)
        names = []
        for tool in tools:
            assert isinstance(tool, dict)
            definition = tool.get("function", tool)
            if tool.get("type") == "namespace":
                for child in tool.get("tools", []):
                    names.append(dict(namespace=tool.get("name"), name=child.get("name")))
            else:
                names.append(dict(namespace=None, name=definition.get("name")))
        record = dict(ordinal=len(self.records) + 1, kind=kind, wire=wire, scope=scope,
                      declarations=len(tools), names=names,
                      sha256=hashlib.sha256(encoded.encode()).hexdigest(), tools=json.loads(encoded))
        self.records.append(record)
        if self.directory:
            self.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
            target = self.directory / ("catalog-%04d.json" % record["ordinal"])
            target.write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n")
            target.chmod(0o600)
        return record

    def finish(self):
        if self.directory:
            self.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
            target = self.directory / "summary.json"
            target.write_text(json.dumps(dict(records=[{k:v for k,v in r.items() if k != "tools"}
                              for r in self.records], baseline_full_wire_available=False,
                              evidence="observed catalogs plus exact source-owned profile fixtures; not baseline wire equivalence"),
                              indent=2) + "\n")
            target.chmod(0o600)
        for key, encoded in self.references.items():
            print("native integration: observed catalog", *key, "declarations", len(json.loads(encoded)),
                  "sha256", hashlib.sha256(encoded.encode()).hexdigest(), flush=True)


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


# This probe is read-only. Qualification requires actual daemon-created source,
# reservation and cache-owner rows; an enabled option or successful fallback is
# insufficient. It never fabricates lifecycle or cache state.
READ_CONTAINMENT = r'''
package main
import("context";"encoding/json";"os";"time";"github.com/jackc/pgx/v5")
func main(){
 ctx,cancel:=context.WithTimeout(context.Background(),15*time.Second);defer cancel()
 conn,err:=pgx.Connect(ctx,os.Getenv("AIMEE_STORE_URL"));if err!=nil{panic("qualification PostgreSQL unavailable")};defer conn.Close(ctx)
 rows,err:=conn.Query(ctx,`SELECT jsonb_build_object('reservation',to_jsonb(r),
 'terminal',e.payload::jsonb,'terminal_actor',e.actor,'terminal_turn',e.turn_id,
 'running_count',(SELECT count(*) FROM session_ir_events x WHERE x.session_id=r.session_id AND x.turn_id=r.turn_id AND x.actor=r.actor AND x.payload::jsonb->>'kind'='execution' AND x.payload::jsonb->'payload'->>'status'='running'),
 'terminal_count',(SELECT count(*) FROM session_ir_events x WHERE x.session_id=r.session_id AND x.turn_id=r.turn_id AND x.payload::jsonb->>'kind'='execution' AND x.payload::jsonb->'payload'->>'status' IN ('completed','failed','cancelled')),
 'owner',to_jsonb(o),'cache_token',h.native_id)::text
 FROM session_native_reservations r JOIN server_sessions s ON s.id=r.session_id AND s.principal=$2
 LEFT JOIN session_ir_events e ON e.session_id=r.session_id AND e.sequence=r.terminal_sequence
 LEFT JOIN session_native_cache_owners o ON o.session_id=r.session_id AND o.owner_id=r.owner_id AND o.turn_id=r.turn_id
 LEFT JOIN session_harness_handles h ON h.session_id=o.session_id AND h.harness=o.cache_key
 WHERE r.session_id=$1 AND r.actor=$2 AND r.harness=$3 ORDER BY r.created_at,r.owner_id LIMIT 17`,os.Args[1],os.Args[2],os.Args[3]);if err!=nil{panic(err)};defer rows.Close()
 out:=[]json.RawMessage{};for rows.Next(){var raw string;if err=rows.Scan(&raw);err!=nil{panic(err)};out=append(out,json.RawMessage(raw))};if err=rows.Err();err!=nil{panic(err)}
 if err=json.NewEncoder(os.Stdout).Encode(out);err!=nil{panic(err)}
}
'''


def assert_containment_rows(rows, sid, actor, kind, turns):
    assert len(rows) == len(turns) == 2, ("required containment used fallback or leaked ownership", sid, rows)
    assert {row["reservation"]["turn_id"] for row in rows} == set(turns), rows
    for row in rows:
        reservation = row["reservation"]
        assert reservation["session_id"] == sid and reservation["actor"] == actor, row
        assert reservation["harness"] == kind and reservation["state"] == "finished", row
        assert reservation["terminal_sequence"] > 0 and row["terminal_actor"] == actor, row
        assert row["terminal_turn"] == reservation["turn_id"], row
        assert row["terminal"]["kind"] == "execution" and row["terminal"]["payload"]["status"] == "completed", row
        assert row["running_count"] == row["terminal_count"] == 1, row
        for field, length in (("binding_sha256", 64), ("containment_sha256", 64),
                              ("scope_sha256", 64), ("context_sha256", 64),
                              ("owner_id", 32), ("claim_id", 32)):
            value = reservation[field]
            assert len(value) == length and all(c in "0123456789abcdef" for c in value), (field, row)
        manifest_text = reservation["containment_json"]
        assert hashlib.sha256(manifest_text.encode()).hexdigest() == reservation["containment_sha256"], row
        manifest = json.loads(manifest_text)
        assert manifest["version"] == 1 and manifest["kind"] == "linux_cgroup_v2_namespace", row
        assert manifest["protocol"] == "user-mount-pid-cgroup-network-write-preexec-v3", row
        assert len(manifest["tcp_ports"]) == 1 and 0 < manifest["tcp_ports"][0] <= 65535, row
        token = "complete:" + reservation["claim_id"] + ":" + reservation["turn_id"]
        assert reservation["finished_from"] == token and not reservation["recovery_epoch"], row
    # A resumed turn may replace the first turn's cache owner row. The latest
    # actual owner must still agree with its durable reservation and cache CAS.
    latest = rows[-1]
    owner = latest["owner"]
    assert owner, latest
    for field in ("actor", "turn_id", "harness", "scope_sha256", "context_sha256",
                  "binding_sha256", "owner_id", "claim_id", "containment_sha256"):
        assert owner[field] == latest["reservation"][field], latest
    assert latest["cache_token"] == latest["reservation"]["finished_from"], latest


def build_containment_probe(directory):
    source = Path(directory) / "containment.go"
    source.write_text(READ_CONTAINMENT)
    probe = Path(directory) / "read-containment"
    repository = Path(__file__).resolve().parents[2]
    built = subprocess.run([os.environ.get("GO", "go"), "-C", str(repository / "server-go"),
                            "build", "-o", str(probe), str(source)],
                           capture_output=True, text=True, timeout=120)
    assert built.returncode == 0, built.stdout + built.stderr
    return probe


def main():
    home = Path(os.environ["AIMEE_HOME"])
    require_containment = os.environ.get("AIMEE_TEST_NATIVE_REQUIRE_CONTAINMENT") == "1"
    if require_containment:
        assert os.environ.get("AIMEE_NATIVE_CONTAINMENT_ROOT"), "required containment root unset"
    qualification_directory = tempfile.TemporaryDirectory(prefix="native-qualification-", dir=home) if require_containment else None
    qualification_probe = build_containment_probe(qualification_directory.name) if require_containment else None
    workspace = Path(os.environ["AIMEE_IR_DELEGATE_WORKSPACE"])
    from test_native_ir_browser_control import prepare_browser_workspace, assert_completed_parent
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
    browser_delegate_question = False
    refusal_case = False
    refusal_details = dict(type="refusal", category="future_fixture_category",
                           explanation="Synthetic provider decline for fidelity validation.")
    delegation_case = False
    delegation_released = threading.Event()
    child_requests = []
    observed = []
    errors = []
    count = 0
    artifact = os.environ.get("AIMEE_TEST_NATIVE_ARTIFACT_DIR")
    catalogs = NativeCatalogObservations(Path(artifact) / "native-catalogs" if artifact else None)

    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            nonlocal count
            try:
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                if request.get("model") == "native-delegate-child":
                    assert delegation_released.wait(60), "parent native owner did not finish"
                    child_requests.append(request)
                    catalogs.observe("http-child", "openai", "diagnose", request.get("tools"))
                    assert self.headers.get("Authorization") == "Bearer native-child-fixture-only"
                    names = {tool.get("name", tool.get("function", {}).get("name")) for tool in request.get("tools", [])}
                    assert "delegate" not in names and "delegate_status" not in names, names
                    results = [message for message in request.get("messages", []) if message.get("role") == "tool"]
                    message = dict(role="assistant", content="CHILD_MIRROR_OK")
                    if not results:
                        tool = "request_input" if browser_delegate_question else "read_file"
                        assert tool in names, (tool, names)
                        message = dict(role="assistant", tool_calls=[dict(id="native-child-read", type="function",
                            function=dict(name=tool, arguments=json.dumps(dict(question="DELEGATE_BROWSER_QUESTION") if browser_delegate_question else dict(path="README.md"))))])
                    else:
                        assert ("DELEGATE_BROWSER_REPLY" if browser_delegate_question else "canonical delegate fixture") in json.dumps(results), results
                        assert "CLIENT_CWD_MUST_NOT_BE_READ" not in json.dumps(results), results
                        if browser_delegate_question:
                            message["content"] = "CHILD_BROWSER_QUESTION_OK"
                    self.send_payload(dict(id="native-child-response", model="native-delegate-child",
                        choices=[dict(index=0, message=message, finish_reason="stop" if results else "tool_calls")],
                        usage=dict(prompt_tokens=100, completion_tokens=10)))
                    return
                assert active is not None
                kind, wire, marker, call_id = active
                observed.append(request)
                (home / "native-provider-requests.json").write_text(json.dumps(observed, indent=2))
                route = urlsplit(self.path)
                # The pinned Claude SDK selects Anthropic's beta surface. The
                # native gateway must preserve that exact marker only when the
                # host-selected upstream is Anthropic, including token counts.
                expected_query = "beta=true" if kind == "claude" and wire == "anthropic" else ""
                assert route.query == expected_query and not route.fragment, self.path
                if route.path.endswith("/count_tokens"):
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
                catalogs.observe(kind, wire, "run" if async_case else "primary", request.get("tools"))
                if wire == "anthropic" and auth == "api_key":
                    assert self.headers.get("x-api-key") == token
                else:
                    assert self.headers.get("Authorization") == "Bearer " + token
                if auth == "codex-oauth":
                    assert self.headers.get("chatgpt-account-id") == "native-account"
                if auth == "claude-oauth":
                    assert self.headers.get("x-api-key") is None
                    assert "oauth" in self.headers.get("anthropic-beta", "")
                if refusal_case:
                    assert kind == "claude" and wire == "anthropic" and count == 1
                    payload = dict(id="msg_native_refusal", type="message", role="assistant",
                                   model=marker, content=[], stop_reason="refusal",
                                   stop_details={**refusal_details,
                                       "fallback_credit_token": "DO_NOT_PERSIST_REFUSAL_CREDIT",
                                       "recommended_model": "DO_NOT_SELECT_THIS_MODEL"},
                                   usage=dict(input_tokens=100, output_tokens=0))
                    if request.get("stream"):
                        frames = [("message_start", dict(type="message_start", message={
                            **payload, "stop_reason": None, "stop_details": None})),
                            ("message_delta", dict(type="message_delta", delta=dict(
                                stop_reason="refusal", stop_details=payload["stop_details"]),
                                usage=dict(output_tokens=0))),
                            ("message_stop", dict(type="message_stop"))]
                        data = "".join("event: " + name + "\ndata: " + json.dumps(value) + "\n\n"
                                       for name, value in frames).encode()
                        self.send_response(200)
                        self.send_header("Content-Type", "text/event-stream")
                        self.send_header("Content-Length", str(len(data)))
                        self.end_headers()
                        self.wfile.write(data)
                    else:
                        self.send_payload(payload)
                    return
                tool_call = count == 1 or (delegation_case and count == 2)
                if delegation_case and count == 2:
                    call_id += "_status"
                tool_name = ("delegate_status" if count > 1 else "delegate") if delegation_case else "request_input" if browser_question else "read_file"
                logical = "mcp__aimee__" + tool_name if kind == "claude" else tool_name
                tools = request.get("tools", [])
                names = {tool.get("name", tool.get("function", {}).get("name")) for tool in tools}
                assert logical in names, (logical, names)
                if count > 1:
                    previous_call_id = active[3] + ("_status" if delegation_case and count == 3 else "")
                    assert previous_call_id in encoded and ("job_id" if delegation_case else "BROWSER_REPLY" if browser_question else "canonical delegate fixture") in encoded, json.dumps(request.get("input", request.get("messages", [])), ensure_ascii=False)[-4000:]
                if count == 3 and not delegation_case:
                    assert marker + "_FOLLOWUP" in encoded
                text = marker + ("_FINAL" if count == (3 if delegation_case else 2) else "_RESUMED")
                arguments = json.dumps(dict(role="diagnose", persona="engineer", via="native-delegate-child", tools=True,
                    prompt="Ask the browser operator a question and report its actual answer" if browser_delegate_question else "Read README.md and report the committed source") if delegation_case else
                    {"question": "BROWSER_QUESTION"} if browser_question else {"path": "README.md"})
                if delegation_case and count == 2:
                    jobs = set(job_ids(request))
                    assert len(jobs) == 1, "native status call needs the returned job"
                    arguments = json.dumps(dict(job_id=jobs.pop(), full_result=True))
                if wire == "anthropic":
                    assert route.path.endswith("/messages"), self.path
                    content = [{"type": "tool_use", "id": call_id, "name": logical,
                                "input": json.loads(arguments)}] if tool_call else [{"type": "text", "text": text}]
                    payload = {"id": "msg_" + marker + str(count), "type": "message", "role": "assistant",
                               "model": marker, "content": content,
                               "stop_reason": "tool_use" if tool_call else "end_turn",
                               "stop_sequence": None, "usage": {"input_tokens": 100, "output_tokens": 10}}
                    frames = [("message_start", {"type": "message_start", "message": {
                        **payload, "content": [], "stop_reason": None}})]
                    block = dict(content[0])
                    if tool_call:
                        block["input"] = {}
                    else:
                        block["text"] = ""
                    frames += [("content_block_start", {"type": "content_block_start", "index": 0, "content_block": block}),
                               ("content_block_delta", {"type": "content_block_delta", "index": 0,
                                "delta": {"type": "input_json_delta", "partial_json": arguments} if tool_call else
                                         {"type": "text_delta", "text": text}}),
                               ("content_block_stop", {"type": "content_block_stop", "index": 0}),
                               ("message_delta", {"type": "message_delta", "delta": {
                                   "stop_reason": payload["stop_reason"], "stop_sequence": None}, "usage": {"output_tokens": 10}}),
                               ("message_stop", {"type": "message_stop"})]
                elif wire == "chatgpt":
                    assert route.path.endswith("/responses"), self.path
                    item = {"type": "function_call", "id": "fc_" + marker, "call_id": call_id,
                            "name": logical, "arguments": arguments, "status": "completed"} if tool_call else {
                            "type": "message", "id": "msg_" + marker + str(count), "role": "assistant",
                            "status": "completed", "content": [{"type": "output_text", "text": text, "annotations": []}]}
                    payload = {"id": "resp_" + marker + str(count), "object": "response", "status": "completed",
                               "model": marker, "output": [item], "usage": {"input_tokens": 100, "output_tokens": 10, "total_tokens": 110}}
                    frames = [("response.created", {"type": "response.created", "response": {**payload, "status": "in_progress", "output": []}}),
                              ("response.output_item.added", {"type": "response.output_item.added", "output_index": 0,
                               "item": {**item, "status": "in_progress", **({"arguments": ""} if tool_call else {"content": []})}})]
                    frames += [("response.function_call_arguments.delta", {"type": "response.function_call_arguments.delta",
                               "output_index": 0, "item_id": item["id"], "delta": arguments})] if tool_call else [
                              ("response.output_text.delta", {"type": "response.output_text.delta", "output_index": 0,
                               "content_index": 0, "item_id": item["id"], "delta": text})]
                    frames += [("response.output_item.done", {"type": "response.output_item.done", "output_index": 0, "item": item}),
                               ("response.completed", {"type": "response.completed", "response": payload})]
                else:
                    assert route.path.endswith("/chat/completions"), self.path
                    delta = {"tool_calls": [{"index": 0, "id": call_id, "type": "function",
                              "function": {"name": logical, "arguments": arguments}}]} if tool_call else {"content": text}
                    payload = {"id": "chat_" + marker + str(count), "object": "chat.completion.chunk", "model": marker,
                               "choices": [{"index": 0, "delta": delta, "finish_reason": None}]}
                    frames = [(None, payload), (None, {**payload, "choices": [{"index": 0, "delta": {},
                        "finish_reason": "tool_calls" if tool_call else "stop"}],
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
                    message = {"role": "assistant", "content": text if not tool_call else None}
                    if tool_call:
                        message["tool_calls"] = [{"id": call_id, "type": "function", "function": {"name": logical, "arguments": arguments}}]
                    self.send_payload({"id": "chat_" + marker + str(count), "object": "chat.completion", "model": marker,
                        "choices": [{"message": message, "finish_reason": "tool_calls" if tool_call else "stop"}],
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
        models.append(dict(name="native-delegate-child", model="native-delegate-child", provider="openai",
            auth_type="api_key", endpoint=f"http://127.0.0.1:{server.server_port}/v1", backend="http",
            roles=["all"], enabled=True, tools_enabled=True, max_tokens=4096, timeout_ms=75000,
            context_window=131072, max_parallel=1, max_turns=3))
        forbidden_credentials.add("native-child-fixture-only")
        api("/v1/vault/set_server", dict(agent="native-delegate-child", cred="api_key", secret="native-child-fixture-only"))
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
                qualified_turns = []
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
                        compact_records = [json.loads(line) for line in compact.splitlines()]
                        assert_completed_parent(compact_records)
                        checkpoint = [record["event"] for record in compact_records]
                        assert all(event["kind"] in ("execution", "history") for event in checkpoint), compact
                        assert any(event["kind"] == "history" and event["payload"]["phase"] == "commit"
                                   for event in checkpoint), compact
                        assert count == 2, ("compaction launched a model", active, count)
                    raw = api("/v1/chat/stream", {"attach_id": attachment, "message": marker + "_" + label,
                              "aimee_session_id": sid, "cwd": str(workspace), "ir_version": 1})
                    records = [json.loads(line) for line in raw.splitlines() if line.strip()]
                    assert not errors, errors
                    assert_completed_parent(records)
                    actual_turns = {record["turn_id"] for record in records
                                    if record["event"]["kind"] == "execution"
                                    and record["event"]["payload"]["status"] == "running"}
                    assert len(actual_turns) == 1, records
                    qualified_turns.extend(actual_turns)
                    expected = marker + ("_FINAL" if label == "FIRST" else "_RESUMED")
                    assert expected in raw, raw[-8000:]
                assert count == 3, (active, count)
                if qualification_probe:
                    deadline = time.monotonic() + 10
                    while True:
                        checked = subprocess.run([str(qualification_probe), sid, actor, kind],
                                                 capture_output=True, text=True, timeout=20)
                        assert checked.returncode == 0, checked.stderr
                        ownership = json.loads(checked.stdout)
                        if len(ownership) == 2 and all(row["reservation"]["state"] == "finished" for row in ownership):
                            break
                        assert time.monotonic() < deadline, ("required containment fallback or unfinished owner", sid, ownership)
                        time.sleep(0.05)
                    assert_containment_rows(ownership, sid, actor, kind, qualified_turns)
                    print("native containment qualification:", kind, wire, auth,
                          "actual tool/model + resumed SDK turns, v3 descriptor/hash, terminal and cache custody pass", flush=True)
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
        # A refusal is a normal provider response, not an invented HTTP error.
        # Exercise the real SDK and durable owner without a live provider or retry.
        refusal_case = True
        async_case = False
        marker = case_name("claude", "anthropic", "api_key")
        active = ("claude", "anthropic", marker, "unused_refusal_tool")
        count = 0
        sid = api("/v1/sessions/create", dict(client_type="native-ir-refusal-integration"))["session_id"]
        sdk_root = home / "browser-claude" / sid
        sdk_before = {path: path.read_bytes() for path in sdk_root.rglob("*.jsonl")}
        attachment = api(f"/v1/sessions/{sid}/attach", dict(surface="webchat", owner=actor))["attach_id"]
        api(f"/v1/sessions/{sid}/primary", dict(agent=marker))
        try:
            raw = api("/v1/chat/stream", dict(attach_id=attachment, message=marker + "_FIRST",
                aimee_session_id=sid, cwd=str(workspace), ir_version=1))
            records = [json.loads(line) for line in raw.splitlines() if line.strip()]
            assert count == 1 and not errors, (count, errors)
            events = [record["event"] for record in records]
            executions = [event["payload"]["status"] for event in events if event["kind"] == "execution"]
            assert executions == ["running", "failed"], executions
            stops = [event["payload"] for event in events
                     if event["kind"] == "delta" and event["payload"]["type"] == "TURN_STOP"]
            expected = dict(category=refusal_details["category"], explanation=refusal_details["explanation"])
            assert len(stops) == 1 and stops[0]["stop_reason"] == "content_filter", stops
            assert stops[0]["raw_stop_reason"] == "refusal" and stops[0]["refusal"] == expected, stops
            assert not any(event["kind"] in ("native_checkpoint", "control_request", "control_response")
                           for event in events), events
            for event in events:
                if event["kind"] == "message" and event["payload"]["role"] == "assistant":
                    assert not event["payload"]["blocks"], "SDK diagnostic became model content"
                if event["kind"] == "delta" and event["role"] == "assistant":
                    assert event["payload"].get("kind") != "tool_use"
                    assert not event["payload"].get("text_delta"), "SDK diagnostic became model text"
            assert "DO_NOT_PERSIST_REFUSAL_CREDIT" not in raw and "DO_NOT_SELECT_THIS_MODEL" not in raw
            connection = Local("localhost")
            try:
                connection.request("GET", f"/v1/sessions/{sid}/events?format=ir&cursor=0")
                response = connection.getresponse()
                assert response.status == 200
                replayed = []
                target = int(records[-1]["sequence"])
                while not replayed or int(replayed[-1]["sequence"]) < target:
                    line = response.readline().decode()
                    assert line, (sid, target)
                    if line.startswith("data: "):
                        replayed.append(json.loads(line[6:]))
                assert replayed == records, "Refusal metadata changed between live delivery and durable replay"
            finally:
                connection.close()
            diagnostics = []
            for path in sdk_root.rglob("*.jsonl"):
                data = path.read_bytes()
                if data == sdk_before.get(path, b""):
                    continue
                assert b"DO_NOT_PERSIST_REFUSAL_CREDIT" not in data and b"DO_NOT_SELECT_THIS_MODEL" not in data
                for line in data.splitlines():
                    record = json.loads(line)
                    message = record.get("message")
                    if (record.get("type") == "assistant" and isinstance(message, dict)
                            and message.get("role") == "assistant" and message.get("stop_reason") == "refusal"):
                        diagnostics.append(message.get("stop_details"))
            assert diagnostics == [refusal_details], diagnostics
            print("native integration: Claude actual SDK refusal retains typed category/explanation, "
                  "durable replay and one-request/no-retry boundary pass", flush=True)
        finally:
            api(f"/v1/sessions/{sid}/detach", dict(attach_id=attachment))
            refusal_case = False

        # The model calls the real orchestration tool without supplying cwd.
        # The child must retain the approved mirror after its parent SDK closes.
        delegation_case = True
        async_case = False
        docker = shutil.which("docker")
        sandbox_ready = docker and subprocess.run([docker, "version"], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, timeout=15).returncode == 0
        def job_ids(value):
            if isinstance(value, dict):
                if isinstance(value.get("job_id"), int):
                    yield value["job_id"]
                for item in value.values():
                    yield from job_ids(item)
            elif isinstance(value, list):
                for item in value:
                    yield from job_ids(item)
            elif isinstance(value, str):
                try:
                    parsed = json.loads(value)
                except (ValueError, TypeError):
                    return
                if not isinstance(parsed, str):
                    yield from job_ids(parsed)
        for kind, wire in (("codex", "chatgpt"), ("claude", "anthropic")):
            marker = case_name(kind, wire, "api_key")
            active = (kind, wire, marker, "toolu_delegate_" + kind)
            count = 0
            before = len(observed)
            child_before = len(child_requests)
            delegation_released.clear()
            sid = api("/v1/sessions/create", dict(client_type="native-delegation-integration"))["session_id"]
            attachment = api(f"/v1/sessions/{sid}/attach", dict(surface="webchat", owner=actor))["attach_id"]
            api(f"/v1/sessions/{sid}/primary", dict(agent=marker))
            try:
                raw = api("/v1/chat/stream", dict(attach_id=attachment, message=marker + "_FIRST",
                    aimee_session_id=sid, cwd=str(workspace), ir_version=1))
                records = [json.loads(line) for line in raw.splitlines() if line.strip()]
                assert_completed_parent(records)
                assert count == 3 and not errors, (count, errors)
            finally:
                api(f"/v1/sessions/{sid}/detach", dict(attach_id=attachment))
            jobs = set(job_ids(observed[before:]))
            assert len(jobs) == 1, observed[before:]
            delegation_released.set()
            job = jobs.pop()
            deadline = time.monotonic() + 90
            while True:
                child = api("/v1/delegate/status", dict(job_id=job, full_result=True))
                if child.get("job_status") in ("done", "failed", "partial", "cancelled"):
                    break
                assert time.monotonic() < deadline, child
                time.sleep(0.05)
            assert not errors, errors
            if sandbox_ready:
                assert child["job_status"] == "done", child
                output = child["output"]
                assert output["role"] == "assistant", output
                text = "".join(block["text"] for block in output["blocks"] if block["type"] == "text")
                assert "CHILD_MIRROR_OK" in text, output
                # full_result exposes the actual canonical owner record, not a
                # response reconstructed from the compatibility job.result.
                history = child["history"]
                assert isinstance(history, list) and output in history, child
                source = child["canonical_source"]
                assert isinstance(source, dict), child
                events = child["events"]
                assert isinstance(events, list) and events, child
                owner, terminal = source["child"], source["terminal_source"]
                assert source["execution_status"] == "completed", source
                assert source["parent_source"]["session_id"] == sid and source["parent_source"]["actor"] == actor, source
                assert owner["role"] == child["role"] and owner["agent"] == child["agent_name"], source
                assert terminal["session_id"] == owner["session_id"] and terminal["turn_id"] == owner["turn_id"], source
                assert terminal["actor"] == actor and int(source["last_sequence"]) >= int(terminal["sequence"]), source
                completed = [event for event in events if event["event_id"] == terminal["event_id"]]
                assert len(completed) == 1, events
                assert completed[0]["turn_id"] == owner["turn_id"] and completed[0]["actor"] == actor, completed
                assert completed[0]["sequence"] == terminal["sequence"], completed
                assert completed[0]["payload"]["kind"] == "execution" and completed[0]["payload"]["payload"]["status"] == "completed", completed
                assert len(child_requests) == child_before + 2, child_requests[child_before:]
            else:
                # Host sandbox admission failed before a child provider call.
                # This is an operator diagnostic, not a model refusal/output.
                reason = child.get("diagnostic")
                assert child["job_status"] == "failed", child
                assert isinstance(reason, str) and "could not be given a sandboxed container" in reason, child
                assert child["api_call_count"] == 0 and child["cursor_turn"] == 0, child
                assert len(child_requests) == child_before, child_requests[child_before:]
            saved = api("/v1/trajectory/export", dict(session_id=sid, compress=False))["trajectory"]
            assert active[3] in json.dumps(saved) and active[3] + "_status" in json.dumps(saved) and "job_id" in json.dumps(saved), saved
            print("native integration:", kind, "original delegation, inherited mirror and queued child ownership pass", flush=True)
        delegation_case = False
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
            if sandbox_ready:
                from test_native_ir_browser_control import run_browser_delegate_control
                delegation_case = browser_delegate_question = True
                for kind, wire in (("codex", "chatgpt"), ("claude", "anthropic")):
                    marker = case_name(kind, wire, "api_key")
                    active = (kind, wire, marker, "toolu_browser_delegate_" + kind)
                    count = 0
                    child_before = len(child_requests)
                    run_browser_delegate_control(api, home, workspace, marker, delegation_released)
                    assert count == 3 and len(child_requests) == child_before + 2 and not errors, (count, child_requests[child_before:], errors)
                delegation_case = browser_delegate_question = False
            else:
                assert not os.environ.get("CI"), "CI delegate browser controls require actual Docker"
                print("native integration: SKIP private delegate browser controls require actual Docker", flush=True)
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
        catalogs.finish()
        delegation_released.set()
        server.shutdown()
        server.server_close()
        thread.join()
        if qualification_directory:
            qualification_directory.cleanup()


if __name__ == "__main__":
    main()
