#!/usr/bin/env python3
"""Required real Hillock HDC contract, using a pinned upstream checkout and no LLM."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

REVISION = "1edd166ead75b85a9ab95cd6ba4faf7011ad567c"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", required=True)
    parser.add_argument("--go", default="go")
    parser.add_argument("--source", help="existing upstream checkout; revision is verified")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    work = Path(args.artifacts).resolve()
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        parser.error("use an empty artifact directory")
    source = Path(args.source).resolve() if args.source else work / "upstream"
    if not args.source:
        subprocess.run(["git", "clone", "https://github.com/roandejager/Hillock.git", str(source)], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["git", "-C", str(source), "checkout", "--detach", REVISION], check=True, stdout=subprocess.DEVNULL)
    actual = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if actual != REVISION:
        raise RuntimeError("upstream revision mismatch")
    if subprocess.check_output(["git", "-C", str(source), "diff", "--name-only", "HEAD"], text=True).strip():
        raise RuntimeError("modified upstream source")
    (source / "AIMEE_UPSTREAM_REVISION").write_text(REVISION + "\n")
    token = work / "token"
    token.write_text("contract-fixture\n")
    token.chmod(0o600)
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    endpoint = f"http://127.0.0.1:{port}"
    env = os.environ.copy()
    env.update(HILLOCK_SOURCE=str(source), HILLOCK_TOKEN_FILE=str(token), HILLOCK_HOST="127.0.0.1", HILLOCK_PORT=str(port))

    def request(path, body=None, authenticated=True):
        headers = {"Content-Type": "application/json"}
        if authenticated:
            headers["Authorization"] = "Bearer contract-fixture"
        req = urllib.request.Request(endpoint + path, data=None if body is None else json.dumps(body).encode(), headers=headers)
        return urllib.request.urlopen(req, timeout=20)

    def denied(path, body, status, authenticated=True):
        try:
            with request(path, body, authenticated):
                raise RuntimeError("invalid request admitted")
        except urllib.error.HTTPError as error:
            if error.code != status:
                raise RuntimeError(f"wrong rejection status {error.code}, expected {status}") from error

    results = []
    with (work / "server.log").open("w") as log, (work / "contract.log").open("w") as contract:
        for round_number in range(2):
            server = subprocess.Popen([sys.executable, str(root / "integrations/hillock/service.py")], env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 20
                while True:
                    if server.poll() is not None:
                        raise RuntimeError("Hillock exited; inspect server.log")
                    try:
                        with request("/v1/health") as response:
                            health = json.load(response)
                        break
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise RuntimeError("Hillock startup timeout")
                        time.sleep(.1)
                if health.get("upstream_revision") != REVISION or not health.get("stateless"):
                    raise RuntimeError("wrong engine/profile")
                denied("/v1/health", None, 401, False)
                denied("/v1/rank", {"query": "height", "candidates": [], "limit": 1}, 401, False)
                denied("/v1/chat/completions", {}, 404)
                denied("/v1/rank", {"query": "height", "candidates": [], "limit": 257}, 413)
                denied("/v1/rank", {"query": "height", "candidates": [{"id": True, "revision": "0" * 64, "text": "height"}], "limit": 1}, 400)
                denied("/v1/rank", {"query": "height", "candidates": [{"id": 1, "revision": "0" * 64, "text": "word " * 257}], "limit": 1}, 413)
                denied("/v1/rank", {"query": "x" * 129, "candidates": [], "limit": 1}, 413)
                test_env = os.environ.copy()
                test_env["AIMEE_HILLOCK_TEST_URL"] = endpoint
                result = subprocess.run([args.go, "test", "-race", "-count=1", "-v", "./modules/memory/hillock", "-run", "^TestHillockLiveContract$"], cwd=root / "server-go", env=test_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
                contract.write(result.stdout)
                contract.flush()
                if result.returncode or "--- PASS: TestHillockLiveContract" not in result.stdout or "SKIP" in result.stdout:
                    raise RuntimeError("required real Hillock contract failed; inspect contract.log")
                results.append({"round": round_number, "passed": True})
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
    summary = {"upstream_revision": REVISION, "profile": "hdc-subword", "real_engine": True, "restart_rounds": results, "http_auth_validation_capacity": "passed", "contract": "passed"}
    (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
