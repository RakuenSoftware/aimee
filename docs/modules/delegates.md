# delegates module

## Purpose and non-goals

`delegates` is required core because Aimee must route roles to agents, execute bounded work, exchange
messages, use tools, and return auditable results. The module owns delegation planning, selection seams,
provider drivers, execution backends, credentials, sandbox/workspace coordination, and lifecycle. It is
not an optional extension and does not own roundtable policy, tools, vault, or workspace storage.

## Public contracts

Current canonical source under `src/modules/delegates` includes `delegate_driver`, routing, launch/plan,
run phases, local/Docker/SSH backends, credential acquisition/binding/retry classification, source authority,
sandbox image, economics, and gateway
orchestration. The main durable worker and HTTP/RPC orchestration still live in `src/server/server_compute*`;
root `cmd_agent_delegate.c` is an entry-point consumer. Remaining server/root implementations are relocation
debt, not a second supported delegate engine.

## Dependencies and consumers

- `audit`: records delegate decisions, actions, evidence, and terminal outcomes.
- `config`: supplies agents, roles, providers, limits, backends, and delegate policy inputs.
- `execution-policy`: authorizes delegation, tools, egress, credentials, and sandbox actions.
- `ir`: supplies canonical turn, response, tool-call, usage, and streaming structures.
- `module-runtime`: supplies required lifecycle and extension contracts for delegation.
- `routing`: selects eligible agents/providers/tiers and explains exclusions.
- `tools`: supplies the authorized capability catalog invoked by delegate turns.
- `vault`: supplies scoped credentials without transferring ownership to delegate state.
- `workspace`: supplies bounded filesystem/execution authority and lifecycle.

Consumers include CLI/API delegation, workflows, roundtable, gateway orchestration, background jobs, and
the primary runtime when it hands specialized work to another agent.

## Providers and readiness

Provider drivers and local, Docker, or SSH `delegate_backend_t` implementations sit beneath required core
delegation. Registration alone does not prove a backend is selected by the live tool/workspace path;
readiness must trace acquisition, command/filesystem use, release, credentials, and result delivery for a
supported journey. At least one policy-allowed agent/provider/backend path is required.

## Configuration and activation

- `runtime_toggle.supported`: `false`; delegation is core while individual agents, providers, and backends are configurable.

Agent roster, role/tier mappings, concurrency, budgets, backend and sandbox settings, credentials, tools,
timeouts, liveness, and workspace policy tune delegation. GUI/config fields must be hidden when their
provider or backend has no live consumer. Registering Docker/SSH must not imply delegates use it unless
the actual execution path binds that backend.

## Surfaces

Surfaces include `aimee delegate`, native/MCP delegation tools, `/v1/delegate/*` routes, mailbox and status
events, agent/delegate logs, backend/image diagnostics, provider results, and workflow/roundtable seams.
Tool calls, vault values, workspaces, routing policy, and audit storage remain owned dependencies even
when their user-facing commands participate in a delegate journey.

## Data and migrations

Delegate `job` records, messages, results, traces, budgets, credentials references, backend leases, worktree/source
authority, and outcome evidence span server and database owners. Migrations must preserve job/session and
parent-child identity, role/provider/model, attempt/turn counts, tool evidence, terminal state, budget,
workspace authority, and credential reference without persisting live secret material.

## Security and privacy

Delegates execute untrusted model output, so `execution-policy` governs every tool, command, filesystem
operation, network request, credential use, and source write; each remains policy- and workspace-scoped. Environment stripping, sandbox
binding, Git source authority, vault lookup, identity propagation, and audit are load-bearing. A local
fallback must never silently escape a requested/required sandbox or tenant boundary.

## Supported journeys

A caller submits a `delegate` role/task; routing selects an eligible configured agent; policy and budgets authorize
the run; a provider driver and execution/workspace backend conduct bounded turns and tools; messages and
evidence are recorded; liveness drives a terminal result; and the result returns through CLI/API, mailbox,
workflow, gateway, or roundtable. Retries remain within the same identity, policy, vault scope, workspace,
and audit boundaries while reacquiring credentials through the authorized vault path when required.

## Tests and failure behavior

`src/tests/test_delegate_driver.c` plus the backend, credentials, dispatch, role, plan, economics, sandbox,
ephemeral-workspace, liveness, budget, gateway-orchestration, CLI/API, and workflow suites cover the
distributed implementation. An unavailable route, provider, or backend must fail concretely; partial runs retain audit
evidence; policy, budget, or sandbox failure is fail-closed and cannot downgrade to raw local execution.

## Operational diagnostics

Use `/v1/delegate/status`, logs, parent-child and job IDs, selected agent/provider/model/backend, route exclusions,
turn/tool counts, token/time budget, liveness state, credential retry classification, workspace/sandbox
binding, and terminal evidence. Diagnostics must distinguish registered from actively bound backends and
must redact prompts, secrets, command environments, and private tool results.

## Compatibility

CLI/API request and result envelopes, role names, provider-driver capabilities, backend lifecycle,
mailbox/status events, job state, tool evidence, budget semantics, and workspace/source authority are
compatibility contracts. Moving server/root orchestration into `src/modules/delegates` must preserve
build/link targets, durable jobs, and supported in-flight recovery behavior.

## Extension and removal

Add providers through `delegate_driver_t` and execution environments through the backend/workspace
contracts, proving a live caller rather than registry-only tests. Deep-dive self-contained drivers,
backends, fallbacks, and wrappers with definition/caller and supported-journey evidence before retention;
test-only self-consumption is not liveness. Delegates cannot be optional because routing agents requires
the delegation contract.
