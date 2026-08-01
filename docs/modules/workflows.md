# workflows module

## Purpose and non-goals

`workflows` is an optional orchestration module that owns workflow definitions, triggers as workflow
intake, durable run state, advancement, approvals, scheduling, and workflow-specific integrations. It
does not own generic routing, delegates, skills, tools, policy decisions, audit, workspace authority, or
roundtable panel semantics merely because a workflow invokes them.

## Public contracts

The current contracts include `wfe_def_parse`, `wfe_def_validate`, `wfe_work_item_create`,
`wfe_engine_run`, `wfe_autonomy_run`, `wfe_advance_request_run`, block-executor registration, scheduler
notification, and trigger dispatch through `gw_orch_workflows_run`. Definitions and state transitions
must remain deterministic and versioned; provider seams perform effects without owning the lifecycle.

The descriptor declares this module's thirty sources, twenty-four module-root headers, thirty-three
direct tests, and this document; it sets `ownership_complete: true`. All twenty-four headers are
declared as `private_headers` because they live at the module root rather than under
`src/modules/workflows/include/aimee/workflows/`, the layout the header-layout checker treats as
private; every one pairs with a like-named source, and six sources carry no paired header
(`wfe_canonical.c`, `wfe_custom.c`, `wfe_live_forge.c`, `wfe_router_catalog.c`, `wfe_scheduler.c`,
`wfe_validate.c`), declaring instead through the paired headers. Make compiles all thirty sources;
CMake compiles twenty-four, omitting `gw_orch_workflows.c`, `wfe_live_foreach.c`, `wfe_live_forge.c`,
`wfe_live_panel.c`, `wfe_panel_roundtable.c`, and `wfe_replay_worktree.c`. These live, panel, forge,
replay, and gateway-orchestration units follow the same intentional thin-client
boundary recorded for the earlier modules. `docs/validation/core-modularization-slice-54.md` records
the declaration audit and `docs/validation/core-modularization-slice-55.md` the completeness audit; the
two were split so the latch reviews declarations merged on their own first. Adding a new module-local
source or module-root header without declaring it now fails CI on `rule=ownership-complete`.

## Dependencies and consumers

- `audit`: records workflow intake, decisions, effects, state transitions, and terminal outcomes.
- `config`: supplies module activation, trigger, autonomy, forge, approval, and scheduler settings.
- `delegates`: executes delegated workflow blocks through a bounded provider seam.
- `execution-policy`: authorizes tool, shell, Git, delivery, and externalization effects.
- `ir`: carries canonical request/result records across workflow steps.
- `module-runtime`: supplies optional lifecycle, capability, and readiness contracts.
- `routing`: selects a workflow without owning its intake or durable state.
- `skills`: supplies reusable instructions requested by workflow blocks.
- `tools`: exposes typed effects requested by workflow execution.
- `workspace`: supplies authorized roots and per-run worktrees.

Consumers include trigger/interactive/dev-submit intake, server workflow APIs, delegates, optional
roundtable gates, and operator dashboards. Trigger delivery is an integration; workflows owns how a
trigger becomes a run and how that run advances.

## Providers and readiness

Definitions may be built-in, YAML, or custom-block backed; executor providers include delegates,
verification, judge, forge, foreach, human gates, and roundtable gates. Readiness must report definition
validity, store/scheduler state, activation, registered providers, workspace availability, and missing
effect capabilities independently. `registered` does not imply the optional module is enabled.

## Configuration and activation

- `runtime_toggle.supported`: `false`; the descriptor is `enabled_by_default: false`, so activation is selected before startup rather than hot-toggled.

`modules.workflows` is the canonical activation field used by trigger dispatch, with a deprecated
environment fallback. Trigger rules, cron jobs, concurrency/cost ceilings, autonomy, live forge, custom
commands, and proposal autoscan are separate settings. Current server startup still calls
`wfe_autonomy_register` and `wfe_scheduler_init` unconditionally; complete disabled-profile isolation is
`not present` and must be proven by a later source/build slice.

