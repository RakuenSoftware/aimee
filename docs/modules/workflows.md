# workflows module

## Purpose and non-goals

The C `workflows` module does not run workflows. `aimee-wfe`, the Go control plane, owns the
lifecycle. The C directory remains compiled and tested while its reusable helpers move behind the
Go resource boundary and its former engine is retired.

Do not add new lifecycle behavior here. New definitions, scheduling, state transitions, triggers,
gates, worktrees, forge decisions, and recovery belong in `server-go`.

## Public contracts

The supported runtime contract is the Go API under `/v1/workflow/*`, `/v1/trigger/fire`, and
`/v1/dev/submit`. C routes proxy to the Go Unix socket or return `410 Gone`; they do not select a C
engine.

The separately supervised `workflows` process serves one bounded Go stage at principal 20/event
9217: it classifies an already-parsed `advance_request` as `ok`, `replay`, `stale`, `unbound`,
`terminal`, or `badargs`. The caller still owns state reads, authorization, audit, persistence, and
the transition itself. Its C `module_adapter.c` is a wire-parity fixture, not an executable workflow
engine.

The local CLI still uses the C parser for `workflow blocks`, `validate`, `show`, `list`, and `new`.
Those commands are compatibility helpers. Browser Save and Validate, server admission, and the Go
registry are authoritative for execution. `workflow run` and `workflow status` call the server.

`module.yaml` is a retirement inventory for source, headers, tests, and ownership checks. Its
`enabled_by_default: false` and `runtime_toggle.supported: false` fields do not describe a second
runtime that operators can enable.

## Dependencies and consumers

The descriptor records these dependencies for the C retirement inventory:

- `audit`: records compatibility-path evidence.
- `config`: supplies legacy activation and policy values.
- `delegates`: executes retained delegate seams.
- `execution-policy`: authorizes effects on compatibility paths.
- `ir`: carries typed request and result records.
- `module-runtime`: supplies descriptor lifecycle contracts.
- `routing`: selects retained compatibility journeys.
- `skills`: supplies instructions requested by delegate helpers.
- `tools`: exposes typed effects to retained executors.
- `workspace`: confines repository and worktree access.

Most consumers are C tests, local CLI definition commands, and compatibility routes awaiting removal.

The Go WFE consumes typed resource-plane operations for agents, credentials, delegates,
roundtables, and forge mechanics. That dependency does not transfer lifecycle ownership back to C.

## Providers and readiness

Go reports definition, store, scheduler, runner, workspace, and provider readiness. The C module's
registration or test coverage is not a Go WFE readiness signal.

The narrow `/v1/internal/forge/execute` resource route is an exception: C resolves credentials and
performs a typed, worktree-confined operation requested by Go. It does not choose the operation or
advance the run.

## Configuration and activation

`AIMEE_WFE_ENGINE` must resolve to `go`; any other value fails server startup. The C workflow
scheduler exits without registering when Go owns WFE.

- `runtime_toggle.supported`: `false`; the descriptor cannot hot-enable a C workflow runtime.

Variables such as `AIMEE_WORKFLOW_REPO`, `AIMEE_WORKFLOW_BASE`, `AIMEE_AUTONOMY_BASE`, and
`AIMEE_DEFAULT_BRANCH` configure legacy C paths and tests. They do not override repository or branch
authority for a Go work item. Live Go policy comes from `aimee.yaml` and the Workflows **Run policy**
panel, subject to explicit process environment overrides.

## Surfaces

Current operator surfaces are the Go workflow API, Workflow Actions, Edit Workflows, and server-backed
`workflow run` and `workflow status` commands. Local C definition commands remain useful for authoring,
but their catalog and validator can drift until they are converted to the Go API.

## Data and migrations

Go stores work items, events, node attempts, artifacts, approvals, leases, pause state, costs,
trigger identities, worktrees, and PR references. It snapshots the canonical definition and resolved
block catalog when a run starts. Artifact bodies live under `$AIMEE_HOME/wfe-artifacts`.

Do not migrate active Go state through the retired C store. Removal work must preserve Go replay,
idempotency, lease behavior, and history.

## Security and privacy

Treat workflow YAML, proposals, artifacts, prompts, repository content, provider output, and commands
as untrusted. Go owns lifecycle validation and policy. C confines the resource operations it still
performs, resolves credentials without returning them, and applies `execution-policy` at retained
effect boundaries.

The current human decision is a hashed approval artifact plus a lifecycle transition. It is not a
cryptographic principal signature.

## Supported journeys

A supported run enters the Go API, pins a definition, acquires `aimee-wfe` scheduler capacity, and
advances through Go block executors. Agent and forge work may cross the typed C resource boundary,
then return an artifact or named failure to Go.

The C-only engine, scheduler, trigger dispatcher, and approval path are not supported journeys.

## Tests and failure behavior

The descriptor lists the C tests that preserve retirement safety and compatibility behavior. They do
not prove that C is a supported runtime. The C/Go process parity tests cover only the advance
classification described above. Go engine, API, WFE, and roundtable tests own current lifecycle
behavior under `server-go/internal`.

Invalid definitions fail before Go admission. Missing providers, denied effects, lease conflicts,
budget exhaustion, verification failures, and forge failures must leave an inspectable park or
terminal state. They must not silently complete or fall back to C.

## Operational diagnostics

Start with `workflow status <id>` and record the pinned version, node, state, park reason, attempt,
cost, repository, and provider readiness. Use C logs only when the failing action names the resource
plane, socket proxy, or forge boundary.

Do not report prompt bodies, credentials, approval material, or raw provider output.

## Compatibility

Definition grammar, block names, version hashes, API shapes, event state, retry rules, gate outcomes,
and child IDs are Go compatibility contracts. The C parser is not the source of truth for those
contracts.

The current child ID shape is `<root>.s<10hex>.g<generation>.<index>`. Final PRs target the branch
checked out when the repository was admitted; slice PRs target the exact parent feature branch.

## Extension and removal

Add blocks, triggers, lifecycle APIs, and scheduler behavior in Go. Add a C change only when a typed
resource operation or an existing non-lifecycle caller still requires it.

Before deleting a C unit, prove that no CLI helper, compatibility route, resource seam, build target,
or test still consumes it. Update `module.yaml` with the removal so the ownership checker remains an
accurate retirement ledger.
