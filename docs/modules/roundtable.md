# roundtable module

## Purpose and non-goals

`roundtable` is an optional multi-agent deliberation module that owns panel/seat formation, chair
behavior, roundtable-specific review/verification, iterative authoring pipelines, and composition of
panel findings. It is not a workflow engine, generic router, delegate runtime, benchmark authority, or
replacement for core `response-composition`.

## Public contracts

Current contracts include delegate ensemble execution/results, panel eligibility and seat resolution,
`roundtable_chair_apply`, preset load/save/apply, pipeline capture/chunk/evaluation, and verification.
Roundtable-specific composition must preserve attributed panel evidence and verdict semantics before
handing the result to general response composition or a consuming workflow.

## Dependencies and consumers

- `audit`: records panel selection, model calls, verification, chair decisions, and outcomes.
- `config`: supplies presets, seats, rounds, budgets, pipeline, chair, and provider settings.
- `delegates`: invokes panelists and the chair through core credential/routing seams.
- `ir`: carries canonical prompts, contributions, findings, and results.
- `module-runtime`: supplies optional lifecycle, capability, and readiness contracts.
- `response-composition`: renders the final user-facing response after roundtable semantics are resolved.
- `routing`: selects eligible delegate providers/models without owning panel policy.

Consumers include `ensemble` CLI/MCP/API routes, server authoring pipelines, sweep/review flows, optional
workflow roundtable gates, and the frontend Roundtable surface. Workflows may await a result, but retains
its own durable state, triggers, approvals, and scheduling.

## Providers and readiness

Panel providers are resolved from configured agents, `roundtable_preset` eligibility/authorization/availability filters,
seat presets, and random-seat rules; chair and verifier calls use delegate providers. Readiness must
separate activation, usable seats, provider credentials/health, budget, preset validity, capture store,
and pipeline state. A compiled route or saved preset is not proof of an executable panel.

## Configuration and activation

- `runtime_toggle.supported`: `false`; the descriptor is `enabled_by_default: false`, so activation is selected before startup rather than hot-toggled.

Configuration covers reference seats/models, consensus rounds/turns, personas, chair behavior, cost and
token bounds, pipeline passes/attempts/gates, capture, and named presets. Unlike workflows, no
`modules.roundtable` activation field was found in `config_parse_modules_section`; server routes and
roundtable objects are compiled directly. Descriptor-declared optionality is therefore not fully
enforced by current configuration/build wiring.

## Surfaces

Surfaces include `aimee ensemble roundtable`, ensemble review/start/status/pause/advance/list tools,
roundtable presets, authoring-pipeline APIs, frontend authoring controls, sweep review, and workflow
roundtable/panel blocks. Generic delegate calls belong to delegates; generic output rendering belongs to
response composition; only panel deliberation semantics belong here.

## Data and migrations

State includes named JSON presets, DB1 ensemble/session records, panel assignments and contributions,
round/pass/attempt/gate state, captured prompts/results, costs, verdicts, and pipeline worktrees/artifacts.
Filesystem paths under `$AIMEE_HOME/roundtables` and `roundtable_pipeline` are physical providers.
Migrations must preserve attribution, ordering, resumability, verdict identity, and redacted evidence.

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

`test_delegate_ensemble` and chair, preset, seat-resolution, pipeline capture/chunk/eval, panel composition, verification,
MCP, HTTP, and workflow-gate suites cover current behavior. No eligible/available seats, provider error,
invalid model output, budget/turn exhaustion, failed quorum, capture failure, or non-convergence must
produce a typed incomplete/failure result rather than invented consensus.

## Operational diagnostics

Report `roundtable` mode, preset, seat/provider identity, eligibility/availability reason, round/pass,
quorum/convergence, chair use, bounded token/cost totals, capture identifier, and safe failure class.
Exclude prompts, diffs, private panel content, credentials, and raw model responses. Diagnostics must
distinguish descriptor-disabled, provider-unready, non-converged, and workflow-consumer failures.

## Compatibility

Tool/API names, preset shapes, seat aliases, panel result/finding schemas, quorum and verification
semantics, chair contracts, pipeline state, attribution, and workflow provider results are compatibility
contracts. Roundtable-specific result composition may depend on `response-composition` but cannot replace
or redefine its general memory-grounded response contract.

## Extension and removal

New panel/chair/verifier providers use delegate/routing seams and preserve attributed IR. Distributed
server pipeline/routes and workflow panel adapters are `relocate` candidates. Legacy `session_*` tool
aliases, overlapping review pipelines, unused preset fields, or self-tested-only stages are candidates,
not confirmed dead; consolidation requires surface, config, caller, persistence, and runtime evidence.