## Surfaces

Surfaces include workflow definitions, `trigger.*` and workflow API/CLI operations, `wfe_advance`,
interactive/dev-submit intake, scheduled/armed trigger blocks, dashboards, and provider callbacks.
Triggers are part of workflows: cron, proposal, manual, or adapter code may detect an event, but the
workflow module owns admission, materialized intake, durable identity, advancement, and terminal state.

## Data and migrations

Durable data includes definitions and canonical versions, work items, events, node attempts, artifacts,
approvals, leases, pause/resume state, trigger runs/materialized inputs, costs, and per-run workspace/PR
references. DB1 `wfe_store`/trigger stores and filesystem definitions are current physical providers. Schema or
definition migrations must preserve replay, idempotency, CAS/lease behavior, and observable history.

## Security and privacy

Workflow YAML, triggers, artifacts, prompts, repository content, provider results, commands, and
approval tokens are untrusted. `execution-policy` gates effects, workspace bounds paths, vault retains
credentials, and audit receives privacy-bounded evidence. Native/shell externalization gates, protected
branch checks, signed approval content, cost limits, and bounded retries must fail closed without logging
secrets or silently advancing state.

## Supported journeys

A trigger or explicit request selects a definition, files one durable work item, and notifies the
`wfe_scheduler`; the engine leases the item, executes the current block through a registered provider,
persists output/transition evidence, and advances, pauses for approval, retries, or terminates. A
roundtable block awaits a roundtable result while workflows retains run state and scheduling ownership.

## Tests and failure behavior

The descriptor's thirty-three direct tests each link at least one `modules/workflows` object as their
subject, covering definition, validation, blocks, the engine, safety, manager flow, the scheduler,
routing and the router catalog, approval and autonomy, the native gate, enforcement, externalization,
delivery, the roundtable and panel seams, replay-worktree, the web API, and
`test_gw_orch_workflows.c`. Eleven of them are additionally registered as CTest cases; the rest run
under Make alone.

The `wfe_` prefix is shared with DB1's `src/db1/wfe_store.c` and `src/db1/wfe_binding.c`, so a
`test_wfe_*` filename is a poor ownership signal for this module and future files must be classified by
which object their target links, not by name. Four such files are deliberately not claimed:
`test_wfe_binding.c` links only DB1 objects and exercises the DB1 binding store; `test_wfe_gate_apply.c`
and `test_wfe_submitter.c` link `db1/wfe_store.o`; and `test_workflow_gate_caps.c` links no module
object at all, asserting a route capability contract. Together with those adjacent suites they exercise
current behavior. Disabled intake returns capability absence instead of filing; invalid graphs fail before
execution; lease/CAS conflict, missing provider, denied effect, budget exhaustion, or failed verification
must preserve an inspectable non-success state rather than silently complete.

## Operational diagnostics

Report `workflow`, version, work-item ID, intake source, trigger identity, activation, current node/state,
attempt/lease, provider readiness, approval/pause reason, cost/budget, workspace, and safe error class.
Exclude prompt/artifact bodies, approval keys, credentials, and raw provider output. Distinguish a
registered-but-disabled module from a scheduler or provider failure.

## Compatibility

Workflow/block names such as `trigger.watch-dir`, definition grammar, canonical version hashes, state/event shapes, trigger intake,
API/tool schemas, approval semantics, retry/CAS rules, provider interfaces, and terminal outcomes are
compatibility contracts. Triggers cannot be split into an independent module, and roundtable integration
cannot make either module the lifecycle owner of the other.

## Extension and removal

New triggers enter through the workflow intake contract; new blocks register typed executors and declare
effects/dependencies. Distributed server trigger/scheduler/provider code is a `relocate` candidate for a
later source slice. Registrations with no reachable activated journey, duplicate router/gate logic, and
self-tested-only blocks are candidates, not confirmed dead; removal requires build, config, caller,
state, and runtime-liveness evidence.
