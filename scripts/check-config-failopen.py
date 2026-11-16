#!/usr/bin/env python3
"""Security enforcement accessors must seed their safe state before a read."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
text = (root / "src/headers/config_accessor_keys.def").read_text()
# Every security gate native code reads through the config bus. The thin
# client reads require_aimee_memory from its own config file, and no native
# code reads delegate_sandbox_require_isolation, cross_verify or
# skills_eval_gate_enabled, so their bus accessors are gone.
required_on = (
    "config_require_session_worktree", "config_require_aimee_git", "config_integrity_enabled",
    "config_roundtable_replay_verify_enabled", "config_mcp_osv_enabled",
    "config_computer_use_redact_sensitive_screenshots", "config_subagent_ban_enabled",
)
bad = []
for name in required_on:
    key = name.removeprefix("config_")
    match = re.search(rf'^CONFIG_NUMBER\({key},\s*int,\s*"{key}",\s*1\)$', text, re.M)
    if not match:
        bad.append(name)
if bad:
    print("config-failopen-check: unsafe/missing accessors: " + ", ".join(bad), file=sys.stderr)
    raise SystemExit(1)
print("config-failopen-check: security accessors default to their safe state")
