#!/usr/bin/env python3
"""Opt-in real Codex SDK qualification through Aimee's compiled native adapter.

Run with --adapter-binary <unit-test-cli-codex> --codex-binary <codex>.
Uses only a loopback fixture provider and a fresh credential-free HOME. Synthetic
usage is protocol test data, never evidence of provider billing or cache savings.
"""
import argparse
import json
import os
import sys
from pathlib import Path
import subprocess
import tempfile
import threading
import tomllib
from http.server import BaseHTTPRequestHandler, HTTPServer


def mcp_fixture():
    """Local stdio MCP fixture, including a protocol-valid failed tool result."""
    for line in sys.stdin:
        request = json.loads(line)
        if "id" not in request:
            continue
        method = request["method"]
        if method == "initialize":
            result = {"protocolVersion": "2024-11-05", "capabilities": {"tools": {}},
                      "serverInfo": {"name": "native-fixture", "version": "1"}}
        elif method == "tools/list":
            result = {"tools": [{"name": "fail", "description": "Local failure fixture",
                "annotations": {"readOnlyHint": True, "destructiveHint": False, "openWorldHint": False},
                "inputSchema": {"type": "object", "properties": {
                    "value": {"type": "integer"}}, "required": ["value"],
                    "additionalProperties": False}}]}
        elif method == "tools/call":
            assert request["params"]["name"] == "fail"
            assert request["params"]["arguments"] == {"value": 7}
            result = {"content": [{"type": "text", "text": "fixture refused"}],
                      "isError": True, "structuredContent": {"fixture": "refused"}}
        else:
            result = {}
        print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)


def tool_names(tools, prefix=""):
    result = set()
    for tool in tools:
        name = tool.get("name", tool.get("type", "unknown"))
        if tool.get("type") == "namespace":
            result.update(tool_names(tool.get("tools", []), prefix + name + "."))
        else:
            result.add(prefix + name)
    return result


