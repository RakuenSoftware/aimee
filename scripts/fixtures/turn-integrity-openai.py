#!/usr/bin/env python3
"""Deterministic OpenAI-compatible provider for turn-integrity live E2E."""

from __future__ import annotations

import json
import os
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


HOST = os.environ.get("TI_FIXTURE_HOST", "127.0.0.1")
PORT = int(os.environ.get("TI_FIXTURE_PORT", "18991"))
LOG = os.environ.get("TI_FIXTURE_LOG", "/tmp/turn-integrity-provider.jsonl")


def strings(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, list):
        for item in value:
            yield from strings(item)
    elif isinstance(value, dict):
        for item in value.values():
            yield from strings(item)


def response_for(body):
    wire = "\n".join(strings(body))
    messages = body.get("messages", [])
    has_tool_result = any(isinstance(m, dict) and m.get("role") == "tool" for m in messages)

    if has_tool_result:
        content = "TURN_INTEGRITY_FIXTURE_DONE"
        if "TI_SEARCH_KB_DOWN" in wire:
            content = "TURN_INTEGRITY_KB_FAILURE_OBSERVED"
        elif "TI_SEARCH_EMPTY" in wire:
            content = "TURN_INTEGRITY_EMPTY_OBSERVED"
        elif "TI_GIT_PUSH" in wire:
            content = "TURN_INTEGRITY_PUSH_OBSERVED"
        elif "TI_MCP_TIMEOUT" in wire:
            content = "TURN_INTEGRITY_UNKNOWN_OUTCOME_OBSERVED"
        return completion(content)

    name = None
    args = None
    if "TI_WRITE_FILE" in wire:
        name = "write_file"
        args = {"path": "turn-integrity-live.txt", "content": "live-contract-ok\n"}
    elif "TI_EDIT_FILE" in wire:
        name = "edit_file"
        args = {
            "path": "turn-integrity-live.txt",
            "old_string": "live-contract-ok",
            "new_string": "live-contract-edited",
        }
    elif "TI_SEARCH_EMPTY" in wire or "TI_SEARCH_KB_DOWN" in wire:
        name = "search_memory"
        args = {"query": "turn-integrity-no-such-fact-8f67c15c"}
    elif "TI_GIT_PUSH" in wire:
        name = "git_push"
        args = {}
    elif "TI_MCP_TIMEOUT" in wire:
        name = "ti_remote:mutate"
        args = {"request": "turn-integrity-live-mutation"}

    if not name:
        return completion("TURN_INTEGRITY_FIXTURE_DONE")
    return {
        "id": "chatcmpl-ti-tool",
        "object": "chat.completion",
        "created": int(time.time()),
        "model": "turn-integrity-fixture",
        "choices": [
            {
                "index": 0,
                "message": {
                    "role": "assistant",
                    "content": None,
                    "tool_calls": [
                        {
                            "id": "call-ti-1",
                            "type": "function",
                            "function": {"name": name, "arguments": json.dumps(args)},
                        }
                    ],
                },
                "finish_reason": "tool_calls",
            }
        ],
        "usage": {"prompt_tokens": 10, "completion_tokens": 4, "total_tokens": 14},
    }


def completion(content):
    return {
        "id": "chatcmpl-ti-final",
        "object": "chat.completion",
        "created": int(time.time()),
        "model": "turn-integrity-fixture",
        "choices": [
            {
                "index": 0,
                "message": {"role": "assistant", "content": content},
                "finish_reason": "stop",
            }
        ],
        "usage": {"prompt_tokens": 11, "completion_tokens": 3, "total_tokens": 14},
    }


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        return

    def send_json(self, status, payload):
        data = json.dumps(payload, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path.rstrip("/").endswith("/models"):
            self.send_json(
                200,
                {
                    "object": "list",
                    "data": [
                        {
                            "id": "turn-integrity-fixture",
                            "object": "model",
                            "owned_by": "aimee-e2e",
                        }
                    ],
                },
            )
        else:
            self.send_json(404, {"error": {"message": "not found"}})

    def do_POST(self):
        length = int(self.headers.get("content-length", "0"))
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError:
            self.send_json(400, {"error": {"message": "invalid json"}})
            return
        with open(LOG, "a", encoding="utf-8") as out:
            out.write(
                json.dumps({"path": self.path, "body": body}, separators=(",", ":")) + "\n"
            )
        if self.path.rstrip("/").endswith("/chat/completions"):
            self.send_json(200, response_for(body))
        else:
            self.send_json(404, {"error": {"message": "not found"}})


if __name__ == "__main__":
    ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()
