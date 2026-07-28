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
import re
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


def _run_missing_managed_auth(script: str, endpoint_key: str, url: str, stdin: str) -> None:
    env = {
        **os.environ,
        endpoint_key: url,
        "AIMEE_LLM_AUTH_REQUIRED": "1",
        "AIMEE_LLM_AUTH_TOKEN": "",
    }
    proc = subprocess.run(
        [sys.executable, str(SCRIPTS / script)],
        input=stdin,
        capture_output=True,
        text=True,
        env=env,
        timeout=30,
    )
    assert proc.returncode != 0, f"{script} accepted missing managed auth"
    assert "AIMEE_LLM_AUTH_TOKEN is empty" in proc.stderr, proc.stderr


def check_embed_remote() -> None:
    vector = [round(i * 0.001, 3) for i in range(384)]
    token = "kb-to-llm-test-token"

    class EmbedStub(BaseHTTPRequestHandler):
        def log_message(self, *a):  # quiet
            pass

        def do_POST(self):
            assert self.path.rstrip("/") == "/embed", self.path
            assert self.headers.get("authorization") == f"Bearer {token}"
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
        out = _run(
            "embed-remote.py",
            {
                "AIMEE_EMBEDDER_URL": url,
                "AIMEE_LLM_AUTH_TOKEN": token,
                "AIMEE_LLM_AUTH_REQUIRED": "1",
            },
            "hello world",
        )
        parsed = json.loads(out)
        assert isinstance(parsed, list) and len(parsed) == 384, f"got {len(parsed)} dims"
        _run_missing_managed_auth("embed-remote.py", "AIMEE_EMBEDDER_URL", url, "hello")
    finally:
        server.shutdown()
    print("  embed-remote.py: ok")


def check_rerank_remote() -> None:
    token = "kb-to-llm-test-token"

    class RerankStub(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_POST(self):
            assert self.path.rstrip("/") == "/rerank", self.path
            assert self.headers.get("authorization") == f"Bearer {token}"
            length = int(self.headers.get("content-length", "0") or "0")
            pairs = json.loads(self.rfile.read(length))
            body = json.dumps([0.75 for _ in pairs]).encode()
            self.send_response(200)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server, url = _serve(RerankStub)
    try:
        out = _run(
            "rerank-remote.py",
            {
                "AIMEE_RERANKER_URL": url,
                "AIMEE_LLM_AUTH_TOKEN": token,
                "AIMEE_LLM_AUTH_REQUIRED": "1",
            },
            '[["query", "candidate"]]',
        )
        assert json.loads(out) == [0.75]
        _run_missing_managed_auth(
            "rerank-remote.py", "AIMEE_RERANKER_URL", url, '[["query", "candidate"]]'
        )
    finally:
        server.shutdown()
    print("  rerank-remote.py: ok")


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


def check_no_baked_endpoint_defaults() -> None:
    """No sidecar may carry a baked-in network address as its endpoint default.

    Three of them shipped `env.setdefault("LLM_ENDPOINT", "http://<a LAN IP>")`.
    Where nothing owned that address, every curator code-extraction job failed
    `No route to host` and the queue never drained — silently, because doc
    extraction used a different path and kept working. Where something DID own
    it, the sidecar would post the user's code and prompts to a stranger's
    machine. embed-remote.py had already removed its equivalent fallback; these
    outlived that cleanup, so pin the rule down here.

    An endpoint must come from the environment (AIMEE_LLM_URL / LLM_ENDPOINT) or
    fail closed. Literal loopback is fine — it addresses only this host.
    """
    host_literal = re.compile(
        r"""setdefault\(\s*["'](?:LLM_ENDPOINT|AIMEE_LLM_URL|LLM_BASE_URL)["']\s*,\s*["']([^"']+)["']"""
    )
    offenders = []
    self_name = Path(__file__).name
    for path in sorted(SCRIPTS.glob("*.py")):
        if path.name == self_name:
            continue  # this file quotes the banned pattern to describe it
        for lineno, line in enumerate(path.read_text().splitlines(), 1):
            m = host_literal.search(line)
            if not m:
                continue
            value = m.group(1)
            if "127.0.0.1" in value or "localhost" in value:
                continue
            offenders.append(f"{path.name}:{lineno}: baked endpoint default {value!r}")

    if offenders:
        for o in offenders:
            print(f"  FAIL {o}")
        raise AssertionError(
            "sidecar endpoints must come from the environment or fail closed; "
            "a baked address either breaks every deployment that does not own it "
            "or leaks user code to whoever does"
        )
    print("  no baked endpoint defaults: ok")


def main() -> int:
    print("sidecar-clients:")
    check_embed_remote()
    check_rerank_remote()
    check_llm_chat()
    check_llm_chat_reasoning_split()
    check_no_baked_endpoint_defaults()
    print("sidecar-clients: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
