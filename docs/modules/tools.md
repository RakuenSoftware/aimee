# tools module

## Purpose and non-goals

`tools` is required core and owns typed capability discovery, schemas, argument validation, visibility,
authorization handoff, dispatch, cancellation, and bounded result handling. It does not own policy
decisions, raw workspace authority, stored credentials, Git semantics, protocol encoding, or the
domain behavior of every capability it exposes.

## Public contracts

The `src/modules/tools/` directory owns the tool dispatch and implementation surface: `agent_tools.c`
(tool implementations), `agent_tools_dispatch.c` (the tool-call dispatcher, which routes by name and applies
the shared guardrails/snapshot/slop hooks), and `agent_tools_anchored.c` (Part III anchored/agent-shaped
tools), behind two module-root headers: `agent_tools.h` (the tool contract, consumed by server, posix,
delegates, tests, and the Windows client through `-Imodules/tools`) and `agent_tools_internal.h` (the
private seam shared across the three sources). These were relocated from `src/posix/` (the
implementations) and `src/headers/` (the contract).

The tool-dispatch classification stage now runs in the supervised Go
`aimee-module-tools` process from `server-go/modules/tools`. The C
`module_adapter.c` remains a byte-parity fixture. The dispatcher uses only the
registered event-bus classifier; if it is absent or fails, the tool remains
unclassified rather than falling back to a local name table. Tool execution,
policy and workspace binding, schemas, and result handling are deliberately not
claimed as ported by this first classification-stage conversion.

`agent_tools.h` also declares a turn/snapshot/toolset session-state slice (`agent_tools_begin_turn`,
`agent_tools_set_snap_id`, `agent_tools_set_active_toolset`) implemented in `src/server/agent_tools.c`:
server-side tool-execution session orchestration that is not module-local, the same arrangement by
which `memory` owns its contract while DB1/DB2 implement storage. `toolset.c`, DB2 `tool_registry`,
argument/schema helpers, and MCP native dispatch remain owned elsewhere and consume this module.

## Dependencies and consumers

- `audit`: records authorized tool calls, effects, evidence, and outcomes.
- `config`: supplies tool visibility, toolsets, output limits, and execution settings.
- `execution-policy`: authorizes each proposed capability and effect before dispatch.
- `ir`: supplies canonical tool definitions, calls, results, and streaming events.
- `module-runtime`: supplies required lifecycle plus optional tool-provider registration contracts.
- `vault`: supplies scoped secret references or bounded values required by a tool implementation.
- `workspace`: supplies the selected resource provider and filesystem/process context.

Consumers include [delegates](delegates.md), [protocols](protocols.md), [gateway](gateway.md), skills,
and optional workflows/plugins. Protocol exposure and delegate loops consume the catalog; neither is
an alternate owner of schema validation or native dispatch.

## Providers and readiness

Native built-ins and the local `dispatch_tool_call_ctx` platform dispatcher form the required provider. MCP and optional plugins
may register additional tools through bounded registries. Readiness requires a consistent catalog,
valid schemas, one dispatch target for every advertised name, and a usable local reference capability;
advertised-but-unbound tools are an error, not a capability.

## Configuration and activation

- `runtime_toggle.supported`: `false`; the tool contract is required while individual capabilities and providers are configurable.

### Config touchpoint

The module consumes toolsets, role visibility, output caps (`src/modules/config/config_fields.c:107`),
compaction thresholds (`src/modules/config/config_sections.c:655`), sandbox/tool policy,
and provider registration settings. `config` parses and projects values; tools interprets catalog and
dispatch behavior. A GUI field is valid only when its named tool/provider has a live consumer path.

## Surfaces

Surfaces include model-visible tool schemas, MCP/ACP tool listing and calls, `aimee toolset`, tool events,
native result envelopes, output retrieval, and diagnostics. Git operations exposed as tools delegate to
`git`; memory/code search delegates to [memory](memory.md); filesystem/process operations execute through
`workspace`; naming them in the catalog does not transfer domain ownership.

## Data and migrations

`tool_registry` definitions, prompts, schemas, availability, provider identity, side-effect/idempotence metadata,
toolset membership, and bounded output references span code, configuration, DB1/DB2, and runtime state.
Migration must preserve canonical names, schema bytes, visibility, dispatch target, result correlation,
and cancellation while treating raw outputs, environments, and credential values as transient.

## Security and privacy

Repository content, arguments, tool output, subprocess argv/environment, and configuration are untrusted.
Every effect passes `execution-policy` and `workspace` containment before invocation. Raw secrets may
reach a bounded tool provider only through vault-authorized injection and must not enter schemas, model-
visible errors, output storage, logs, or child environments beyond the explicitly authorized operation.

## Supported journeys

[Delegates](delegates.md) request a catalog filtered by role, provider, and policy; the model emits an
IR tool call; arguments are coerced and validated; execution-policy authorizes it; workspace and vault
bind resources; `dispatch_tool_call_ctx` invokes one provider; output is capped/normalized and returned
as correlated IR plus audit evidence before the next turn.

## Tests and failure behavior

`test_toolset.c`, tool validation/schema/args/output/prompt suites, MCP native surface/dispatch tests,
script runner, server compute, and agent role-policy tests cover the distributed implementation.
Unknown, hidden, malformed, cancelled, unauthorized, unbound, or oversized calls fail concretely;
provider failure cannot fall through to shell execution or return an uncorrelated success result.

## Operational diagnostics

Report canonical tool name, catalog source, `toolset`/role visibility, schema version, provider/dispatch
target, policy result, workspace binding, duration, cancellation, output size/cap, and redacted status.
Operators must distinguish absent, hidden, invalid, denied, unavailable, timed-out, and failed tools
without logging arguments or results that contain secrets or private repository content.
Cross-module dispatch evidence is collected in the [Slice 16 validation record](../validation/core-modularization-slice-16.md).

## Compatibility

Tool names, schemas, toolset semantics, provider registration, call/result correlation, result envelopes,
and cancellation are compatibility contracts. Consolidation under `src/modules/tools` must keep native,
MCP, ACP, and delegate behavior aligned; forwarding code may translate protocol shapes but cannot maintain
a second catalog or skip common validation.

## Extension and removal

New providers register against the core catalog and dispatch seam with explicit ownership and tests.
Files named tool that implement Git, memory, gateway messaging, or protocol adapters remain with their
domain owners. Duplicate schemas/dispatch tables and definitions consumed only by their own tests are
`duplicated-by-adjacent-module` or `test-only` candidates for a later liveness and consolidation slice.
