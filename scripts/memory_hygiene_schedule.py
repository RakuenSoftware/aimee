#!/usr/bin/env python3
"""Opt-in, bounded scheduler tick for proposal-only memory hygiene.

Invoke explicitly or from an operator-owned timer. No schedule is installed or
activated by this script. Server-owned jobs and proposals provide retry identity;
this local cursor contains no canonical content or credentials.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time


def save_state(path, value):
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as out:
            json.dump(value, out, sort_keys=True)
            out.write("\n")
            out.flush()
            os.fsync(out.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def tick(cli, scope, state_path, max_pages=4, max_seconds=30, restart=False):
    if not 1 <= max_pages <= 16 or not 1 <= max_seconds <= 120:
        raise ValueError("scheduler bounds exceeded")
    if ":" not in scope or len(scope) > 1024:
        raise ValueError("explicit authorized scope required")
    state_path = Path(state_path)
    state_path.parent.mkdir(parents=True, exist_ok=True)
    binding = hashlib.sha256(json.dumps([str(cli), scope]).encode()).hexdigest()
    with open(str(state_path) + ".lock", "a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return {"status": "busy", "pages": 0}
        state = json.loads(state_path.read_text()) if state_path.exists() else {}
        if state and state.get("binding") != binding:
            raise ValueError("state belongs to another CLI/scope")
        cursor = "" if restart else state.get("cursor", "")
        deadline = time.monotonic() + max_seconds
        pages = 0
        while pages < max_pages and time.monotonic() < deadline:
            args = [str(cli), "memory", "hygiene", "--scope", scope,
                    "--max-rows", "64", "--max-content-bytes", "32768", "--json"]
            if cursor:
                args += ["--cursor", cursor]
            reply = subprocess.run(args, capture_output=True, text=True,
                                   timeout=max(0.1, deadline - time.monotonic()), check=False)
            if reply.returncode:
                raise RuntimeError("hygiene command failed; cursor preserved")
            result = json.loads(reply.stdout)
            if result.get("status") != "ok" or result.get("dry_run") is not False or result.get("canonical_writes") != 0 or not result.get("job_id"):
                raise RuntimeError("invalid proposal-only job receipt; cursor preserved")
            next_cursor = result.get("resume_cursor", "")
            if not isinstance(next_cursor, str) or len(next_cursor) > 4096:
                raise RuntimeError("invalid resume cursor")
            pages += 1
            partial = result.get("partial") is True
            status = "partial" if partial else "complete"
            if partial and (not next_cursor or next_cursor == cursor):
                status = "partial_without_progress"
            state = {"schema_version": 1, "binding": binding, "cursor": next_cursor,
                     "status": status, "last_job_id": result["job_id"],
                     "last_run_id": result.get("run_id"), "policy": result.get("policy"),
                     "unvisited": result.get("unvisited")}
            save_state(state_path, state)
            cursor = next_cursor
            if status != "partial":
                break
        return {"status": state.get("status", "budget_exhausted"), "pages": pages,
                "resume_available": bool(cursor), "canonical_writes": 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--scope", required=True)
    parser.add_argument("--state", required=True, type=Path)
    parser.add_argument("--max-pages", type=int, default=4)
    parser.add_argument("--max-seconds", type=int, default=30)
    parser.add_argument("--restart", action="store_true")
    args = parser.parse_args()
    try:
        result = tick(args.cli, args.scope, args.state, args.max_pages, args.max_seconds, args.restart)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired):
        print(json.dumps({"status": "unavailable", "reason": "tick_failed_cursor_preserved"}))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 1 if result["status"] == "partial_without_progress" else 0


if __name__ == "__main__":
    raise SystemExit(main())
