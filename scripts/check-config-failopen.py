#!/usr/bin/env python3
"""Security enforcement accessors must seed their safe state before a read."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
text = "\n".join(p.read_text() for p in sorted((root / "src").glob("config_client_accessors_*.c")))
required_on = (
    "config_require_session_worktree", "config_require_aimee_git",
    "config_delegate_sandbox_require_isolation", "config_integrity_enabled",
    "config_cross_verify", "config_roundtable_replay_verify_enabled",
    "config_mcp_osv_enabled", "config_computer_use_redact_sensitive_screenshots",
    "config_skills_eval_gate_enabled", "config_subagent_ban_enabled",
)
bad = []
for name in required_on:
    match = re.search(rf"int\s+{name}\s*\([^)]*\)\s*\{{(.*?)\n\}}", text, re.S)
    if not match or not re.search(r"double\s+value\s*=\s*1\s*;", match.group(1)):
        bad.append(name)
if bad:
    print("config-failopen-check: unsafe/missing accessors: " + ", ".join(bad), file=sys.stderr)
    raise SystemExit(1)
print("config-failopen-check: security accessors default to their safe state")
