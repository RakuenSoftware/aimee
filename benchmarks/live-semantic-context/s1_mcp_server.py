#!/usr/bin/env python3
"""Cell-scoped MCP surface for the live semantic context paired study."""

from __future__ import annotations

import json
import os
from pathlib import Path
import select
import subprocess
import sys
import time
from typing import Any


BASE = Path(__file__).resolve().parent
TOOLS = json.loads((BASE / "tools" / "s1-tool-schemas-v1.json").read_text())
WORKSPACE = Path(os.environ["S1_WORKSPACE"]).resolve()
ARM = os.environ["S1_ARM"]
LOG_PATH = Path(os.environ["S1_TOOL_LOG"])
STARTED = time.monotonic()
MAX_RESULT_BYTES = 32768


def now_ms() -> int:
    return round((time.monotonic() - STARTED) * 1000)


def contained(path: Path) -> bool:
    try:
        path.resolve().relative_to(WORKSPACE)
        return True
    except (OSError, ValueError):
        return False


def checked_path(value: object, *, must_exist: bool = True) -> Path:
    if not isinstance(value, str) or not value or Path(value).is_absolute() or ".." in Path(value).parts:
        raise ValueError("path must be canonical and workspace-relative")
    path = WORKSPACE / value
    resolved = path.resolve(strict=must_exist)
    if not contained(resolved):
        raise ValueError("path is outside the checked worktree")
    return resolved


def logged(name: str, arguments: object, result: dict[str, Any], started: float) -> dict[str, Any]:
    result.setdefault("observed_at_monotonic_ms", now_ms())
    record = {
        "tool": name,
        "arguments": arguments,
        "started_at_monotonic_ms": round((started - STARTED) * 1000),
        "completed_at_monotonic_ms": now_ms(),
        "result": result,
    }
    with LOG_PATH.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n")
    return result


class Bridge:
    def __init__(self) -> None:
        command = [
            os.environ["S1_BRIDGE"],
            os.environ["S1_PROVIDER_COMMAND"],
            os.environ.get("S1_PROVIDER_ARG", "-"),
            os.environ["S1_PROVIDER_EXTENSION"],
        ]
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

    def call(self, request: dict[str, Any]) -> dict[str, Any]:
        if self.process.poll() is not None or not self.process.stdin or not self.process.stdout:
            return {"status": "unavailable", "reason": "semantic provider bridge exited"}
        request = {"workspace": str(WORKSPACE), **request}
        self.process.stdin.write(json.dumps(request, separators=(",", ":")) + "\n")
        self.process.stdin.flush()
        ready, _, _ = select.select([self.process.stdout], [], [], 45)
        if not ready:
            return {"status": "unavailable", "reason": "semantic provider request timed out"}
        line = self.process.stdout.readline()
        if not line:
            detail = self.process.stderr.read(1000) if self.process.stderr else ""
            return {"status": "unavailable", "reason": f"semantic provider exited: {detail}"}
        try:
            parsed = json.loads(line)
        except json.JSONDecodeError:
            return {"status": "unavailable", "reason": "semantic provider returned invalid JSON"}
        return parsed if isinstance(parsed, dict) else {
            "status": "unavailable", "reason": "semantic provider returned a non-object"
        }

    def close(self) -> None:
        if self.process.stdin:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(timeout=5)


BRIDGE = Bridge() if ARM != "production" else None


def tool_definitions() -> list[dict[str, Any]]:
    definitions = []
    for item in TOOLS["non_lsp_tools"] + TOOLS["arm_lsp_tools"][ARM]:
        definitions.append({
            "name": item["name"],
            "description": item["description"],
            "inputSchema": item["parameters"],
        })
    return definitions


def run_search(arguments: dict[str, Any], *, structural: bool) -> dict[str, Any]:
    query = arguments.get("query")
    if not isinstance(query, str) or not query:
        return {"status": "abstained", "reason": "query is required"}
    try:
        target = checked_path(arguments.get("path", "."))
    except ValueError as error:
        return {"status": "unauthorized", "reason": str(error)}
    if structural:
        command = [os.environ["S1_AST_GREP"], "run", "--pattern", query,
                   "--json=stream", str(target)]
    else:
        command = [os.environ["S1_RG"], "-n", "--color", "never", "--no-heading",
                   "--fixed-strings", "--", query, str(target)]
    try:
        completed = subprocess.run(
            command, cwd=WORKSPACE, text=True, capture_output=True, timeout=10, check=False
        )
    except subprocess.TimeoutExpired:
        return {"status": "unavailable", "reason": "search timed out"}
    if completed.returncode not in (0, 1):
        return {"status": "unavailable", "reason": completed.stderr[:1000]}
    output = completed.stdout[:MAX_RESULT_BYTES]
    return {
        "status": "ok" if output else "empty",
        "matches": output,
        "truncated": len(completed.stdout) > len(output),
        "authority": "local_checkout:.",
    }


