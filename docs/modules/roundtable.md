# roundtable module

## Purpose and non-goals

`roundtable` is an optional multi-agent deliberation module that owns deliberation-specific seat policy, chair
behavior, roundtable-specific review/verification, iterative authoring pipelines, and composition of
panel findings. It is not a workflow engine, generic router, delegate runtime, benchmark authority, or
replacement for core `response-composition`.

### Go process stage

The supervised roundtable process uses the shared pure-Go module runtime for its
deterministic post-deliberation verification rubric. The RTGR/RTGD wire contract
maps replay status, factuality, and claimed severity to the existing
keep/cap/degrade/reject decision without a model call. The C adapter remains a
wire-parity fixture. Panel execution, seat resolution, providers, chair logic,
capture, pipelines, and active workflow composition remain in their current C
or existing Go owners while their process boundaries are migrated.

## Public contracts

Current contracts include delegate ensemble execution, preset seat resolution,
`roundtable_chair_apply`, preset load/save/apply, pipeline capture/chunk/evaluation, and verification.
Roundtable-specific composition must preserve attributed panel evidence and verdict semantics before
handing the result to general response composition or a consuming workflow.

Provider-neutral panel domain layouts live in `aimee/core/types/panel_result.h`.
The private `roundtable_types.h` provides aliases; `delegate_ensemble.h` declares
optional execution entry points. Callers release owned results through the
matching implementation's free function. The unused provider facade has been
retired. `scripts/check_panel_contract_boundary.py` rejects new private-header
consumers and ratchets the remaining composition dependencies.

The aggregate entry point accepts a canonical request. Reference seats receive
owned IR clones; synthesis extends the original task instead of rebuilding it
from a text projection. Text/image blocks, context metadata, opaque sidecars and
explicit sampling controls remain intact. Participant replies travel as ordered
assistant blocks, including private reasoning metadata; labels and generated
instructions occupy separate user blocks. Selected and summarized carryover stays
canonical across sequential turns, scoring, repair, chair and synthesis. Text
projections are local to deterministic review/scoring and final domain output;
there is no execution-result display cache.

## Dependencies and consumers

- `audit`: records panel selection, model calls, verification, chair decisions, and outcomes.
- `config`: supplies presets, seats, rounds, budgets, pipeline, chair, and provider settings.
- `delegates`: invokes panelists and the chair through core credential/routing seams.
- `ir`: carries canonical prompts, contributions, findings, and results.
- `module-runtime`: supplies optional lifecycle, capability, and readiness contracts.
- `response-composition`: renders the final user-facing response after roundtable semantics are resolved.
- `routing`: selects eligible delegate providers/models without owning panel policy.

Consumers include `ensemble` CLI/MCP/API routes, server authoring pipelines, sweep/review flows, optional
workflow roundtable gates, and the frontend Roundtable surface. Workflows may await a result, but retain
their own durable state, triggers, approvals, and scheduling.

The descriptor is the checked inventory for all owner-local roundtable translation units and private
headers. `ownership_complete: true` makes additions, removals, and stale declarations fail validation;
the descriptor also names the direct ensemble, chair, preset, seat-resolution, pipeline, and verification
tests plus this canonical module document. Server, workflow, DB, and protocol integration tests remain
with their composing layers. Build selection uses that boundary but is still maintained explicitly in
Make and CMake until descriptor-driven generation lands.

## Providers and readiness

Delegate routing applies eligibility, authorization and availability policy.
Roundtable adds named presets and deliberation-specific seat/persona policy;
chair and verifier calls use delegate providers. `delegate_ensemble.c` owns
`delegate_ensemble_run` and `delegate_roundtable_run` execution;
`roundtable_activation.c` owns activation and surface classification. Readiness must
separate activation, usable seats, provider credentials/health, budget, preset validity, capture store,
and pipeline state. A compiled route or saved preset is not proof of an executable panel.

## Configuration and activation

- `runtime_toggle.supported`: `false`; there is no administrative hot-toggle surface for this module.
- `modules.roundtable` is the canonical boolean activation control. When absent, the owner-local
  fallback reads `AIMEE_MODULE_ROUNDTABLE`; it accepts case-insensitive `1`, `true`, `on`, and
  `yes`, or `0`, `false`, `off`, and `no`. Missing, empty, whitespace-padded, or unknown values
  fail closed to disabled. An explicit config value always wins over the environment.
- The same control decides whether `aimee-module-roundtable` is launched. Reviews run in that
  module process over the event bus, and since the private HTTP proxy was deleted the daemon has
  no other implementation of `roundtable.review`, so with the module absent the route reports it
  as not attached however the feature is configured. The shipped module manifest is fixed when the
  image is built and cannot know what an operator enabled, so the container entrypoint consults
  `AIMEE_MODULE_ROUNDTABLE` at startup and adds the module when it is set. Containers must pass
  that variable through; a compose deployment that only sets it in `.env` does not reach the
  container.

