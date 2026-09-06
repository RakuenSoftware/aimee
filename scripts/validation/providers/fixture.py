#!/usr/bin/env python3
"""Deterministic external provider for the isolated Providers GUI E2E stack.

Only this vendor endpoint is a fixture. The browser, web proxy, server, config,
Vault, and model execution must be the real build under test. Keys below are
public test values, never credentials for an external service.
"""
import argparse
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

KEYS = {"fixture-key-a": "account-a", "fixture-key-b": "account-b", "fixture-key-rotated": "account-a-rotated"}
EVENTS = []
LOCK = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass  # Never log authorization headers.

    def reply(self, status, data):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def authorized(self):
        account = KEYS.get(self.headers.get("Authorization", "").removeprefix("Bearer "))
        with LOCK:
            EVENTS.append({"method": self.command, "path": self.path, "account": account, "authorized": account is not None})
        if not account:
            self.reply(401, {"error": {"message": "fixture authentication failed"}})
            return False
        if self.path.startswith("/unavailable/"):
            self.reply(503, {"error": {"message": "fixture deliberately unavailable"}})
            return False
        return True

    def do_GET(self):
        if self.path == "/health":
            return self.reply(200, {"status": "ok"})
        if self.path == "/events":
            with LOCK:
                return self.reply(200, list(EVENTS))
        if not self.authorized():
            return
        if self.path.endswith("/models"):
            return self.reply(200, {"data": [
                {"id": "fixture-model", "context_window": 32768, "max_output": 4096},
                {"id": "fixture-unknown-limits"},
            ]})
        self.reply(404, {"error": {"message": "not found"}})

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))) or b"{}")
        if not self.authorized():
            return
        if not self.path.endswith("/chat/completions"):
            return self.reply(404, {"error": {"message": "not found"}})
        model = body.get("model", "fixture-model")
        if body.get("stream"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            for delta, finish in [({"role": "assistant", "content": "ok"}, None), ({}, "stop")]:
                chunk = {"id": "fixture", "object": "chat.completion.chunk", "model": model,
                         "choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}
                self.wfile.write(("data: " + json.dumps(chunk) + "\n\n").encode())
            self.wfile.write(b"data: [DONE]\n\n")
            return
        self.reply(200, {"id": "fixture", "object": "chat.completion", "model": model,
                         "choices": [{"index": 0, "message": {"role": "assistant", "content": "ok"}, "finish_reason": "stop"}],
                         "usage": {"prompt_tokens": 4, "completion_tokens": 1, "total_tokens": 5}})


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=18765)
    parser.add_argument("--bind", default="127.0.0.1")
    options = parser.parse_args()
    ThreadingHTTPServer((options.bind, options.port), Handler).serve_forever()
