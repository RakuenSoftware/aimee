# delegates module

## Purpose and non-goals

`delegates` is required core because Aimee must route roles to agents, execute bounded work, exchange
messages, use tools, and return auditable results. The module owns delegation planning, selection seams,
provider drivers, execution backends, credentials, sandbox/workspace coordination, and lifecycle. It is
not an optional extension and does not own roundtable policy, tools, vault, or workspace storage.

### Go process stage

The supervised `delegate-invocation` stage runs in the shared pure-Go module
runtime. Its handler preserves the fixed DROL/DCAN role-normalization contract
for old bus callers and adds a version-2 execution contract for the native WFE
and roundtable. The latter selects a configured CLI agent and owns its bounded
subprocess lifecycle entirely in Go; no agent-service HTTP call returns to the C
daemon. Workflow lifecycle fields stay caller-side and never enter this wire.

The C adapter remains a wire-parity fixture. The C daemon still hosts the event
bus and external control surfaces, but it is not the producer used by Go
workflows or module-to-module delegation.

## Public contracts

Current canonical source under `src/modules/delegates` includes `delegate_driver`, routing, launch/plan,
run phases, local/Docker/SSH backends, credential acquisition/binding/retry classification, source authority,
sandbox image, economics, panel roster/provider seams, and gateway
orchestration. The main durable worker and HTTP/RPC orchestration still live in `src/server/server_compute*`;
root `cmd_agent_delegate.c` is an entry-point consumer. Remaining server/root implementations are relocation
debt, not a second supported delegate engine.

The descriptor declares this module's twenty-seven sources, twenty-one public headers, nineteen direct
tests, and this document; it sets `ownership_complete: true`. `delegates` is the only module in
the program whose headers all live under the canonical `src/modules/delegates/include/aimee/delegates/`
tree, so it declares no `private_headers`: the module root holds no header, and an absent field is an
empty declared set against an empty actual set. An earlier slice declared the panel and IR-rescue
pieces (three sources and two tests), and this declaration completes the remaining twenty-four sources
and the broader direct-test set. Make compiles all twenty-seven sources; CMake compiles twenty-three,
omitting `aimee_ir_rescue.c`, `delegate_ephemeral_ws.c`, `delegate_sandbox_image.c`, and
`gw_orch_delegates.c`. These server/KB-side units follow the same intentional thin-client boundary recorded
for gateway, learning, workspace, vault, config, and git, though CMake reaches far more of this module
than of those. `docs/validation/core-modularization-slice-52.md` records the declaration audit and
`docs/validation/core-modularization-slice-53.md` the completeness audit; the two were split so the
latch reviews declarations merged on their own first. Adding a new module-local source now fails CI on
`rule=ownership-complete`, and so does adding a module-root header: the absent `private_headers` field
means the empty set is enforced, not unchecked.

### IR-side prose tool-call rescue

The module owns `aimee_ir_rescue_tool_calls` in
`src/modules/delegates/aimee_ir_rescue.c`, with its public contract at
`src/modules/delegates/include/aimee/delegates/aimee_ir_rescue.h`. This recovery layer handles a delegate
model capability gap: models without reliable native tool calling may emit XML, Qwen, harmony, Mistral,
or policy-enabled JSON calls as prose. It reuses the delegate-owned
`delegate_rescue_parse_tool_calls` dialect parser and converts eligible `AIMEE_BLK_TEXT` content into
canonical `AIMEE_BLK_TOOL_USE` blocks.

The rescue scans only `AIMEE_BLK_TEXT`. If the response already contains an `AIMEE_BLK_TOOL_USE` block,
it leaves the entire response unchanged to avoid duplicate dispatch. For a rewritten response, malformed
or non-object arguments become an empty JSON object, the stop reason becomes `AIMEE_STOP_TOOL_USE`, and
`ir_rescue_recoveries` is incremented once regardless of the number of calls recovered. Preparation and
final block-array allocation happen before source content is consumed, so a pre-commit allocation failure
returns `0` with the response unchanged.