Configuration covers reference seats/models, consensus rounds/turns, personas, chair behavior, cost and
token bounds, pipeline passes/attempts/gates, capture, and named presets. The aggregate and roundtable
engines enforce activation before fan-out or provider/model work. The CLI returns an explicit disabled
diagnostic, while a workflow `gate.roundtable` records a permanent step failure when the module is
disabled instead of entering the transient provider-retry loop.

The server resolves activation once at startup because administrative hot toggling is unsupported.
Changing `modules.roundtable` or its environment fallback therefore requires a server restart. While
disabled, roundtable-owned raw methods are absent from `server.info`, HTTP operation routes return 404,
and MCP tools are absent from `tools/list`, `find_tools`, and `describe_tool`; direct raw method or MCP
calls return unknown-method/tool semantics. Executable panel availability also requires enabled
seats and working model connections.

The current Make build includes roundtable execution. Runtime activation remains
separate: an included module is disabled unless explicitly activated. There is
no supported roundtable-omitted Make or CMake profile in this checkout.

## Surfaces

Surfaces include `aimee ensemble roundtable`, ensemble review/start/status/pause/advance/list tools,
roundtable presets, authoring-pipeline APIs, frontend authoring controls, sweep review, and workflow
roundtable/panel blocks. Generic delegate calls belong to delegates; generic output rendering belongs to
response composition; only panel deliberation semantics belong here.

As of slice 28, `roundtable_activation.c` is the single owner of roundtable operation and MCP-tool
classification. Disabled server operations are `delegate.aggregate`, `delegate.roundtable`, and
`pipeline.*`; disabled MCP surfaces are `ensemble_review`, the collapsed `pipeline` family, and direct
`pipeline_*` aliases. HTTP `POST /v1/delegate/aggregate` and
`POST /v1/delegate/roundtable` are absent at the route matcher. When runtime-disabled, workflow roundtable gates remain valid schema and fail permanently without
panel work. Static documentation remains available;
executable discovery surfaces do not advertise unavailable functionality.

## Data and migrations

State includes named JSON presets, DB1 ensemble/session records, panel assignments and contributions,
round/pass/attempt/gate state, captured prompts/results, costs, verdicts, and pipeline worktrees/artifacts.
Filesystem paths under `$AIMEE_HOME/roundtables` and `roundtable_pipeline` are physical providers.

Migrations must preserve attribution, ordering, resumability, verdict identity, and redacted evidence.

Successful roundtable results own their artifact; callers release it through
`delegate_roundtable_result_free`. Aggregate results own a canonical response,
released by `delegate_ensemble_result_free`. Shallow copies are non-owning views.
Core owns the shared domain layout; each implementation owns execution and release.

## Security and privacy

Prompts, diffs, panel output, model metadata, presets, repository context, and captured artifacts are
untrusted and may be sensitive. `delegates`, vault, routing, and audit retain their core authority.
Authorization/availability filters, bounded turns/costs/context, output parsing, secret redaction, and
workspace isolation must fail closed without allowing one panelist or chair to forge another's evidence.

## Supported journeys

A caller submits a bounded task; `roundtable` resolves eligible seats, invokes panelists, normalizes and
deduplicates findings, optionally runs iterative review or a chair, verifies convergence, and returns an
attributed result to the caller. In a workflow gate, that result advances or blocks the workflow through
the workflow provider seam; roundtable never owns the work item's durable lifecycle.

## Tests and failure behavior

`test_delegate_ensemble`, `test_server_dispatch`, `test_server_http`, and `test_mcp_client_registry`,
plus chair, preset, seat-resolution, pipeline capture/chunk/eval, panel composition, verification, and
workflow-gate suites cover current behavior. No eligible/available seats, provider error,
invalid model output, budget/turn exhaustion, failed quorum, capture failure, or non-convergence must
produce a typed incomplete/failure result rather than invented consensus.

## Operational diagnostics

Report `roundtable` mode, preset, seat/provider identity, eligibility/availability reason, round/pass,
quorum/convergence, chair use, bounded token/cost totals, capture identifier, and safe failure class.
Exclude prompts, diffs, private panel content, credentials, and raw model responses. Diagnostics must
distinguish startup-config-disabled, provider-unready, non-converged, and workflow-consumer failures.

## Compatibility

Tool/API names, preset shapes, seat aliases, core-owned `aimee/core/types/panel_result.h` result/finding schemas,
quorum and verification semantics, chair contracts, pipeline state, attribution, and workflow provider
results are compatibility contracts. `scripts/check_panel_contract_boundary.py` prevents required-side
private-header dependencies. Roundtable-specific result composition
may depend on `response-composition` but cannot replace or redefine its general memory-grounded response
contract.

## Extension and removal

New panel/chair/verifier providers use delegate/routing seams and preserve attributed IR. Distributed
server pipeline/routes and workflow panel adapters are `relocate` candidates. Legacy `session_*` tool
aliases, overlapping review pipelines, unused preset fields, or self-tested-only stages are candidates,
not confirmed dead; consolidation requires surface, config, caller, persistence, and runtime evidence.
