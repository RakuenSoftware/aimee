# Claude Code hooks (aimee)

Committed PreToolUse hook scripts. `.claude/settings.json` / `settings.local.json`
(which *wire* these hooks) are **gitignored**, so the scripts live here but each dev
must wire them locally (or let `aimee`'s installer do it; see below).

## `block_subagent.py`: sub-agent ban (delegate-only)

Claude Code sub-agents (the `Agent`/`Task` tool) are **banned** in this repo; use
**aimee delegates** instead (`aimee delegate <role> …`, `aimee delegate roundtable …`).

Why the script is needed: aimee's own sub-agent ban lives in the runtime gateway
(`src/gateway_policy.c` → `gateway_policy_strip_tools` → `is_subagent_tool_name`,
canonicalizing `Task`/`Agent`/`spawn_agent` → `"Subagent"`). That only strips tools
flowing **through aimee's `/v1` gateway** to model providers, so it governs *aimee's
own* agents. It never sees Claude Code's harness-level `Agent`/`Task` tool. This
PreToolUse hook closes that gap at the Claude Code layer: it exits 2 (blocks the
tool) with a redirect-to-`aimee delegate` message.

### Wire it (personal, per-dev: `settings.json` is gitignored)

```jsonc
// .claude/settings.local.json
{
  "permissions": { "deny": ["Task", "Agent"] },
  "hooks": {
    "PreToolUse": [{
      "matcher": "Agent|Task|Subagent|spawn_agent|RemoteTrigger",
      "hooks": [{ "type": "command",
                  "command": "<repo>/.claude/hooks/block_subagent.py" }]
    }]
  }
}
```

Verify without spawning a sub-agent:
`echo '{"tool_name":"Agent"}' | .claude/hooks/block_subagent.py; echo $?`  → `2`.
(Never "test" it by actually invoking `Agent`; that spawns the banned sub-agent if
the settings watcher hasn't reloaded. Open `/hooks` once, or restart, to load it.)

## Team-wide auto-install via aimee's installer (implemented)

`block_subagent.py` is now the **zero-dependency fallback**. The ban is auto-installed
by aimee's standard client setup, with no gitignored-settings dependency, so a fresh
clone is protected once `aimee` runs:

1. `aimee subagent-guard` (a CLI subcommand, `src/cli_main.c: handle_subagent_guard`)
   reads the PreToolUse hook JSON on stdin and denies the call for a sub-agent tool
   (`client_tool_is_subagent`: `Task`/`Agent`/`spawn_agent`/`RemoteTrigger`/`Subagent`),
   emitting the redirect-to-`aimee delegate` message. Allow = exit 0 with no output.
2. `ensure_claude_code_hooks` → `ensure_subagent_ban` (`src/client_integrations.c`)
   evaluates the gate ONCE at setup, config `subagent_ban_enabled` (default on) AND a
   one-shot `agent.list` server probe reporting `any_delegate_available`, and, when it
   holds, wires the `subagent-guard` PreToolUse hook **plus** a `permissions.deny
   [Task, Agent]` backstop. When it does not hold (config opt-out or no usable
   delegates), it removes both, so a config/delegate change un-installs on the next
   setup / session-start. A "server unreachable" probe leaves settings untouched.

The gate is deliberately delegates-aware: the ban only engages when there is a delegate
to redirect to. Set `subagent_ban_enabled: false` in `aimee.yaml` to opt out entirely
(also disables the server-side guardrail block); the change takes effect at the next
client setup.

The hook's mere presence is the runtime gate, so it carries no per-call config read or
server round-trip. `block_subagent.py` remains a hand-wired option for checkouts where
`aimee` client setup has not run.

## `enforce_worktree.py`: keep edits out of the shared main clone

aimee already isolates its **own** work in worktrees (every delegate / work item
runs in a locked `aimee/wi/<id>` worktree, `src/server/delegate_checkout.c`,
`src/workflow/wfe_blocks.c`). But the **primary** session (Claude Code in a tmux
TUI) edits files with the harness's `Edit`/`Write` tools, which never traverse
aimee's `/v1` gateway, the same blind spot the sub-agent ban describes. So nothing
stopped the primary from editing the **shared main clone** directly, and concurrent
sessions piled uncommitted changes onto one branch.

Detection primitive: a main clone's `.git` is a **directory**; a linked worktree's
`.git` is a **file**. So "am I in the shared checkout?" is a one-line filesystem
test, no git subprocess.

This PreToolUse hook blocks `Edit`/`Write`/`MultiEdit`/`NotebookEdit` whose target
lives in a main clone, with a redirect to create/use a worktree. Escape hatch for
the human who owns the main-clone branch: `AIMEE_ALLOW_MAIN_CHECKOUT=1` or a
`<repo>/.git/aimee-allow-main-edits` marker.

### Two layers (belt + suspenders)
1. **Native (installer-wired, primary):** the same policy lives in `aimee hooks pre`
   (`src/cmd_hooks.c`: `path_is_main_clone` + `main_clone_edits_allowed`), which
   aimee's installer already wires as a PreToolUse hook, so it enforces on every
   client with no gitignored-settings dependency.
2. **Script (zero-dependency fallback):** `enforce_worktree.py`, wired the same way
   as `block_subagent.py`:
   ```jsonc
   // .claude/settings.local.json
   { "hooks": { "PreToolUse": [{
       "matcher": "Edit|Write|MultiEdit|NotebookEdit",
       "hooks": [{ "type": "command",
                   "command": "<repo>/.claude/hooks/enforce_worktree.py" }] }] } }
   ```
