#!/usr/bin/env python3
"""Required real Hillock HDC contract, pinned by default or to an exact CI revision."""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

REVISION = "1edd166ead75b85a9ab95cd6ba4faf7011ad567c"
REMOTE = "https://github.com/roandejager/Hillock.git"


def prepare_source(work, revision, existing=None):
    source = Path(existing).resolve() if existing else work / "upstream"
    if not existing:
        subprocess.run(["git", "init", "--quiet", str(source)], check=True, timeout=30)
        subprocess.run(["git", "-C", str(source), "remote", "add", "origin", REMOTE], check=True, timeout=30)
        subprocess.run(["git", "-C", str(source), "fetch", "--depth=1", "origin", revision], check=True, timeout=120)
        subprocess.run(["git", "-C", str(source), "checkout", "--detach", "FETCH_HEAD"], check=True, timeout=30)
    actual = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True, timeout=30).strip()
    if actual != revision:
        raise RuntimeError("upstream revision mismatch")
    if subprocess.check_output(["git", "-C", str(source), "diff", "--name-only", "HEAD"], text=True, timeout=30).strip():
        raise RuntimeError("modified upstream source")
    marker = source / "AIMEE_UPSTREAM_REVISION"
    if subprocess.check_output(["git", "-C", str(source), "ls-files", "AIMEE_UPSTREAM_REVISION"], text=True, timeout=30).strip():
        raise RuntimeError("upstream owns the qualification revision marker")
    marker.write_text(revision + "\n")
    return source


def qualification_profile(root, work, revision):
    # Only the expected revision changes. The actual upstream ranking code and
    # every Aimee protocol, authority and lifecycle check remain under test.
    # Keep production source and its supported revision pin unchanged.
    service = root / "integrations/hillock/service.py"
    if revision == REVISION:
        return service, []
    profile = work / "profile"
    profile.mkdir()
    text = service.read_text()
    pin = f'UPSTREAM_REVISION = "{REVISION}"'
    if text.count(pin) != 1:
        raise RuntimeError("Hillock service pin changed; update qualification profile")
    candidate_service = profile / "service.py"
    candidate_service.write_text(text.replace(pin, f'UPSTREAM_REVISION = "{revision}"'))
    adapter = root / "server-go/modules/memory/hillock/hillock.go"
    text = adapter.read_text()
    pin = f'const UpstreamRevision = "{REVISION}"'
    if text.count(pin) != 1:
        raise RuntimeError("Hillock adapter pin changed; update qualification profile")
    candidate_adapter = profile / "hillock.go"
    candidate_adapter.write_text(text.replace(pin, f'const UpstreamRevision = "{revision}"'))
    overlay = profile / "overlay.json"
    overlay.write_text(json.dumps({"Replace": {str(adapter): str(candidate_adapter)}}) + "\n")
    return candidate_service, ["-overlay=" + str(overlay)]


def require_live_tests(result, names):
    # An absent, skipped or failing required scenario cannot produce a receipt.
    if result.returncode or re.search(r"(?:SKIP|FAIL)", result.stdout):
        raise RuntimeError("required real Hillock/Aimee tests failed; inspect contract.log")
    for name in names:
        if not re.search(r"^--- PASS: " + re.escape(name) + r" \([^\n]*\)$", result.stdout, re.MULTILINE):
            raise RuntimeError("missing required live scenario " + name + "; inspect contract.log")


def validate(root, work, source, revision, go, scale):
    service, go_flags = qualification_profile(root, work, revision)
    token = work / "token"
    token.write_text("contract-fixture\n")
    token.chmod(0o600)
    try:
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
                server = subprocess.Popen([sys.executable, str(service)], env=env, stdout=log, stderr=subprocess.STDOUT)
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
                    if health.get("upstream_revision") != revision or not health.get("stateless"):
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
                    result = subprocess.run([go, "test", *go_flags, "-race", "-count=1", "-v", "./modules/memory/hillock", "-run", "^TestHillockLiveContract$"], cwd=root / "server-go", env=test_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
                    contract.write(result.stdout)
                    contract.flush()
                    require_live_tests(result, ("TestHillockLiveContract",))
                    # These construct Aimee's production external catalog/owner and
                    # use its public command/admission/recall boundaries with the
                    # real candidate service, not a fake ranking provider.
                    owner = subprocess.run([go, "test", *go_flags, "-race", "-count=1", "-v", "./modules/memory",
                                            "-run", "^TestHillockLive(Owner|Recall)Scenarios$"],
                                           cwd=root / "server-go", env=test_env, stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT, text=True, timeout=180)
                    contract.write(owner.stdout)
                    contract.flush()
                    required = ("TestHillockLiveOwnerScenarios", "TestHillockLiveRecallScenarios")
                    require_live_tests(owner, required)
                    governed = subprocess.run([go, "test", *go_flags, "-race", "-count=1", "-v", "./modules/egress",
                                               "-run", "^TestHillockLiveGovernedTransport$"],
                                              cwd=root / "server-go", env=test_env, stdout=subprocess.PIPE,
                                              stderr=subprocess.STDOUT, text=True, timeout=120)
                    contract.write(governed.stdout)
                    contract.flush()
                    require_live_tests(governed, ("TestHillockLiveGovernedTransport",))
                    if scale and round_number == 0:
                        with (work / "scale.log").open("w") as scale_log:
                            subprocess.run([go, "run", *go_flags, "../scripts/validation/memory/qualify-external.go",
                                            "-backend", "hillock", "-endpoint", endpoint,
                                            "-token-file", str(token), "-deadline", "30s",
                                            "-output", str(work / "scale.json")],
                                           cwd=root / "server-go", env=test_env, stdout=scale_log,
                                           stderr=subprocess.STDOUT, check=True, timeout=300)
                    results.append({"round": round_number, "passed": True})
                finally:
                    server.terminate()
                    try:
                        server.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait()
        return {"restart_rounds": results, "http_auth_validation_capacity": "passed", "contract": "passed", "aimee_owner_scenarios": "passed", "governed_transport": "passed",
                "scale": "passed" if scale else "not_requested"}
    finally:
        token.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", required=True)
    parser.add_argument("--go", default="go")
    parser.add_argument("--source", help="existing upstream checkout; revision is verified")
    parser.add_argument("--revision", default=REVISION, help="exact 40-character upstream commit; production pin remains unchanged")
    parser.add_argument("--branch", default="", help="discovered upstream branch, recorded in evidence")
    parser.add_argument("--scale", action="store_true", help="qualify 100/1000/10000-record cold and concurrent warm retrieval")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}", args.revision) or args.revision == "0" * 40:
        parser.error("revision must be an exact nonzero lowercase 40-character commit SHA")
    root = Path(__file__).resolve().parents[3]
    work = Path(args.artifacts).resolve()
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        parser.error("use an empty artifact directory")
    summary = {"status": "running", "upstream_repository": REMOTE,
               "upstream_branch": args.branch, "upstream_revision": args.revision,
               "aimee_revision": subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip(),
               "profile": "hdc-subword", "real_engine": True}
    try:
        source = prepare_source(work, args.revision, args.source)
        summary.update(validate(root, work, source, args.revision, args.go, args.scale))
        summary["status"] = "passed"
    except Exception as error:
        summary.update(status="failed", error=str(error))
        raise
    finally:
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary), flush=True)


if __name__ == "__main__":
    main()