def file_read(arguments: dict[str, Any]) -> dict[str, Any]:
    try:
        path = checked_path(arguments.get("path"))
        data = path.read_bytes()
    except (OSError, ValueError) as error:
        return {"status": "unauthorized", "reason": str(error)}
    if b"\0" in data:
        return {"status": "unsupported", "reason": "file is not text"}
    text = data.decode("utf-8", errors="replace")
    content = text[:MAX_RESULT_BYTES]
    return {
        "status": "ok",
        "path": str(path.relative_to(WORKSPACE)),
        "content": content,
        "truncated": len(text) > len(content),
        "authority": "local_checkout:.",
    }


def span_get(arguments: dict[str, Any]) -> dict[str, Any]:
    spans = arguments.get("spans")
    if not isinstance(spans, list) or not 1 <= len(spans) <= 16:
        return {"status": "abstained", "reason": "one to sixteen spans are required"}
    results = []
    remaining = MAX_RESULT_BYTES
    for span in spans:
        try:
            path = checked_path(span.get("path"))
            start = int(span.get("start_line"))
            end = int(span.get("end_line"))
            if start < 1 or end < start:
                raise ValueError("line range is invalid")
            lines = path.read_text(errors="replace").splitlines(keepends=True)
        except (OSError, TypeError, ValueError) as error:
            return {"status": "unauthorized", "reason": str(error)}
        content = "".join(lines[start - 1:end])[:remaining]
        remaining -= len(content)
        results.append({
            "path": str(path.relative_to(WORKSPACE)),
            "start_line": start,
            "end_line": min(end, len(lines)),
            "content": content,
        })
    return {"status": "ok", "spans": results, "truncated": remaining == 0,
            "authority": "local_checkout:."}


def lsp_call(name: str, arguments: dict[str, Any]) -> dict[str, Any]:
    if not BRIDGE:
        return {"status": "unsupported", "reason": "this arm has no semantic provider surface"}
    injection = os.environ.get("S1_FAILURE_INJECTION", "")
    expected = os.environ.get("S1_FAILURE_STATUS", "")
    if injection:
        return {"status": expected, "reason": f"checked failure injection: {injection}"}
    if name == "lsp_context":
        return BRIDGE.call({
            "operation": "context",
            "semantic_operation": arguments.get("operation"),
            "anchors": arguments.get("anchors"),
            "max_source_bytes": arguments.get("max_source_bytes", 8192),
        })
    try:
        requested_workspace = Path(str(arguments.get("workspace", ""))).resolve(strict=True)
        requested_file = Path(str(arguments.get("file", ""))).resolve(strict=True)
        if requested_workspace != WORKSPACE or not contained(requested_file):
            raise ValueError("shipping LSP paths do not name the checked worktree")
        relative_file = str(requested_file.relative_to(WORKSPACE))
    except (OSError, ValueError) as error:
        return {"status": "unauthorized", "reason": str(error)}
    anchor = {
        "file": relative_file,
        "line": arguments.get("line"),
        "column": arguments.get("column"),
    }
    return BRIDGE.call({
        "operation": "definition" if name == "lsp_definition" else "references",
        "anchors": [anchor],
    })


def dispatch(name: str, arguments: object) -> dict[str, Any]:
    started = time.monotonic()
    args = arguments if isinstance(arguments, dict) else {}
    if name == "local_text_search":
        result = run_search(args, structural=False)
    elif name == "local_structure_search":
        result = run_search(args, structural=True)
    elif name == "file_read":
        result = file_read(args)
    elif name == "code_span_get":
        result = span_get(args)
    elif name in ("file_edit", "test_execution"):
        result = {"status": "unsupported", "reason": "the checked corpus is read-only"}
    elif name in ("lsp_definition", "lsp_references", "lsp_context"):
        result = lsp_call(name, args)
    else:
        result = {"status": "unsupported", "reason": "tool is not on this arm's frozen surface"}
    return logged(name, args, result, started)


def send(message: dict[str, Any]) -> None:
    sys.stdout.write(json.dumps(message, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def main() -> int:
    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    try:
        for line in sys.stdin:
            try:
                request = json.loads(line)
            except json.JSONDecodeError:
                continue
            method = request.get("method")
            request_id = request.get("id")
            if method == "initialize":
                send({
                    "jsonrpc": "2.0", "id": request_id,
                    "result": {
                        "protocolVersion": "2024-11-05",
                        "capabilities": {"tools": {"listChanged": False}},
                        "serverInfo": {"name": "aimee-s1-study", "version": "1"},
                    },
                })
            elif method == "tools/list":
                send({"jsonrpc": "2.0", "id": request_id,
                      "result": {"tools": tool_definitions()}})
            elif method == "tools/call":
                params = request.get("params") or {}
                result = dispatch(params.get("name", ""), params.get("arguments"))
                send({
                    "jsonrpc": "2.0", "id": request_id,
                    "result": {
                        "content": [{"type": "text", "text": json.dumps(result, sort_keys=True)}],
                        "structuredContent": result,
                        "isError": result.get("status") in {
                            "unavailable", "unauthorized", "unsupported", "stale", "abstained"
                        },
                    },
                })
            elif request_id is not None:
                send({"jsonrpc": "2.0", "id": request_id,
                      "error": {"code": -32601, "message": "method not found"}})
    finally:
        if BRIDGE:
            BRIDGE.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
