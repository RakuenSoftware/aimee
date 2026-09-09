#!/usr/bin/env python3
"""Read-only release checks against a configured, populated Aimee thin client.

Example: python3 scripts/test-private-index-live.py --client /path/to/aimee \
  --sample aimee=memory_insert --sample games-on-whales/wolf=die \
  --memory-key aimee-deployment --min-projects 2 --output /tmp/index-checks.json
No credentials, source contents, or memory contents are written to the report.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", default="aimee")
    parser.add_argument("--sample", action="append", required=True, metavar="PROJECT=SYMBOL")
    parser.add_argument("--memory-key", required=True)
    parser.add_argument("--min-projects", type=int, default=1)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    results = []

    def call(command, allow_error=False):
        start = time.monotonic()
        proc = subprocess.run([args.client, "--json", *command], text=True,
                              capture_output=True, timeout=45)
        elapsed = round(time.monotonic() - start, 3)
        if proc.returncode:
            raise AssertionError(f"{command}: CLI exit {proc.returncode}")
        try:
            reply = json.loads(proc.stdout)
        except ValueError as exc:
            raise AssertionError(f"{command}: non-JSON reply") from exc
        if isinstance(reply, dict) and "span" in reply:
            reply = reply["span"]
        if isinstance(reply, dict) and not allow_error:
            assert not reply.get("error") and reply.get("status") != "error", command
        return reply, {"command": command, "seconds": elapsed, "passed": True}

    overview, record = call(["index", "overview"])
    assert isinstance(overview, list) and len(overview) >= args.min_projects
    record["projects"] = len(overview)
    results.append(record)
    for sample in args.sample:
        project, symbol = sample.split("=", 1)
        reply, record = call(["index", "find", symbol, "--project", project])
        hits = reply["hits"]
        assert hits and all(hit["project"] == project for hit in hits), sample
        results.append(record)
        path = hits[0]["file_path"]
        line = max(1, hits[0].get("line", 1))
        reply, record = call(["index", "structure", path, "--project", project])
        assert any(d["name"].lower() == symbol.lower() for d in reply["definitions"]), sample
        results.append(record)
        reply, record = call(["index", "span", path, str(line), str(line + 5), "--project", project])
        assert reply["content"] and reply["line_count"] > 0, sample
        assert re.fullmatch(r"[0-9a-f]{64}", reply["source_version"]), sample
        results.append(record)
        reply, record = call(["index", "callers", symbol, "--project", project])
        assert isinstance(reply["hits"], list), sample
        results.append(record)
        reply, record = call(["index", "blast-radius", path, "--project", project])
        assert reply.get("result_status") == "ok" and reply.get("file") == path, sample
        assert isinstance(reply.get("dependencies"), list) and isinstance(reply.get("dependents"), list)
        results.append(record)
        for operation in ("hybrid", "investigate"):
            reply, record = call(["index", operation, symbol, "--project", project])
            assert reply.get("hits") or reply.get("results"), (sample, operation)
            results.append(record)
        for path in (".env", "../outside.c", "/etc/passwd", "__aimee_missing_source__.c"):
            reply, record = call(["index", "span", path, "1", "2", "--project", project], True)
            assert isinstance(reply.get("error"), str) and reply["error"], (sample, path)
            results.append(record)
        reply, record = call(["index", "structure", "__aimee_missing_source__.c", "--project", project])
        assert reply["definitions"] == [], sample
        results.append(record)
    reply, record = call(["memory", "search", args.memory_key])
    assert args.memory_key in json.dumps(reply), "existing memory was not retrieved"
    results.append(record)
    # Exercise shared request capacity while detached workspaces are registered.
    command = ["memory", "search", args.memory_key]
    with ThreadPoolExecutor(max_workers=8) as pool:
        for reply, record in pool.map(lambda _: call(command), range(16)):
            assert args.memory_key in json.dumps(reply)
            record["concurrent"] = True
            results.append(record)
    args.output.write_text(json.dumps(results, indent=2) + "\n")
    print(f"private index live: {len(results)} checks passed across {len(args.sample)} projects")


if __name__ == "__main__":
    main()
