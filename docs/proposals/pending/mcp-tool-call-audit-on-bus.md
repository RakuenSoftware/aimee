# Proposal: audit MCP / tool-call activity onto the event bus

## Context

The event-bus observability arc records six governed surfaces to the audit ledger
through `obs_bus` (a NULL-default hook in the module + a server-only bridge, D7):
governed-action audit, guardrail events, vault credential access, sandbox
degraded-isolation, and memory mutations (server + kb). Each was reviewed for a
no-secret / no-content leak and confirmed on the bus.

One governed surface is **not** on the bus: **MCP / tool-call activity**. Every
tool aimee invokes — its own tools, tools exposed over the MCP gateway, and tools
called out to external MCP servers — carries **arguments and results**, which are
precisely the secret / PII surface (file contents, credentials in arguments,
command lines, query bodies, model output). Today these calls are dispatched with
no durable, replayable audit record of *who called which tool, against which
server, with what argument fingerprint, and with what outcome*.

This proposal closes that gap by mirroring the established bridge pattern — **and
nothing more**. It records tool-call *identity and outcome*, never argument or
result *content*.

## Decision

Add an **MCP / tool-call audit bridge** that publishes one audit row per tool call
over `obs_bus` (`KIND_AUDIT_ACTION` → the ledger + capture/replay), fingerprint
only.

**Fire sites (the two choke points already in the code):**

1. **Outbound** — `mcp_client_call_tool()` (`modules/protocols/mcp/mcp_client.c`):
   every call aimee makes to an external MCP server. Carries the session (server
   identity + transport), the tool name, the `cJSON` arguments, and the rc/error.
2. **Internal + served** — `dispatch_tool_call()` /
   `dispatch_tool_call_ctx()` (`server/server_compute_async.c`): the central
   dispatch for aimee's own tools and the MCP-gateway-exposed tools, carrying the
   tool name, arguments, and result status. (The existing
   `agent_tool_event_cb_t(phase, tool_name)` is a streaming-telemetry hook — it
   carries no arguments or principal, so it cannot fingerprint and is not reused.)

**Mechanism (identical discipline to the other five bridges):**

- A new NULL-default hook `mcp_set_tool_audit_hook(fn)` in the MCP module, fired at
  the two sites above at call completion (success, error, timeout). A thin client
  that links the MCP module but not the bus leaves the hook NULL — no behavior
  change, no bus dependency (D7).
- A server-only bridge `mcp_tool_audit_bridge.c` implementing the hook →
  `obs_bus_emit(...)`, installed in `server_main.c` next to the existing three
  (`vault` / `sandbox` / `memory`). Only this object links the MCP dispatch to the
  D7-confined bus.

**The row (mapped onto `obs_bus_emit`'s existing eight fields):**

| field | value |
| --- | --- |
| `actor` | the principal / session id driving the call (or `"mcp"` for an unattributed subsystem call) |
| `tool` | the tool name, namespaced by server (e.g. `github:create_issue`, or `local:git_status`) |
| `args_hash` | `audit_args_hash(tool, args_json)` — HMAC-SHA256 over the serialized arguments; one-way, fixed size |
| `command` | the human-readable, **non-secret** identity: `"<direction> <server>/<tool>"` |
| `mode` | direction + transport: `outbound:stdio` / `outbound:http` / `served` / `internal` |
| `reason_code` | error class on failure (`timeout`, `transport`, `tool_error`, `refused`), empty on success |
| `verdict` | `ok` / `error` / `timeout` / `refused` |
| `task_id` | the associated task/session, or 0 |

Because the payload is a fixed-size fingerprint plus short identity strings, the
row is **well under the inline budget** — it rides inline exactly like the other
five bridges. **No arena payload is involved.**

## PII / secret discipline (the crux)

MCP arguments and results are the highest-risk content surface on the bus, so the
rule is absolute and mirrors the memory/vault bridges:

- **Arguments** cross only as `audit_args_hash` — a keyed one-way HMAC. The raw
  argument JSON is serialized transiently to feed the hash and is never placed on
  the bus, never logged, never persisted.
- **Results** are not captured at all — only a `verdict` and, on failure, a
  bounded `reason_code`. No result bytes, no result size that could leak content
  structure beyond the verdict.
- **Tool / server identity** (`tool`, `command`) is non-secret by construction — a
  tool name and server label, the same identity a user sees when configuring the
  MCP server. Any server whose *name* is itself sensitive is out of the trust model
  (the operator configured it).

## Non-goals

- **No raw argument or result content on the bus, ever.** This is audit identity,
  not a wire tap.
- **No full-content capture / replay of tool I/O.** Recording the actual arguments
  and results (for debugging or forensic replay) is a *separate*, opt-in,
  egress-gated feature with its own threat model and redaction requirements — it is
  the natural first real consumer of the arena payload path (args/results can
  exceed the inline budget), but it is explicitly **out of scope here** and must
  not be conflated with audit. This proposal deliberately keeps the safe,
  fingerprint-only audit distinct from any content capture.
- **No new egress.** The row lands in the existing audit ledger via the existing
  bus; the D7 blast-radius invariant is unchanged (the bridge is server-only).
- **No change to tool-call behavior.** The hook is observational and best-effort;
  a bus that is down or backpressured never blocks or fails a tool call.

## Binding checks

- `test_bus_mcp_audit` (mirrors `test_bus_sandbox_audit` / `test_bus_vault_audit`):
  install `mcp_tool_audit_bridge`, drive a real outbound call and a real internal
  dispatch through a host with the bridge, and assert the audit row reaches the
  ledger with the expected `tool` / `mode` / `verdict` and a well-formed
  `args_hash`.
- **No-secret-leak assertion** (as in the memory bridge test): feed a tool call
  whose arguments contain a unique sentinel string; assert the sentinel appears
  **nowhere** in the emitted row or the ledger — only its hash-derived identity.
- `scripts/check_bus_blast_radius.sh` stays green: the MCP module gains a
  NULL-default hook (no bus symbols); only `mcp_tool_audit_bridge.o`, linked into
  aimee-server, references the bus.

## Rollout

1. Land the hook (`mcp_set_tool_audit_hook`) + fire sites, NULL-default — no
   behavior change, no bridge yet.
2. Land `mcp_tool_audit_bridge.c` + install in `server_main.c` + the tests.
3. Multi-agent convergence review focused on the secret-leak surface (the same bar
   the other five bridges cleared), then merge.

This completes the event-bus observability arc: after this, every governed surface
aimee exposes — action, guardrail, vault, sandbox, memory, and MCP/tool calls — is
recorded to the audit ledger through one bus, fingerprint-identified, content-free,
and D7-confined.
