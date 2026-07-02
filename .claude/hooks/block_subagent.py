#!/usr/bin/env python3
"""PreToolUse hook: hard-ban Claude Code SUB-AGENTS in the aimee repo.

Root cause this closes: aimee's own sub-agent ban lives in the runtime gateway
(src/gateway_policy.c: gateway_policy_strip_tools -> is_subagent_tool_name, which
canonicalizes Task/Agent/spawn_agent -> "Subagent" and strips them). That ban only
sees tools flowing THROUGH aimee's /v1 gateway to model providers, so it governs
aimee's OWN agents. Claude Code's harness-level Agent/Task tool never traverses that
gateway, so nothing at aimee's layer can strip it. This hook enforces the same
policy at the Claude Code layer: any attempt to spawn a sub-agent is BLOCKED and
redirected to aimee delegates.

Wire-up (settings.json PreToolUse):
  matcher: "Agent|Task|Subagent|spawn_agent"
  command: <this file>

Exit 2 from a PreToolUse hook blocks the tool call and feeds stderr back to the
model, so the redirect message below is what the model sees instead of a sub-agent.
"""
import json
import sys

BANNED = {"agent", "task", "subagent", "spawn_agent"}

try:
    data = json.load(sys.stdin)
except Exception:
    # Fail OPEN on malformed hook input: never wedge the session on a parse error.
    sys.exit(0)

tool = str(data.get("tool_name") or "").strip().lower()
if tool in BANNED:
    sys.stderr.write(
        "Sub-agents are BANNED in the aimee repo (delegate-only policy). Do not use "
        "the Agent/Task tool. Use an aimee delegate instead:\n"
        '  - aimee delegate <role> "<task>" --persona <name>   '
        "(roles: engineer / qa / security / reviewer / architect)\n"
        '  - aimee delegate roundtable "<task>" --mode review --rounds 1   '
        "(for code/design review)\n"
        "For read-only reconnaissance, run the searches inline or delegate them; "
        "never spawn a sub-agent.\n"
    )
    sys.exit(2)  # block the tool, show stderr to the model

sys.exit(0)
