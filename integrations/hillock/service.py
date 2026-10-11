#!/usr/bin/env python3
"""Aimee's bounded, stateless Hillock HDC retrieval profile (no chat generation)."""
import hmac
import json
import os
from pathlib import Path
import re
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

UPSTREAM_REVISION = "1edd166ead75b85a9ab95cd6ba4faf7011ad567c"
MAX_BODY = 1 << 20
MAX_RECORDS = 256
MAX_TOKENS = 4096
STOP_WORDS = frozenset("a an the is are was were of to in and or do does did what who where when how tell me about please".split())


def load_engine():
    upstream = Path(os.environ.get("HILLOCK_SOURCE", "/opt/hillock"))
    if (upstream / "AIMEE_UPSTREAM_REVISION").read_text().strip() != UPSTREAM_REVISION:
        raise RuntimeError("Hillock source revision mismatch")
    sys.path.insert(0, str(upstream))
    from reservoir import HyperdimensionalReservoir
    # No automatic downloads, LLM, mutable reservoir state, learned synapses,
    # GloVe vocabulary, or extractor. Only the pinned HYDRA/HDC ranking core.
    return HyperdimensionalReservoir(dimension=10000, glove_dict={})


def tokens(text):
    return [t for t in re.findall(r"\w+", text.lower()) if t not in STOP_WORDS]


def rank(engine, request):
    if not isinstance(request, dict) or set(request) != {"query", "candidates", "limit"}:
        raise ValueError("invalid request")
    query, candidates, limit = request["query"], request["candidates"], request["limit"]
    if not isinstance(query, str) or not query.strip() or len(query.encode()) > 16384:
        raise ValueError("invalid query")
    if type(limit) is not int or not 1 <= limit <= MAX_RECORDS:
        raise OverflowError("limit capacity exceeded")
    if not isinstance(candidates, list) or len(candidates) > MAX_RECORDS:
        raise OverflowError("candidate capacity exceeded")
    seen, words, prepared = set(), set(), []
    query_tokens = tokens(query)
    if len(query_tokens) > 64:
        raise OverflowError("query token capacity exceeded")
    words.update(query_tokens)
    for record in candidates:
        if not isinstance(record, dict) or set(record) != {"id", "revision", "text"}:
            raise ValueError("invalid candidate")
        record_id, revision, text = record["id"], record["revision"], record["text"]
        if type(record_id) is not int or not 0 < record_id < 2**63 or record_id in seen:
            raise ValueError("invalid candidate identity")
        if not isinstance(revision, str) or not re.fullmatch(r"[0-9a-f]{64}", revision):
            raise ValueError("invalid candidate revision")
        if not isinstance(text, str):
            raise ValueError("invalid candidate text")
        record_tokens = tokens(text)
        if len(record_tokens) > 256:
            raise OverflowError("record token capacity exceeded")
        seen.add(record_id)
        words.update(record_tokens)
        if len(words) > MAX_TOKENS:
            raise OverflowError("snapshot vocabulary capacity exceeded")
        prepared.append((record_id, revision, record_tokens))
    # Request-local vectors: discard them when ranking returns. Never call
    # get_or_allocate_hypervector or persist state on the shared engine.
    if any(len(word.encode()) > 128 for word in words):
        raise OverflowError("token byte capacity exceeded")
    deadline = time.monotonic() + 10
    vectors = {}
    for word in sorted(words):
        if time.monotonic() >= deadline:
            raise TimeoutError("retrieval compute deadline exceeded")
        vectors[word] = engine.resolve_predicate_hypervector(word)
    query_hvs = [vectors[word] for word in query_tokens]
    hits = []
    for record_id, revision, record_tokens in prepared:
        if time.monotonic() >= deadline:
            raise TimeoutError("retrieval compute deadline exceeded")
        score = engine.hydra_late_interaction_maxsim(query_hvs, [vectors[word] for word in record_tokens])
        if score >= 0.20:
            hits.append({"id": record_id, "revision": revision, "score": score})
    hits.sort(key=lambda hit: (-hit["score"], hit["id"]))
    return {"hits": hits[:limit]}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def log_message(self, *_args):
        pass  # No memory text, query strings, bearer tokens or access histories.

    def setup(self):
        super().setup()
        self.connection.settimeout(10)

    def respond(self, status, payload):
        raw = json.dumps(payload, allow_nan=False, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def authorized(self):
        return self.server.token is None or hmac.compare_digest(
            self.headers.get("Authorization", ""), "Bearer " + self.server.token)

    def do_GET(self):
        if self.path != "/v1/health":
            return self.respond(404, {"error": "unknown route"})
        if not self.authorized():
            return self.respond(401, {"error": "authentication required"})
        self.respond(200, {"version": 1, "upstream_revision": UPSTREAM_REVISION,
                           "profile": "hdc-subword", "stateless": True})

    def do_POST(self):
        if self.path != "/v1/rank":
            return self.respond(404, {"error": "unknown route"})
        if not self.authorized():
            return self.respond(401, {"error": "authentication required"})
        if self.headers.get("Content-Type") != "application/json" or self.headers.get("Transfer-Encoding"):
            return self.respond(400, {"error": "JSON content length required"})
        try:
            length = int(self.headers.get("Content-Length", "-1"))
            if length < 0:
                raise ValueError("content length required")
            if length > MAX_BODY:
                raise OverflowError("request capacity exceeded")
            raw = self.rfile.read(length)
            if len(raw) != length:
                raise ValueError("incomplete request")
            request = json.loads(raw)
            self.respond(200, rank(self.server.engine, request))
        except TimeoutError:
            self.respond(504, {"error": "retrieval compute deadline exceeded"})
        except OverflowError:
            self.respond(413, {"error": "retrieval capacity exceeded"})
        except (ValueError, UnicodeError, TypeError):
            self.respond(400, {"error": "invalid retrieval request"})


def main():
    token_file = os.environ.get("HILLOCK_TOKEN_FILE")
    token = Path(token_file).read_text().strip() if token_file else None
    token = token or None
    if not token and os.environ.get("HILLOCK_AUTH") != "none":
        raise RuntimeError("set HILLOCK_TOKEN_FILE or explicitly select HILLOCK_AUTH=none")
    server = HTTPServer((os.environ.get("HILLOCK_HOST", "127.0.0.1"), int(os.environ.get("HILLOCK_PORT", "8097"))), Handler)
    server.token, server.engine = token, load_engine()
    server.serve_forever()


if __name__ == "__main__":
    main()
