#!/usr/bin/env python3
"""check-sidecar-clients.py: contract test for the container LLM/embedder
thin-client sidecars.

The aimee-kb image popens stdlib HTTP clients that talk to the embedder service
(embed-remote.py) and an OpenAI-compatible LLM (llm-chat.py). The heavy backends
(sentence-transformers, llama.cpp) aren't available in CI, so this test stands
up tiny stub servers that mimic their wire contracts and asserts the clients
produce the expected stdout — i.e. the request/response/parse path the kb relies
on is correct, without any model.

Run: python3 scripts/check-sidecar-clients.py
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent


def _serve(handler_cls) -> tuple[ThreadingHTTPServer, str]:
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler_cls)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    host, port = server.server_address
    return server, f"http://{host}:{port}"


def _run(script: str, env: dict[str, str], stdin: str) -> str:
    proc = subprocess.run(
        [sys.executable, str(SCRIPTS / script)],
        input=stdin,
        capture_output=True,
        text=True,
        env={**os.environ, **env},
        timeout=30,
    )
    if proc.returncode != 0:
        raise AssertionError(f"{script} exited {proc.returncode}: {proc.stderr[:300]}")
    return proc.stdout


def check_embed_remote() -> None:
    vector = [round(i * 0.001, 3) for i in range(384)]

    class EmbedStub(BaseHTTPRequestHandler):
        def log_message(self, *a):  # quiet
            pass

        def do_POST(self):
            assert self.path.rstrip("/") == "/embed", self.path
            length = int(self.headers.get("content-length", "0") or "0")
            self.rfile.read(length)
            body = json.dumps(vector).encode()
            self.send_response(200)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server, url = _serve(EmbedStub)
    try:
        out = _run("embed-remote.py", {"AIMEE_EMBEDDER_URL": url}, "hello world")
        parsed = json.loads(out)
        assert isinstance(parsed, list) and len(parsed) == 384, f"got {len(parsed)} dims"
    finally:
        server.shutdown()
    print("  embed-remote.py: ok")


def check_llm_chat() -> None:
    class ChatStub(BaseHTTPRequestHandler):
        def log_message(self, *a):  # quiet
            pass

        def do_POST(self):
            assert self.path.rstrip("/") == "/v1/chat/completions", self.path
            length = int(self.headers.get("content-length", "0") or "0")
            self.rfile.read(length)
            body = json.dumps(
                {"choices": [{"message": {"content": "synthesised-ok"}}]}
            ).encode()
            self.send_response(200)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server, url = _serve(ChatStub)
    try:
        out = _run(
            "llm-chat.py",
            {"LLM_ENDPOINT": f"{url}/v1", "LLM_MODEL": "gemma-4-e4b-it", "LLM_API_KEY": ""},
            "say something",
        )
        assert "synthesised-ok" in out, f"unexpected output: {out!r}"
    finally:
        server.shutdown()
    print("  llm-chat.py: ok")


def _chat_stub(message: dict):
    """A stub /v1/chat/completions returning the given assistant message."""

    class ChatStub(BaseHTTPRequestHandler):
        def log_message(self, *a):  # quiet
            pass

        def do_POST(self):
            assert self.path.rstrip("/") == "/v1/chat/completions", self.path
            length = int(self.headers.get("content-length", "0") or "0")
            self.rfile.read(length)
            body = json.dumps({"choices": [{"message": message}]}).encode()
            self.send_response(200)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    return ChatStub


def _chat(message: dict, env_extra: dict | None = None) -> str:
    server, url = _serve(_chat_stub(message))
    try:
        env = {"LLM_ENDPOINT": f"{url}/v1", "LLM_MODEL": "stub", "LLM_API_KEY": ""}
        env.update(env_extra or {})
        return _run("llm-chat.py", env, "say something")
    finally:
        server.shutdown()


def check_llm_chat_reasoning_split() -> None:
    """THE CONTRACT: stdout carries the answer, never the reasoning.

    llm-chat.py is the ONE wire boundary for every sidecar (curator-extract,
    curator-synthesize, llm-rewrite, learning-synthesize). Consumers must never
    re-derive this from text — doing so is what cut valid JSON mid-string when a
    payload merely mentioned the tag.
    """
    # 1. Compliant server (llama.cpp --jinja): reasoning already separated.
    out = _chat({"content": "the-answer", "reasoning_content": "my hidden reasoning"})
    assert "the-answer" in out, f"answer missing: {out!r}"
    assert "hidden reasoning" not in out, f"reasoning leaked to stdout: {out!r}"

    # 2. Inlining server: a <think> PREFIX must be split off here, not downstream.
    out = _chat({"content": "<think>deliberating {\"draft\":1}</think>\nthe-answer"})
    assert "the-answer" in out, f"answer missing: {out!r}"
    assert "deliberating" not in out, f"reasoning leaked to stdout: {out!r}"
    assert "<think>" not in out, f"think tag leaked to stdout: {out!r}"

    # 3. THE REGRESSION: a mention of the tag in the ANSWER must survive intact.
    #    (A docstring about stripping think blocks is still an answer.)
    answer = '{"summary":"drops the </think> tag if present"}'
    out = _chat({"content": answer})
    assert "</think>" in out, f"tag in content was eaten: {out!r}"
    assert json.loads(out.strip())["summary"], f"answer corrupted: {out!r}"

    # 4. Both together: prefix stripped, mention preserved.
    out = _chat({"content": '<think>hmm</think>\n{"summary":"handles </think> tags"}'})
    parsed = json.loads(out.strip())
    assert parsed["summary"] == "handles </think> tags", f"got {parsed!r}"

    print("  llm-chat.py reasoning split: ok")


def main() -> int:
    print("sidecar-clients:")
    check_embed_remote()
    check_llm_chat()
    check_llm_chat_reasoning_split()
    print("sidecar-clients: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