Provider wire parsing remains owned by `translation`; final-answer assembly remains owned by
`response-composition`; server IR rollout, transport, and shadow controls remain outside this bounded
delegate capability. The live bridge consumer is `src/posix/agent_ir_parse.c`.

### Panel roster and provider boundary

Panel membership is delegate-routing policy even when no deliberation module is active. The public
`panel_roster.h` contract seeds configured agents, excludes the primary and unauthorized delegates,
resolves random seats, and filters currently unavailable agents. Sweep and other core consumers call this
required seam directly; roster selection is not hidden behind optional roundtable execution.

Optional panel engines register at most one startup-lifetime `aimee_panel_provider_t` through
`panel_provider.h`. Registering the same provider pointer again is idempotent; registering a different
provider while one is installed is rejected. Core callers use `aimee_panel_aggregate` or
`aimee_panel_run`. The facade reports explicit unavailable, invalid-input, and provider-error statuses and
zeroes the output on every failure; if a failing callback supplied an artifact, the facade dispatches its
release before zeroing the result. All request inputs and options are borrowed for the duration of the
call. IR owns the `aimee_panel_result_t` layout. A provider allocates the artifact returned in a successful
result, and the caller must release it exactly once through `aimee_panel_result_release` before the
provider is unregistered. Unregistration is rejected while a result remains outstanding. Registration
and unregistration are startup-only: the provider descriptor must remain valid until all calls have
completed and all results have been released, and concurrent hot unload is unsupported.

When roundtable is omitted at build time, the provider and roster facades remain in core, no optional
private include path or symbol is linked, and panel calls return the typed unavailable status. This keeps
delegation and routing usable without substituting a stub implementation for the absent module.

### Public-header contract

All 21 delegate headers live under `src/modules/delegates/include/aimee/delegates/`, and every consumer
uses the `<aimee/delegates/...>` namespace. `src/modules/delegates/module.yaml` declares that complete
surface in `public_headers`; `scripts/check_module_header_layout.py` rejects flat shadows, bare includes,
missing canonical headers, or restored flat Make/CMake include roots.

The required-core `delegates` module owns this public surface. Adding, removing, or renaming a header must
update the descriptor and refactor public-header baseline in the same change. There is no compatibility
forwarding layer and no second supported flat API. The separately tracked roundtable/delegates header cycle
is a dependency-design concern, not an exception to canonical include ownership.

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

The descriptor's nineteen direct tests each drive a delegates source as their subject: the driver,
backend family (`test_delegate_backend*.c`), credentials, economics, ephemeral workspace, patch
coordinator, plan, role, sandbox image, XML fallback, gateway-orchestration
(`test_gw_orch_delegates.c`), IR rescue, and panel provider suites, plus
`test_delegate_context_shed.c`, `test_delegate_dispatch_reliability.c`, and
`test_delegate_handoff.c`, which each link only `delegate_prompt.o` and together are that source's
coverage. Six adjacent `test_delegate*`/`test_panel*`/`test_gw_orch*` files are not claimed because
their subject is another module: `test_delegate_liveness.c` links `server/liveness.o`,
`test_delegate_token_budget.c` links `server/agent_coord.o`, `test_delegate_ensemble.c` drives
`modules/roundtable/delegate_ensemble.c`, `test_gw_orch_workflows.c` and
`test_gw_orchestration_seam.c` link no delegates object, and `test_panel_ir_contract.c` includes
`<aimee/ir/panel_result.h>`. Together with the CLI/API and workflow suites they cover the
distributed implementation. An unavailable route, provider, or backend must fail concretely; partial runs retain audit
evidence; policy, budget, or sandbox failure is fail-closed and cannot downgrade to raw local execution.
`src/tests/test_aimee_ir_rescue.c` directly covers prose rescue. `src/tests/test_panel_provider.c` covers
unavailable, registration, success, inconsistent failure, ownership, and single-release behavior.
Consumer-level rescue coverage runs through
`src/tests/test_agent_ir_parse.c` and `src/tests/test_responses_parity.c`; all three must pass before changes
to this behavior are considered verified.

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
