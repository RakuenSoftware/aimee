#!/usr/bin/env python3
"""Opt-in real Codex SDK qualification through Aimee's compiled native adapter.

Run with --adapter-binary <unit-test-cli-codex> --codex-binary <codex>.
Uses only a loopback fixture provider and a fresh credential-free HOME. Synthetic
usage is protocol test data, never evidence of provider billing or cache savings.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer


def qualify(adapter_binary, codex_binary):
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
                number = len(requests)
                item = {
                    "id": f"msg_fixture_{number}", "type": "message", "role": "assistant",
                    "status": "completed", "content": [{"type": "output_text",
                    "text": f"fixture final {number}", "annotations": []}],
                }
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
            (native / "config.toml").write_text(
                'model="fixture-model"\nmodel_provider="fixture"\n'
                '[model_providers.fixture]\nname="Native fixture"\n'
                f'base_url="http://127.0.0.1:{server.server_port}/v1"\n'
                'wire_api="responses"\nrequires_openai_auth=false\n', encoding="utf-8")
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

            def run(resume=None):
                command = [str(adapter_binary), "--native-history-probe"]
                if resume:
                    command.append(resume)
                process = subprocess.run(command, env=environment, cwd=root, text=True,
                    capture_output=True, timeout=45, check=False)
                assert process.returncode == 0, (process.stdout, process.stderr)
                report = json.loads(process.stdout)
                assert not report["error"], report
                assert report["thread_id"], report
                return report

            first = run()
            assert first["text"] == "<S>fixture final 1<E>", first
            # A second adapter invocation starts a new SDK process. Resume must
            # retain the adopted items plus the completed turn without reinjection.
            second = run(first["thread_id"])
            assert second["thread_id"] == first["thread_id"], second
            assert second["text"] == "<S>fixture final 2<E>", second
            assert not errors, errors
            assert len(requests) == 2, len(requests)
            for number, request in enumerate(requests):
                items = request["input"]
                users = [item for item in items if item.get("role") == "user"]
                assistants = [item for item in items if item.get("role") == "assistant"]
                assert any("prior user" in json.dumps(item) for item in users), items
                assert any("prior assistant" in json.dumps(item) for item in assistants), items
                calls = [item for item in items if item.get("type") == "function_call"]
                results = [item for item in items if item.get("type") == "function_call_output"]
                assert len(calls) == len(results) == 1, items
                assert calls[0]["call_id"] == results[0]["call_id"] == "call_canonical_fixture"
                assert json.loads(calls[0]["arguments"]) == {"path": "fixture.txt"}
                assert results[0]["output"] == "prior tool result"
                encoded = json.dumps(items)
                assert "current user" in encoded
                if number:
                    assert "fixture final 1" in encoded and "after restart" in encoded
            print("PASS: compiled Aimee adapter adopts typed history into the real SDK; "
                  "restart/resume retains roles, tool IDs/results and completed turns")
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adapter-binary", required=True, type=Path)
    parser.add_argument("--codex-binary", required=True, type=Path)
    arguments = parser.parse_args()
    qualify(arguments.adapter_binary.resolve(strict=True), arguments.codex_binary.resolve(strict=True))
