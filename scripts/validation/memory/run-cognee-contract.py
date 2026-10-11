#!/usr/bin/env python3
"""Run the real Cognee contract against disposable storage and local model fixtures.

Install cognee[api]==1.6.2 into the selected Python environment first. No model
service or external credentials are required. Artifacts contain fixture data only.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class ModelFixture(BaseHTTPRequestHandler):
    calls = {"embeddings": 0, "completions": 0}
    commands = {}
    completion_max_tokens = 0

    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.path.endswith("/embeddings"):
            ModelFixture.calls["embeddings"] += 1
            texts = body.get("input", [])
            if isinstance(texts, str):
                texts = [texts]
            result = {"object": "list", "model": "fixture", "data": [
                {"object": "embedding", "index": i, "embedding": [
                    1., float("needle" in str(text).lower()),
                    float("other" in str(text).lower()), .1, .2, .3, .4, .5]}
                for i, text in enumerate(texts)],
                "usage": {"prompt_tokens": 1, "total_tokens": 1}}
        else:
            ModelFixture.calls["completions"] += 1
            content = json.dumps({"nodes": [], "edges": [], "summary": "needle canonical"})
            result = {"id": "fixture", "object": "chat.completion", "created": 1,
                      "model": "fixture", "choices": [{"index": 0,
                      "message": {"role": "assistant", "content": content},
                      "finish_reason": "stop"}], "usage": {
                      "prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2}}
        if ModelFixture.commands and not self.path.endswith("/embeddings") and ModelFixture.completion_max_tokens:
            body["max_tokens"] = ModelFixture.completion_max_tokens
        operation = "embeddings" if self.path.endswith("/embeddings") else "completions"
        if ModelFixture.commands:
            try:
                answer = subprocess.run(ModelFixture.commands[operation], input=json.dumps(body),
                    capture_output=True, text=True, timeout=190, check=True)
                result = json.loads(answer.stdout)
                if "error" in result: raise RuntimeError("model unavailable")
            except Exception:
                self.send_error(502, "real model request failed")
                return
        raw = json.dumps(result).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--go", default="go")
    parser.add_argument("--artifacts", required=True)
    parser.add_argument("--completion-command", help="JSON argv for real inference, stdin OpenAI request/stdout response")
    parser.add_argument("--embedding-command", help="JSON argv for real embedding, stdin OpenAI request/stdout response")
    parser.add_argument("--request-deadline", type=int, default=0, help="optional diagnostic timeout in seconds, at most 600")
    parser.add_argument("--completion-max-tokens", type=int, default=0)
    parser.add_argument("--model", default="openai/fixture")
    parser.add_argument("--embedding-model", default="text-embedding-3-small")
    parser.add_argument("--embedding-dimensions", type=int, default=8)
    args = parser.parse_args()
    if bool(args.completion_command) != bool(args.embedding_command):
        parser.error("both real model commands are required")
    ModelFixture.completion_max_tokens = args.completion_max_tokens
    if args.completion_command:
        for key, value in (("completions", args.completion_command), ("embeddings", args.embedding_command)):
            command = json.loads(value)
            if not isinstance(command, list) or not command or not all(isinstance(x, str) for x in command):
                parser.error("model command must be JSON argv")
            ModelFixture.commands[key] = command
    if args.request_deadline < 0 or args.request_deadline > 600:
        parser.error("request deadline must be between zero and 600 seconds")
    work = Path(args.artifacts).resolve()
    # Refuse to reuse a stateful catalog: stale fixture datasets cannot make a
    # fresh contract test pass. The caller retains/removes its own artifacts.
    work.mkdir(parents=True, exist_ok=True)
    if any((work / name).exists() for name in ("data", "system", "server.log")):
        parser.error("use a fresh artifact directory for each run")
    model = ThreadingHTTPServer(("127.0.0.1", 0), ModelFixture)
    thread = threading.Thread(target=model.serve_forever, daemon=True)
    thread.start()
    # Reserve an available API port; uvicorn exclusively serves the test account.
    import socket
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        api_port = reservation.getsockname()[1]
    model_url = f"http://127.0.0.1:{model.server_port}/v1"
    api_url = f"http://127.0.0.1:{api_port}"
    env = {key: value for key, value in os.environ.items()
           if not any(part in key for part in ("API_KEY", "TOKEN", "PASSWORD", "SECRET"))}
    env.update(DATA_ROOT_DIRECTORY=str(work / "data"),
               SYSTEM_ROOT_DIRECTORY=str(work / "system"),
               CACHE_ROOT_DIRECTORY=str(work / "cache"),
               COGNEE_LOGS_DIR=str(work / "logs"), COGNEE_REPOS_DIR=str(work / "repos"),
               DEFAULT_USER_PASSWORD="contract-fixture-password", LLM_PROVIDER="custom",
               LLM_MODEL=args.model, LLM_ENDPOINT=model_url, LLM_API_KEY="fixture",
               EMBEDDING_PROVIDER="openai", EMBEDDING_MODEL=args.embedding_model,
               EMBEDDING_ENDPOINT=model_url, EMBEDDING_API_KEY="fixture",
               EMBEDDING_DIMENSIONS=str(args.embedding_dimensions), COGNEE_TRACING_ENABLED="false",
               TELEMETRY_DISABLED="true")
    server = None
    try:
        with (work / "server.log").open("w") as log:
            server = subprocess.Popen([args.python, "-m", "uvicorn", "service:app", "--app-dir", str(Path(__file__).resolve().parents[3] / "integrations/cognee"),
                                       "--host", "127.0.0.1", "--port", str(api_port)],
                                      cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline:
                if server.poll() is not None:
                    raise RuntimeError("Cognee exited during startup; inspect server.log")
                try:
                    with urllib.request.urlopen(api_url + "/openapi.json", timeout=1):
                        break
                except (OSError, ValueError):
                    time.sleep(.5)
            else:
                raise RuntimeError("Cognee startup timed out; inspect server.log")
            test_env = os.environ.copy()
            test_env.update(AIMEE_COGNEE_TEST_URL=api_url,
                            AIMEE_COGNEE_TEST_USER="default_user@example.com",
                            AIMEE_COGNEE_TEST_PASSWORD="contract-fixture-password")
            if args.request_deadline:
                test_env["AIMEE_COGNEE_TEST_DEADLINE"] = str(args.request_deadline) + "s"
            root = Path(__file__).resolve().parents[3]
            result = subprocess.run([args.go, "test", "-race", "-v",
                                     "./modules/memory/cognee", "-run",
                                     "^TestCogneeLiveContract$", "-count=1"],
                                    cwd=root / "server-go", env=test_env,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, timeout=max(300, args.request_deadline+60))
            (work / "contract.log").write_text(result.stdout)
            print(result.stdout, end="", flush=True)
            if result.returncode or "--- PASS: TestCogneeLiveContract" not in result.stdout:
                raise RuntimeError("required real Cognee contract failed or skipped")
            if not ModelFixture.calls["embeddings"] or ModelFixture.calls["completions"]:
                raise RuntimeError("chunk contract must embed sources without graph-completion calls")
            (work / "summary.json").write_text(json.dumps({
                "status": "passed", "cognee": "1.6.2", "models": "real inference and embeddings" if ModelFixture.commands else "deterministic local fixtures",
                "diagnostic_request_deadline_seconds": args.request_deadline or None, "completion_max_tokens": args.completion_max_tokens or None,
                "completion_model": args.model, "embedding_model": args.embedding_model, "embedding_dimensions": args.embedding_dimensions,
                "fixture_calls": ModelFixture.calls}, indent=2))
    finally:
        if server is not None:
            server.terminate()
            try:
                server.wait(timeout=15)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
        model.shutdown()
        model.server_close()
        thread.join(timeout=5)


if __name__ == "__main__":
    main()
