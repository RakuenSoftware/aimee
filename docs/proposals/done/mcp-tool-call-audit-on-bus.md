# Proposal: record tool-call OUTCOME (and transport) on the event bus

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** DONE. Delivered scope archived 2026-07-26.

> **Archived complete (2026-07-26).** The audit found the scoped deliverables shipped,
> superseded by the current implementation, or fully represented by completed child slices.

> **Revised after a five-lens roundtable review** (2026-07-25). The first draft
> claimed MCP tool calls were an entirely un-audited surface protected by an
> "argument fingerprint." Both claims were wrong against the code: tool-call
> *identity* is already on the bus, and `audit_args_hash` is name-only for MCP
> tools. This revision rescopes to the genuine, narrow gap and fixes the
> mechanics. See "What the review corrected" at the end.

## Context: what is already audited

The governed-action audit already publishes a row to `obs_bus`
(`KIND_AUDIT_ACTION` → ledger + capture/replay) for tool calls, at the guardrail
decision point: `pre_tool_check()` → `emit_action_audit()`
(`modules/guardrails/guardrails_action_audit.c:138`), fired from the tool
dispatcher (`modules/tools/agent_tools_dispatch.c:2069`). That row already carries
`actor`, `tool`, a real (allowlist-projected) `args_hash`, a `command` preview,
`mode`, `verdict`, and `task_id`, and it fires for aimee's own tools **and** for
namespaced/external MCP tools (the dispatcher routes `server:tool` names into the
MCP client). Inbound calls to aimee's tools over the MCP gateway are gated on the
same pre-check path.

So tool-call **identity** is not the gap.

## The actual gap

The pre-check row is emitted *before execution*, at the guardrail decision. It
therefore cannot carry what only exists *after* the call returns:

- the **outcome**, did the tool succeed, error, time out, or get refused;
- the **transport**, for an external MCP tool, was it stdio or SSE, to which
  configured server.

Today a governed tool call records "we decided to allow/deny this call" but not
"and here is how it actually ended." For security review and replay, the outcome
of a call (especially a *refused* or *errored* externalization) is the half that
matters most, and it is absent.

## Decision

Add a single **tool-call completion emit** at the dispatch layer that records the
post-execution outcome + transport, correlated to the existing pre-check row by
`actor`/`tool`/`task_id`. One emit, one new row per completed call, fingerprint
and identity only, no argument or result content, no new egress, D7 unchanged.

**Fire site, exactly one, where the principal lives.** The internal choke point
is `dispatch_tool_call_ctx()` (`modules/tools/agent_tools_dispatch.c:1889`, the
`_ctx` wrapper every internal caller funnels through; **not** `dispatch_tool_call`,
a thin wrapper that would double-fire, and **not** `server_compute_async.c`, which
only *calls* the dispatcher). The dispatch layer already holds the principal
(`dispatch_sid = session_id()`) and the role, and it already sees both native
tools and (via the `server:tool` branch → `mcp_client_registry_call_tool`) the
outbound MCP call. Emitting here yields exactly one completion row per logical
call and avoids the double/triple-audit that firing at the raw `mcp_client` layer
would cause (the dispatcher also rewrites arguments before the wire call, so a
raw-layer row would carry a different hash and mislabel transport).

- **Transport** is threaded back through the registry/`mcp_client` return to the
  dispatch layer, not audited at the `mcp_client` layer. It is derived from
  `s->transport->kind` (`MCP_TRANSPORT_STDIO` / `MCP_TRANSPORT_SSE`; there is no
  `http` kind).
- **Refused/blocked paths must emit.** A guardrail block returns early
  (`server_compute_async.c:206-220`) *before* dispatch; a role/cancel denial
  returns early inside `dispatch_tool_call_ctx_inner` (cancel ~1905, role denial
  ~1963); the served surface refuses at the WFE externalization guard
  (`server_mcp.c:1689`). The completion emit must observe these return paths (or
  be emitted from within them) so a *refused* call yields a `refused` row rather
  than silence, enumerate every return path and its verdict, including a
  pre-execution cancel.

**Served surface.** External clients invoking aimee's tools over the gateway are
dispatched by `handle_mcp_call()` (`server_mcp.c:1631`), which does **not** route
through `dispatch_tool_call_ctx`. Either (a) add `handle_mcp_call` as a second
completion site, or (b) confirm-and-cite that `pre_tool_check` already covers the
served path and record only the internal/outbound completion here. This proposal
must resolve which, not assume; the served path is the highest-value "who called
aimee" target and must not be silently uncovered.

