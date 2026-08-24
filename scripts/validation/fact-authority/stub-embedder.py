#!/usr/bin/env python3
"""A minimal embedding service, so the EMBED stage (5891) can be exercised.

WHY A STUB. EMBED is the one memory-module stage this branch never ran: no
embedder was configured, `aimee status` reported "BLOCKED: no embedder
configured -- memory and KB search cannot embed", and the llama-server serving
Qwen answers 501 for /v1/embeddings. Standing up a real embedding model on that
box is a deployment exercise unrelated to what is under test.

WHAT IS REAL AND WHAT IS NOT. The PATH is real: the kb resolves the configured
endpoint, calls it over HTTP, parses the reply, and stores vectors. That is
exactly the wiring that was never exercised. The VECTORS are not real -- they
carry no semantic meaning, so this proves the stage runs, not that recall ranks
well. Any conclusion about retrieval QUALITY from a run against this stub would
be worthless, and the probe that uses it says so.

DETERMINISTIC, not random: the same text always yields the same vector, so a
re-run cannot change an answer for reasons nothing in the system controls. A
random stub would make every recall comparison unrepeatable and quietly
untrustworthy.

Contract, and the two paths differ in a way worth stating because it cost a
round trip to find:

    GET  /health                      -> {"serving_id": "..."}
    POST /embed_batch[?input_type=X]  body: JSON array of strings
                                      -> JSON array of N arrays of DIM floats
    POST /embed[?input_type=X]        body: THE RAW TEXT, not JSON
                                      -> a single JSON array of DIM floats

memory_core_scope_embed.c says so outright: "the polarity rides in the query
string because the body is the raw text itself". A stub that assumes JSON on
both answers 400 to every single-text call, which is what happened first.

Usage: stub-embedder.py [PORT] [DIM]
"""
import hashlib
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8799
DIM = int(sys.argv[2]) if len(sys.argv) > 2 else 384
SERVING_ID = "aimee-e2e-stub-v1-dim%d" % DIM


def vector(text: str):
    """A unit-ish vector derived from the text, stable across runs and hosts."""
    out, seed = [], text.encode("utf-8", "replace")
    while len(out) < DIM:
        seed = hashlib.sha256(seed).digest()
        for i in range(0, len(seed), 4):
            if len(out) >= DIM:
                break
            n = int.from_bytes(seed[i:i + 4], "big")
            out.append((n / 2147483647.5) - 1.0)
    norm = sum(v * v for v in out) ** 0.5 or 1.0
    return [v / norm for v in out]


def texts_from(body: bytes):
    """Accept the shapes /embed_batch is called with, refuse anything else.

    Guessing at an unrecognised shape would return well-formed vectors for the
    wrong text, which is worse than refusing: the caller would store them.
    """
    payload = json.loads(body.decode("utf-8"))
    if isinstance(payload, list) and all(isinstance(x, str) for x in payload):
        return payload
    if isinstance(payload, str):
        return [payload]
    if isinstance(payload, dict):
        for key in ("texts", "input", "inputs"):
            v = payload.get(key)
            if isinstance(v, list) and all(isinstance(x, str) for x in v):
                return v
            if isinstance(v, str):
                return [v]
    raise ValueError("unrecognised request shape")


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, payload):
        blob = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(blob)))
        self.end_headers()
        self.wfile.write(blob)

    def do_GET(self):
        if self.path.split("?")[0] in ("/health", "/healthz"):
            self._send(200, {"serving_id": SERVING_ID, "dim": DIM})
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        route = self.path.split("?")[0]
        raw = b""
        try:
            n = int(self.headers.get("content-length") or 0)
            raw = self.rfile.read(n)
        except Exception as exc:  # noqa: BLE001
            self._send(400, {"error": "unreadable body: %s" % exc})
            return
        if route == "/embed":
            # Raw text, and a SINGLE vector back rather than a list of one.
            self._send(200, vector(raw.decode("utf-8", "replace")))
            return
        if route != "/embed_batch":
            self._send(404, {"error": "not found"})
            return
        try:
            texts = texts_from(raw)
        except Exception as exc:  # noqa: BLE001 - the reason goes to the caller
            # Echo what arrived. A stub that refuses silently is as hard to
            # diagnose as the thing it was built to test.
            sys.stderr.write("stub-embedder: unrecognised body: %r\n" % (raw[:400],))
            self._send(400, {"error": "bad request: %s" % exc})
            return
        self._send(200, [vector(t) for t in texts])

    def log_message(self, fmt, *args):
        sys.stderr.write("stub-embedder: " + (fmt % args) + "\n")


if __name__ == "__main__":
    sys.stderr.write("stub-embedder: :%d dim=%d id=%s\n" % (PORT, DIM, SERVING_ID))
    HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
