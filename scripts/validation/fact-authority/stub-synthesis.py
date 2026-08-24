#!/usr/bin/env python3
"""A minimal synthesis endpoint, so the kb's LLM lane starts.

The memory_facts drain -- which runs the deterministic pattern extractor and
commits through the typed-fact write gate -- lives on the curator's LLM lane,
and that lane only starts when a synthesis endpoint is CONFIGURED. Without one
the drain never runs, memory_facts jobs sit pending forever, and nothing ever
reaches db2_fact_commit.

This stub exists to start the lane, not to extract anything. It answers every
chat completion with an empty fact list, so the LLM pass contributes nothing and
what gets committed is exactly what the pattern extractor found -- which is the
path under test. Run AS ROOT in the container.
"""
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

EMPTY_FACTS = json.dumps({"facts": []})


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):  # noqa: N802
        length = int(self.headers.get("Content-Length") or 0)
        self.rfile.read(length)
        body = json.dumps({
            "id": "stub",
            "object": "chat.completion",
            "choices": [{
                "index": 0,
                "message": {"role": "assistant", "content": EMPTY_FACTS},
                "finish_reason": "stop",
            }],
            "usage": {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0},
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802
        self.send_response(200)
        self.send_header("Content-Length", "2")
        self.end_headers()
        self.wfile.write(b"ok")

    def log_message(self, *args):
        return


if __name__ == "__main__":
    HTTPServer(("127.0.0.1", 8799), Handler).serve_forever()
