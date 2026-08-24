#!/usr/bin/env python3
"""Small deterministic embedder for full-stack contract and failure-path tests.

This is deliberately not a model-quality benchmark. It implements the same
health, single-vector and batch-vector HTTP contract as the production
embedder, using only the standard library, so daemon/module/Postgres E2E rigs
can exercise real vector persistence without downloading model weights.
"""

import hashlib
import json
import math
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit


PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18742
DIM = int(sys.argv[2]) if len(sys.argv) > 2 else 384
SERVING_ID = f"deterministic-test/hash-token-v1/{DIM}"


def embed(text: str) -> list[float]:
    vector = [0.0] * DIM
    tokens = re.findall(r"[a-z0-9_]+", text.lower()) or [text]
    for token in tokens:
        digest = hashlib.sha256(token.encode()).digest()
        index = int.from_bytes(digest[:4], "big") % DIM
        vector[index] += -1.0 if digest[4] & 1 else 1.0
    norm = math.sqrt(sum(value * value for value in vector)) or 1.0
    return [value / norm for value in vector]


class Handler(BaseHTTPRequestHandler):
    def send_json(self, status: int, value: object) -> None:
        body = json.dumps(value, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        if urlsplit(self.path).path == "/health":
            self.send_json(
                200,
                {"status": "ok", "dim": DIM, "serving_id": SERVING_ID},
            )
        else:
            self.send_json(404, {"error": "not found"})

    def do_POST(self) -> None:
        size = int(self.headers.get("content-length", "0"))
        raw = self.rfile.read(size).decode("utf-8", "replace")
        try:
            path = urlsplit(self.path).path
            if path == "/embed":
                self.send_json(200, embed(raw))
            elif path == "/embed_batch":
                values = json.loads(raw)
                if not isinstance(values, list) or not all(
                    isinstance(value, str) for value in values
                ):
                    raise ValueError("expected an array of strings")
                self.send_json(200, [embed(value) for value in values])
            else:
                self.send_json(404, {"error": "not found"})
        except (ValueError, json.JSONDecodeError) as exc:
            self.send_json(400, {"error": str(exc)})

    def log_message(self, *_args: object) -> None:
        pass


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
