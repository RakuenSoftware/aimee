#!/usr/bin/env python3
"""Run explicit routing acceptance cases against a configured Aimee server.

This gate makes real delegate calls. The case file supplies request context and
expected selections; it never edits the provider roster or sends credentials to
anywhere other than the configured server.
"""
import argparse
import json
import os
from pathlib import Path
import ssl
import time
import urllib.error
import urllib.request


def result_object(body):
    """Accept a JSON result or the final result in a foreground SSE response."""
    try:
        value = json.loads(body)
        if isinstance(value, dict):
            return value
    except json.JSONDecodeError:
        pass
    results = []
    for line in body.splitlines():
        if line.startswith("data:"):
            try:
                value = json.loads(line[5:].strip())
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict):
                results.append(value)
    for value in reversed(results):
        if "agent" in value or value.get("status") == "error":
            return value
    raise ValueError("server returned no delegate result")


def await_result(post, result, timeout=180):
    """The public /v1 route queues a durable job; pending is not a success."""
    job_id = result.get("job_id")
    if not job_id or result.get("status") != "ok":
        return result
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status = post("/v1/jobs/status", {"job_id": job_id})
        state = status.get("job_status")
        job = status.get("job", {})
        if state in ("done", "failed", "cancelled", "partial"):
            return {"status": "ok" if state == "done" else "error",
                    "agent": job.get("agent_name"), "message": job.get("result", ""),
                    "job_id": job_id}
        if status.get("status") != "ok" or state == "not_found":
            raise RuntimeError(f"delegate job {job_id} unavailable")
        time.sleep(0.25)
    raise TimeoutError(f"delegate job {job_id} did not finish in {timeout}s")


def run_cases(url, cases, context, token=""):
    if not isinstance(cases, list) or not cases:
        raise ValueError("at least one explicit acceptance case is required")
    records = []
    for case in cases:
        request = case["request"]
        expected_agent = case.get("expected_agent")
        expected_error = case.get("expected_error")
        if bool(expected_agent) == bool(expected_error):
            raise ValueError("each case needs exactly one expected_agent or expected_error")
        if expected_agent and any(k in request for k in ("via", "provider", "model", "tier")):
            raise ValueError("selection acceptance must exercise automatic routing")
        headers = {"Content-Type": "application/json"}
        if token:
            headers["Authorization"] = "Bearer " + token
        def post(path, payload):
            req = urllib.request.Request(url.rstrip("/") + path,
                                         json.dumps(payload).encode(), headers)
            try:
                with urllib.request.urlopen(req, context=context, timeout=180) as response:
                    body = response.read(2 * 1024 * 1024).decode()
            except urllib.error.HTTPError as exc:
                # Authentication or transport failure is never routing evidence.
                if exc.code in (401, 403) or exc.code >= 500:
                    raise RuntimeError(f"server unavailable for case {case['name']}: HTTP {exc.code}") from None
                body = exc.read(2 * 1024 * 1024).decode()
            return result_object(body)

        result = await_result(post, post("/v1/delegate/run", request))
        if expected_agent:
            if result.get("status") != "ok" or result.get("agent") != expected_agent:
                raise AssertionError(f"{case['name']}: expected agent {expected_agent}; got {result.get('agent')} / {result.get('status')}")
        else:
            error = str(result.get("error", result.get("message", "")))
            if result.get("status") != "error" or result.get("agent") or expected_error not in error:
                raise AssertionError(f"{case['name']}: expected routing refusal {expected_error!r}")
        records.append({"case": case["name"], "agent": result.get("agent"), "passed": True})
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cases", type=Path)
    parser.add_argument("--url", default=os.environ.get("AIMEE_API"))
    parser.add_argument("--ca", default=os.environ.get("AIMEE_CA"))
    parser.add_argument("--cert", default=os.environ.get("AIMEE_CLIENT_CERT"))
    parser.add_argument("--key", default=os.environ.get("AIMEE_CLIENT_KEY"))
    args = parser.parse_args()
    if not args.url:
        parser.error("--url or AIMEE_API is required")
    context = ssl.create_default_context(cafile=args.ca)
    if args.cert:
        context.load_cert_chain(args.cert, args.key)
    print(json.dumps(run_cases(args.url, json.loads(args.cases.read_text()), context,
                               os.environ.get("AIMEE_TOKEN", "")), indent=2))


if __name__ == "__main__":
    main()
