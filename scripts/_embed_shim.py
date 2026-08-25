#!/usr/bin/env python3
"""_embed_shim.py: stdlib-only adapter that exposes aimee's embedder contract
(POST /embed with raw UTF-8 text -> bare JSON float array; POST /embed_batch
with a JSON array of strings -> JSON array of vectors; GET /health) in front of a
llama.cpp `llama-server --embeddings` instance speaking the OpenAI
/v1/embeddings API. Used by scripts/test-embedder-qwen.sh to give the local-stack
e2e a real, small Qwen3-Embedding-0.6B (1024-d) embedder without torch/pip.

Usage: _embed_shim.py <llama_port> <shim_port> <dim> [model_label]
"""
import json
import sys
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

LLAMA_PORT = int(sys.argv[1])
SHIM_PORT = int(sys.argv[2])
DIM = int(sys.argv[3])
MODEL = sys.argv[4] if len(sys.argv) > 4 else "Qwen3-Embedding-0.6B"
SERVING_ID = f"{MODEL}/pooling=last/prefix=none/dim={DIM}"


def _embed_one(text):
    """POST a single string to llama-server /v1/embeddings, return its vector."""
    payload = json.dumps({"input": text, "model": MODEL}).encode()
    req = urllib.request.Request(
        f"http://127.0.0.1:{LLAMA_PORT}/v1/embeddings",
        data=payload,
        headers={"content-type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=60) as r:
        doc = json.load(r)
    return doc["data"][0]["embedding"]


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            self._send(
                200,
                {
                    "status": "ok",
                    "dim": DIM,
                    "model": MODEL,
                    "serving_id": SERVING_ID,
                },
            )
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        n = int(self.headers.get("content-length", 0))
        raw = self.rfile.read(n).decode("utf-8", "replace")
        try:
            path = urlsplit(self.path).path
            if path == "/embed":
                self._send(200, _embed_one(raw))
            elif path == "/embed_batch":
                texts = json.loads(raw)
                self._send(200, [_embed_one(t) for t in texts])
            else:
                self._send(404, {"error": "not found"})
        except Exception as exc:  # surface upstream failures as 502, not a hang
            self._send(502, {"error": str(exc)})

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", SHIM_PORT), Handler).serve_forever()