def qualify(adapter_binary, codex_binary, bounded_sdk_catalog=False, host_tools=False, owned_launch=False, flat_tools=False):
    namespace = None if flat_tools else "fixture" if host_tools else "mcp__fixture"
    native_arguments = ('{"value":7,"marker":0.123456789012345678901234567890}'
                        if host_tools else '{"value":7}')
    expected_arguments = json.loads(native_arguments)
    requests = []
    errors = []

    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            try:
                assert self.path == "/v1/responses", self.path
                assert not self.headers.get("Content-Encoding")
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                requests.append(body)
                if bounded_sdk_catalog:
                    # Inspect every real provider catalog, including restarted
                    # resume and fresh/tail adoption. An accepted config key
                    # alone does not establish its effect.
                    names = tool_names(body["tools"])
                    # SDK resource helpers route through configured MCP servers.
                    # This qualifies the catalog, not Aimee's still-unwired
                    # native resource ingress or its authorization/receipts.
                    allowed = {"mcp__fixture.fail", "request_user_input",
                               "functions.request_user_input", "list_mcp_resources",
                               "list_mcp_resource_templates", "read_mcp_resource"}
                    if host_tools:
                        allowed = {"fixture.fail", "request_user_input", "functions.request_user_input"}
                    if flat_tools:
                        allowed = {"fail", "request_user_input", "functions.request_user_input"}
                    expected_tool = "fail" if flat_tools else namespace + ".fail"
                    assert expected_tool in names and names <= allowed, sorted(names)
                number = len(requests)
                item = {
                    "id": f"msg_fixture_{number}", "type": "message", "role": "assistant",
                    "status": "completed", "content": [{"type": "output_text",
                    "text": f"fixture final {number}", "annotations": []}],
                }
                if number == 1:
                    tools = [tool for tool in body["tools"]
                             if (tool.get("name") == "fail" if flat_tools else "fixture" in tool.get("name", ""))]
                    assert len(tools) == 1, body["tools"]
                    item = {"id": "fc_native_fixture", "type": "function_call",
                            "call_id": "call_native_fixture", "name": "fail",
                            "arguments": native_arguments, "status": "completed"}
                    if not flat_tools:
                        item["namespace"] = tools[0]["name"]
                response = {
                    "id": f"resp_fixture_{number}", "object": "response", "status": "completed",
                    "model": "fixture-model", "output": [item],
                    "usage": {"input_tokens": 100, "output_tokens": 10, "total_tokens": 110},
                }
                frames = [
                    ("response.created", {"response": dict(response, status="in_progress", output=[])}),
                    ("response.output_item.added", {"output_index": 0,
                        "item": dict(item, status="in_progress", content=[])}),
                    ("response.output_text.delta", {"output_index": 0, "content_index": 0,
                        "item_id": item["id"], "delta": f"fixture final {number}"}),
                    ("response.output_item.done", {"output_index": 0, "item": item}),
                    ("response.completed", {"response": response}),
                ]
                if number == 1:
                    frames = [frame for frame in frames if frame[0] != "response.output_text.delta"]
                data = "".join("event: " + kind + "\ndata: " +
                    json.dumps(dict(payload, type=kind)) + "\n\n" for kind, payload in frames).encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(data)
                self.wfile.flush()
                self.close_connection = True
            except Exception as error:
                errors.append(str(error))
                self.send_error(500)

    server = HTTPServer(("127.0.0.1", 0), Provider)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        with tempfile.TemporaryDirectory(prefix="native-adoption-", dir="/var/tmp") as root:
            home = Path(root)
            native = home / ".codex"
            native.mkdir()
            # Config contains only local fixture routing, never auth.json.
            restricted = (
                'web_search="disabled"\n[features]\n'
                'shell_tool=false\nunified_exec=false\nmulti_agent=false\n'
                'apps=false\nremote_plugin=false\ngoals=false\nview_image=false\n'
                'image_generation=false\nhooks=false\nshell_snapshot=false\nmemories=false\n'
            ) if bounded_sdk_catalog else ""
            (native / "config.toml").write_text(
                'model="fixture-model"\nmodel_provider="fixture"\n' + restricted +
                '[model_providers.fixture]\nname="Native fixture"\n'
                f'base_url="http://127.0.0.1:{server.server_port}/v1"\n'
                'wire_api="responses"\nrequires_openai_auth=false\n'
                + ('' if host_tools else '[mcp_servers.fixture]\n' +
                 'command=' + json.dumps(sys.executable) + '\n' +
                 'args=' + json.dumps([str(Path(__file__).resolve()), "--mcp-fixture"]) + '\n'),
                encoding="utf-8")
            if owned_launch:
                workspace = home / "workspace"
                workspace.mkdir()
                (workspace / ".codex").mkdir()
                planted = home / "unexpected-project-command"
                (workspace / ".codex/config.toml").write_text(
                    'model="project-must-not-load"\n[mcp_servers.planted]\n'
                    'command="/bin/sh"\nargs=' + json.dumps(["-c", "touch " + str(planted)]) + '\n')
                (native / "config.toml").write_text(
                    'model="fixture-model"\nmodel_provider="aimee_ir"\n'
                    'approval_policy="never"\nsandbox_mode="read-only"\n'
                    'cli_auth_credentials_store="file"\nmcp_oauth_credentials_store="file"\n' +
                    restricted + 'js_repl=false\n' +
                    '[projects.' + json.dumps(str(workspace)) + ']\ntrust_level="untrusted"\n'
                    '[model_providers.aimee_ir]\nname="Aimee canonical host"\n'
                    f'base_url="http://127.0.0.1:{server.server_port}/v1"\n'
                    'wire_api="responses"\nrequires_openai_auth=false\n'
                    '[model_providers.aimee_ir.http_headers]\nAuthorization="Bearer fixture-token"\n')
                expected_file = home / "expected-launch.json"
                expected_file.write_text(json.dumps(tomllib.loads((native / "config.toml").read_text())))
            executable_dir = home / "bin"
            executable_dir.mkdir()
            (executable_dir / "codex").symlink_to(codex_binary)
            environment = {
                "HOME": root, "CODEX_HOME": str(native), "TMPDIR": root,
                "PATH": str(executable_dir) + os.pathsep + os.environ.get("PATH", "/usr/bin:/bin"),
                "LANG": "C.UTF-8", "OTEL_SDK_DISABLED": "true",
                "HTTP_PROXY": "http://127.0.0.1:9", "HTTPS_PROXY": "http://127.0.0.1:9",
                "ALL_PROXY": "http://127.0.0.1:9", "NO_PROXY": "127.0.0.1,localhost",
            }

            if flat_tools:
                environment["AIMEE_TEST_FLAT_HOST_TOOLS"] = "1"
            if owned_launch:
                environment.update(AIMEE_TEST_OWNED_CODEX_CONFIG=str(expected_file),
                    AIMEE_TEST_OWNED_CODEX_BINARY=str(codex_binary),
                    AIMEE_TEST_OWNED_CODEX_URL=f"http://127.0.0.1:{server.server_port}")

            def run(resume=None, history=None):
                command = [str(adapter_binary), "--native-history-probe"]
                if host_tools:
                    command.append("--host-tools")
                if resume:
                    command.append(resume)
                if history:
                    command.extend(["--import", str(history)])
                process = subprocess.run(command, env=environment, cwd=root, text=True,
                    capture_output=True, timeout=45, check=False)
                assert process.returncode == 0, (process.stdout, process.stderr, errors)
                report = json.loads(process.stdout)
                assert not report["error"], report
                assert report["thread_id"], report
                return report

            if owned_launch:
                # Configuration proof must fail before thread creation, model
                # traffic or the host-ready/cache callback. No credentials or
                # arbitrary project commands are involved in these refusals.
                original_expected = expected_file.read_text()
                original_config = (native / "config.toml").read_text()
                for mismatch in ("model", "notify"):
                    refused = json.loads(original_expected)
                    if mismatch == "model":
                        refused["model"] = "unattested-model"
                    else:
                        refused["notify"] = ["/bin/false"]
                        (native / "config.toml").write_text(
                            'notify=["/bin/false"]\n' + original_config)
                    expected_file.write_text(json.dumps(refused))
                    process = subprocess.run(
                        [str(adapter_binary), "--native-history-probe", "--host-tools"],
                        env=environment, cwd=root, text=True, capture_output=True,
                        timeout=45, check=False)
                    assert process.returncode != 0, (mismatch, process.stdout, process.stderr)
                    rejection = json.loads(process.stdout)
                    assert rejection["error"] and not rejection["thread_id"], rejection
                    assert not rejection["host_launch_digest"], rejection
                    assert not rejection["events"] and not rejection["host_calls"], rejection
                    assert not requests and not planted.exists(), (requests, rejection)
                    (native / "config.toml").write_text(original_config)
                    expected_file.write_text(original_expected)

            first = run()
            assert first["text"] == "<S>fixture final 2<E>", first
            if host_tools:
                assert len(first["host_calls"]) == 1, first
                observed = first["host_calls"][0]
                assert observed["call_id"] == "call_native_fixture", observed
                assert observed["namespace"] == namespace and observed["name"] == "fail", observed
                assert "0.123456789012345678901234567890" in observed["arguments_json"], observed
            # A second adapter invocation starts a new SDK process. An owned
            # scheduler also loses its ephemeral HOME/indexes: only its scoped
            # native conversation files survive, as in the production owner.
            if owned_launch:
                next_home = home / "next-private-home"
                next_native = next_home / ".codex"
                next_workspace = next_home / "workspace"
                next_native.mkdir(parents=True)
                next_workspace.mkdir()
                (next_native / "sessions").symlink_to(native / "sessions", target_is_directory=True)
                next_config = (native / "config.toml").read_text().replace(
                    json.dumps(str(workspace)), json.dumps(str(next_workspace)))
                (next_native / "config.toml").write_text(next_config)
                next_expected = next_home / "expected-launch.json"
                next_expected.write_text(json.dumps(tomllib.loads(next_config)))
                environment.update(HOME=str(next_home), CODEX_HOME=str(next_native),
                    TMPDIR=str(next_home), AIMEE_TEST_OWNED_CODEX_CONFIG=str(next_expected))
            # Retain the adopted items plus the completed turn without reinjection.
            second = run(first["thread_id"])
            assert second["thread_id"] == first["thread_id"], second
            assert second["text"] == "<S>fixture final 3<E>", second
            assert not errors, errors
            assert len(requests) == 3, len(requests)
            for number, request in enumerate(requests):
                items = request["input"]
                users = [item for item in items if item.get("role") == "user"]
                assistants = [item for item in items if item.get("role") == "assistant"]
                assert any("prior user" in json.dumps(item) for item in users), items
                assert any("prior assistant" in json.dumps(item) for item in assistants), items
                calls = [item for item in items if item.get("type") == "function_call"]
                results = [item for item in items if item.get("type") == "function_call_output"]
                assert len(calls) == len(results) == (1 if number == 0 else 2), items
                if number:
                    assert calls[1]["call_id"] == results[1]["call_id"] == "call_native_fixture"
                    assert json.loads(calls[1]["arguments"]) == expected_arguments
                    if host_tools:
                        assert "0.123456789012345678901234567890" in calls[1]["arguments"]
                    assert "refused" in json.dumps(results[1]), results[1]
                assert calls[0]["call_id"] == results[0]["call_id"] == "call_canonical_fixture"
                assert json.loads(calls[0]["arguments"]) == {"path": "fixture.txt"}
                assert results[0]["output"] == "prior tool result"
                encoded = json.dumps(items)
                assert "current user" in encoded
                if number == 2:
                    assert "fixture final 2" in encoded and "after restart" in encoded
            if owned_launch:
                assert not first["events"] and not second["events"], (first, second)
                assert len(first["host_launch_digest"]) == 64
                assert first["host_launch_digest"] == second["host_launch_digest"]
                assert not planted.exists(), "disabled project MCP command executed"
                unseen = home / "owned-tail.json"
                unseen.write_text(json.dumps([
                    {"role": "user", "blocks": [{"type": "text", "text": "owned unseen user"}]},
                    {"role": "assistant", "blocks": [{"type": "text", "text": "owned unseen assistant"}]},
                ]))
                third = run(first["thread_id"], unseen)
                assert third["thread_id"] == first["thread_id"] and not third["events"]
                assert third["host_launch_digest"] == first["host_launch_digest"]
                assert not errors and len(requests) == 4, errors
                tail = json.dumps(requests[-1]["input"])
                assert tail.count("owned unseen user") == tail.count("owned unseen assistant") == 1
                assert tail.count("call_canonical_fixture") == 2, tail
                assert not planted.exists()
                print("PASS: descriptor-launched real SDK verifies effective config before replay; "
                      "project MCP disabled, exact host arguments, stable launch digest, "
                      "resume/unseen tail and SDK-observation suppression")
                return
            tool_blocks = [block for event in first["events"]
                for block in event.get("payload", {}).get("blocks", [])
                if block.get("type") in ("tool_use", "tool_result")]
            assert len(tool_blocks) == 2, first
            assert tool_blocks[0]["tool_id"] == tool_blocks[1]["tool_id"], tool_blocks
            assert tool_blocks[0]["name"] == "fail", tool_blocks
            assert tool_blocks[1]["is_error"] is True, tool_blocks
            assert tool_blocks[0]["input"] == expected_arguments, tool_blocks
            assert tool_blocks[0]["namespace"] == namespace, tool_blocks
            assert "refused" in json.dumps(tool_blocks[1]["content"]), tool_blocks
            # Rebuild from persisted canonical messages into a different native
            # thread, rather than only proving opaque same-thread continuation.
            persisted = home / "history.json"
            # Persist the canonical C codec's exact JSON, rather than rounding
            # admitted argument decimals through Python's binary float decoder.
            persisted.write_text(first["history_json"], encoding="utf-8")
            third = run(history=persisted)
            assert third["thread_id"] != first["thread_id"], third
            assert third["text"] == "<S>fixture final 4<E>", third
            adopted = requests[3]["input"]
            call = next(item for item in adopted if item.get("type") == "function_call")
            result = next(item for item in adopted if item.get("type") == "function_call_output")
            assert call["call_id"] == result["call_id"] == "call_native_fixture", adopted
            assert call["name"] == "fail" and call["namespace"] == namespace, call
            assert json.loads(call["arguments"]) == expected_arguments
            if host_tools:
                assert "0.123456789012345678901234567890" in call["arguments"], call
            assert "refused" in json.dumps(result["output"]), result
            assert not errors and len(requests) == 4, (errors, len(requests))
            # A matched cache checkpoint supplies only unseen canonical items.
            # Exercise resume AND adoption through the actual compiled adapter.
            tail = home / "tail.json"
            tail.write_text(json.dumps([
                {"role": "user", "blocks": [{"type": "text", "text": "unseen canonical tail"}]},
                {"role": "assistant", "blocks": [{"type": "tool_use", "tool_id": "call_tail_fixture",
                    "name": "fail", "namespace": namespace, "input": {"value": 7}}]},
                {"role": "user", "blocks": [{"type": "tool_result", "tool_id": "call_tail_fixture",
                    "content": "unseen tool result", "is_error": False}]},
                {"role": "assistant", "blocks": [{"type": "text", "text": "unseen assistant"}]},
            ]), encoding="utf-8")
            fourth = run(first["thread_id"], history=tail)
            assert fourth["thread_id"] == first["thread_id"], fourth
            assert fourth["text"] == "<S>fixture final 5<E>", fourth
            resumed = requests[4]["input"]
            calls = [item for item in resumed if item.get("type") == "function_call"]
            results = [item for item in resumed if item.get("type") == "function_call_output"]
            for call_id in ("call_canonical_fixture", "call_native_fixture", "call_tail_fixture"):
                assert sum(item["call_id"] == call_id for item in calls) == 1, resumed
                assert sum(item["call_id"] == call_id for item in results) == 1, resumed
            tail_call = next(item for item in calls if item["call_id"] == "call_tail_fixture")
            assert tail_call["namespace"] == namespace and tail_call["name"] == "fail", tail_call
            assert json.loads(tail_call["arguments"]) == {"value": 7}, tail_call
            encoded = json.dumps(resumed)
            assert encoded.count("prior user") == encoded.count("unseen canonical tail") == 1, resumed
            assert "unseen tool result" in encoded and "unseen assistant" in encoded, resumed
            assert not errors and len(requests) == 5, (errors, len(requests))
            print("PASS: compiled Aimee adapter adopts typed history into the real SDK; "
                  "restart/resume appends unseen namespaced items without duplicating the committed prefix; "
                  + ("typed host tools and exact arguments verified" if host_tools else
                     "bounded SDK catalog verified" if bounded_sdk_catalog else "ordinary SDK profile"))
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)


if __name__ == "__main__":
    if sys.argv[1:] == ["--mcp-fixture"]:
        mcp_fixture()
        sys.exit(0)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adapter-binary", required=True, type=Path)
    parser.add_argument("--codex-binary", required=True, type=Path)
    arguments = parser.parse_args()
    for bounded_sdk_catalog, host_tools, owned_launch, flat_tools in (
            (False, False, False, False), (True, False, False, False),
            (True, True, False, False), (True, True, True, False), (True, True, True, True)):
        qualify(arguments.adapter_binary.resolve(strict=True),
                arguments.codex_binary.resolve(strict=True), bounded_sdk_catalog=bounded_sdk_catalog,
                host_tools=host_tools, owned_launch=owned_launch, flat_tools=flat_tools)