**Mechanism (mirrors the five existing bridges).** A NULL-default hook on the
**tools-dispatch** seam (named for what it audits (tool dispatch) not "MCP",
since it fires for `Read`/`Bash` too) + a server-only bridge → `obs_bus_emit`,
installed in `server_main.c` next to vault/sandbox/memory. A thin client that
links the tools module but not the bus leaves the hook NULL (D7).

**The completion row (obs_bus_emit's eight fields):**

| field | value |
| --- | --- |
| `actor` | the principal from the dispatch layer (`session_id()` / role): a real "who", not a `"mcp"` constant |
| `tool` | the tool name (namespaced for remote tools, e.g. `github:create_issue`): display identity |
| `args_hash` | `audit_args_hash(bare_tool, NULL)`: **tool-name-only**, exactly as vault/sandbox/memory pass it; correlates to the pre-check row, carries no argument content (see PII discipline) |
| `command` | non-secret identity / correlation string |
| `mode` | `internal` / `outbound:stdio` / `outbound:sse` / `served` |
| `reason_code` | outcome class on non-success: `timeout` / `transport` / `tool_error` / `refused`: a fixed enum, never free text |
| `verdict` | `ok` / `error` / `timeout` / `refused` |
| `task_id` | from the dispatch layer |

Every field is hard-capped by `put_str` truncation (`AB_TOOL`=256, `AB_MODE`=64,
`AB_HASH`=96 > `AUDIT_ARGS_HASH_LEN`=68, `AB_COMMAND`=512), summing (~1.25 KB)
well under `inline_budget`=1900, the row rides **inline**, no arena payload. (A
pathologically long namespaced tool name truncates harmlessly in the display
fields only.)

## PII / secret discipline (the crux)

Arguments and results are the highest-risk content surface; the rules are
absolute:

- **No argument content.** `args_hash` is the tool-name-only HMAC (as above). We
  do **not** serialize the arguments to hash them, for an MCP tool `audit_args_hash`
  would ignore the serialized args anyway (off-allowlist → name-only), so
  serializing would create a transient raw-secret buffer for nothing. Pass `NULL`,
  like the other five bridges. (If genuine per-argument identity is ever required,
  that is a **new, separately-reviewed keyed-hash primitive** with its own
  canonicalization/bounding/determinism/threat model, not a reuse of
  `audit_args_hash`.)
- **No result content.** Only `verdict`. The result cJSON is never read into a
  field.
- **`err_buf` is FORBIDDEN as field input.** `mcp_client` copies the MCP server's
  own error text into `err_buf` (`mcp_client.c:106-108`, `session_roundtrip`), and
  that text is external, server-controlled, and routinely echoes argument values
  (`"file not found: /home/u/.ssh/id_rsa"`). It MUST be classified into the fixed
  `reason_code` enum and MUST NEVER be copied verbatim into `reason_code`,
  `command`, or any field. Note: anything that reaches those fields also persists
  to the `0600` `.aimeecap` capture/replay file (`obs_bus.c:268-294`), so this is
  a disk-persistence rule, not only a bus rule.
- **Tool/server names.** Operator-configured *server labels* are trusted identity;
  server-supplied *tool names* (from the server's `tools/list`, `mcp_client.c:864`)
  are attacker-influenceable but bounded (capped, non-content), charset/length-
  clamp the namespaced token as the `command` preview already is.

## Non-goals

- **No raw argument or result content on the bus, ever**: this is outcome +
  identity, not a wire tap.
- **No full-content capture / replay of tool I/O.** Recording the actual arguments
  and results is a *separate*, opt-in, egress-gated feature with its own threat
  model and redaction requirements. It would be the first real consumer of the
  arena payload path (args/results can exceed the inline budget; see #1954), and
  is explicitly **out of scope** here.
- **No new egress, no behavior change.** The row lands in the existing ledger via
  the existing bus (D7 unchanged); the emit is observational and best-effort. A
  down or backpressured bus never blocks or fails a tool call.

## Binding checks

- `test_bus_tool_completion` (mirrors `test_bus_sandbox_audit`): install the
  bridge, drive a real native call, a real outbound MCP call, and a **refused**
  call, and assert each yields exactly ONE completion row with the expected
  `mode` / `verdict` / `reason_code`, and that an external call produces exactly
  one row (not the double/triple the raw-layer design would).
- **No-secret-leak assertion, on all three content surfaces.** Plant distinct
  sentinels in (a) the arguments, (b) the tool RESULT payload, and (c) the MCP
  server's ERROR RESPONSE, and assert each appears **nowhere** in the row, the
  ledger, OR the `.aimeecap` capture file. (The args-only assertion in the first
  draft would have passed vacuously, since MCP args are never hashed.)
- `scripts/check_bus_blast_radius.sh` stays green: the tools module gains a
  NULL-default hook (no bus symbols); only the new bridge object, linked into
  aimee-server, references the bus.

## Implementation status: LANDED (this PR)

The dispatch-layer completion audit is implemented:

- **Hook.** A process-global, NULL-default completion hook lives in a new
  dependency-free TU `modules/tools/agent_tools_completion.c` (so the dispatcher's
  many callers (and a thin client) link the hook mechanism without the bus, and
  the tools module stays bus-free for D7). `dispatch_tool_call_ctx` resets a
  thread-local outcome, runs the call, and fires the hook once on the way out.
- **Verdict/reason.** Set at every refusal/error return path of the dispatcher as
  fixed enums (`cancelled` / `role` / `policy` / `guardrail` / `bad_args` /
  `unknown_tool` / `tool_error`); the ok/error/timeout of a normal execution is
  classified from the result string's leading marker only. The string itself is
  never stored or passed on, so `err_buf` (and all content) stays out of the audit
  fields.
- **Bridge.** `server/tool_completion_audit_bridge.c`, installed in
  `server_main.c` next to vault/sandbox/memory, maps the outcome onto
  `obs_bus_emit` (name-only `args_hash`, empty `command`, no content).
- **Test.** `test_bus_tool_completion` drives the real bridge → obs_bus → ledger
  and asserts verdict/mode/reason_code, identity-only rows, a sentinel absent
  everywhere, and that every `reason_code` is a fixed enum value. Wired into
  `check_bus_perf_gate.sh`. D7 gate green.

**The two follow-ons are now also landed:**

- **Transport specificity.** An outbound call records `outbound:stdio` /
  `outbound:sse` (not generic `outbound`), via a new locked registry accessor
  `mcp_client_registry_transport_kind(qualified_name)` that reads the serving
  session's `transport->kind`, no change to the `call_tool` signatures.
- **Served-call OUTCOME.** `handle_mcp_call` (the `mcp.call` socket method; an
  external client invoking aimee's tools) was in fact **unaudited** (it does not go
  through `pre_tool_check`; the `server.c:1030` identity row is the separate HTTP
  PreToolUse path). It is now wrapped: one `mode=served` completion row per call,
  with the resolved tool, the caller's session id, and a classified verdict
  (`refused` on the WFE-externalization/capability gate, `error`/`bad_args` on a
  bad tool/args, `ok` when dispatched). This is the served-DISPATCH outcome; a
  delegate's own deeper success/failure is audited where it runs. The row is
  identity + enums only, no argument, result, or error content.

## Original rollout (for reference)

1. Land the tools-dispatch completion hook + fire site(s), NULL-default. *(done)*
2. Land the bridge + install in `server_main.c` + the tests above. *(done)*
3. Multi-agent convergence review focused on the err_buf/reason_code fence and the
   refused-path coverage, then merge. *(review welcome on this PR)*

## What the review corrected (for the record)

- **"Argument fingerprint" was false.** `audit_args_hash` projects argument values
  only for a hardcoded 8-tool allowlist (`audit_action.c`: *"A tool absent here
  hashes its NAME ONLY"*); every namespaced MCP tool is off-list, so the hash is
  name-only and the original no-leak test would have passed vacuously. Dropped the
  claim; the hash is honestly identity-only.
- **The gap was smaller than claimed.** `emit_action_audit` already records
  tool-call identity on the bus; the real delta is outcome + transport.
- **The two fire sites nested** (dispatch → registry → mcp_client), double/triple-
  auditing external calls with divergent hashes → collapsed to one dispatch-layer
  emit, which also fixes the missing-principal problem.
- **The "served" surface bypassed both proposed sites** (`handle_mcp_call`) → now
  called out as a must-resolve.
- **`err_buf` was an unfenced content surface** that also persists to disk → now an
  explicit non-forwarding rule with a dedicated leak test.
- **File locations were wrong** (`server_compute_async.c` → `agent_tools_dispatch.c`)
  and a transport (`outbound:http`) named a kind that does not exist.
- **Kept:** the fixed-size INLINE / no-arena claim and the D7 NULL-default-hook
  story, both verified correct.
