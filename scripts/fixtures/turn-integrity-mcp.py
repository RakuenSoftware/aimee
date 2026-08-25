#!/usr/bin/env python3
"""Deterministic stdio MCP peer for external-mutation timeout validation."""

from __future__ import annotations

import json
import sys
import time


TOOLS = [
    {
        "name": "mutate",
        "description": "A non-idempotent external mutation that never acknowledges.",
        "inputSchema": {
            "type": "object",
            "properties": {"request": {"type": "string"}},
            "required": ["request"],
        },
    }
]


for line in sys.stdin:
    try:
        request = json.loads(line)
    except json.JSONDecodeError:
        continue
    method = request.get("method")
    request_id = request.get("id")
    if method == "initialize":
        result = {"protocolVersion": "2024-11-05"}
    elif method == "tools/list":
        result = {"tools": TOOLS}
    elif method == "tools/call":
        # Deliberately model the dangerous ambiguity: the peer received the
        # mutation request, but the caller never receives an acknowledgement.
        time.sleep(3600)
        continue
    else:
        result = None
    if result is None:
        reply = {
            "jsonrpc": "2.0",
            "id": request_id,
            "error": {"code": -32601, "message": "method not found"},
        }
    else:
        reply = {"jsonrpc": "2.0", "id": request_id, "result": result}
    sys.stdout.write(json.dumps(reply, separators=(",", ":")) + "\n")
    sys.stdout.flush()
