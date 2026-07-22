# Module Ownership

Hotspot modules requiring explicit review when modified.
Changes to files listed below should be reviewed by the named owner.

## Hotspot Modules

| File | Layer | Cap | Owner | Notes |
|------|-------|-----|-------|-------|
| `webchat/` | Go service | 2000 | aimee core | Standalone browser webchat service |
| `src/modules/guardrails/guardrails.c` | 1 (data) | 2000 | aimee core | Safety-critical policy enforcement |
| `src/cmd_agent.c` | 3 (cmd) | 2000 | aimee core | Agent command entry points |
| `src/agent_tools.c` | 2 (agent) | 2000 | aimee core | Tool execution engine |
| `src/mcp_git.c` | 0 (core) | 1850 | aimee core | Git operations (has layer exemptions) |
| `src/cmd_agent_trace.c` | 3 (cmd) | 1750 | aimee core | Agent trace/debug UI |
| `src/cmd_hooks.c` | 3 (cmd) | 1700 | aimee core | Hook management |
| `src/memory.c` | 1 (data) | 1600 | aimee core | Tiered memory system |
| `src/agent.c` | 2 (agent) | 1600 | aimee core | Agent execution loop |
| `src/mcp_server.c` | 0 (core) | 1600 | aimee core | MCP server protocol |
| `src/db.c` | 0 (core) | 1600 | aimee core | Database + migrations |

## Layer Exemptions

Known layer boundary violations tracked for reduction:

| Source File | Includes | Violation | Reason |
|-------------|----------|-----------|--------|
| `src/render.c` | `agent_types.h` | L0 -> L2 | Render needs agent type definitions for display |
| `src/mcp_git.c` | `guardrails.h` | L0 -> L1 | Git operations enforce guardrail checks |
| `src/mcp_git.c` | `branch_ownership.h` | L0 -> L1 | Git operations enforce branch ownership |
| `src/cmd_describe.c` | `commands.h`, `agent.h` | L1 -> L2/L3 | Describe command straddles layers |

## Layer Architecture

```
Layer 3: Commands + UI  (cmd_*, webchat, dashboard)
Layer 2: Agent          (agent*, http_retry)
Layer 1: Data + Policy  (memory*, index, extractors, rules, guardrails, workspace, ...)
Layer 0: Foundation     (db, config, util, text, render, log, platform_*, mcp_*)
```

Lower layers must not include headers from higher layers. Violations require an
entry in the exemption table above and in `src/tests/test_build_integrity.sh`.
