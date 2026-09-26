# execution-policy module

## Purpose and non-goals

`execution-policy` makes fail-closed authorization decisions for tool, filesystem, process, network,
credential, and repository actions. It owns verdicts and denial reasons. It does not execute actions,
hold credentials, resolve workspaces, or replace the append-only audit and optional governance layers.

## Public contracts

Principal `17` serves event `8449`, stage `1`, through the public header
`aimee/execution-policy/module_api.h`. The caller submits an already classified, bounded action and
authenticated context; the Go handler returns a typed allow, deny, or approval verdict and reason.

## Dependencies and consumers

- `config`: supplies effective computer-use settings and the fixed operator-policy locations.
- `ir`: supplies the canonical typed action evaluated by the policy engine.
- `module-runtime`: supervises the process and authenticates principal `17` request/reply traffic.

Delegates, gateway, tools, Git, workspace, workflows, and governance consume the verdict. The enforcing
caller applies it synchronously and has no local authorization fallback.

## Providers and readiness

The Go implementation at `server-go/modules/execution-policy` provides the required handler. Readiness
requires valid built-in rules and any configured policy documents. A parser error, missing required
contract, or failed bus registration leaves the seam unavailable, causing consumers to deny actions.

## Configuration and activation

- `runtime_toggle.supported`: `false`; a live action path cannot lose its required authorization decision point.

The module reads `.aimee-policy.json` and `$AIMEE_HOME/policy.json` for forbidden commands, tool rules,
and approval levels, plus typed settings from `config`. Policy files may narrow authority but cannot bypass hard rails.

## Surfaces

Action callers use the single `execution-policy` bus contract; operators edit the documented policy files and observe
denials through UI, CLI, logs, and audit evidence. There is no public endpoint that accepts an invented
principal or raw allow verdict, and no command that disables the required module during execution.

## Data and migrations

The module keeps no mutable database. `Policy` documents are version-controlled or operator-managed JSON,
while request decisions are ephemeral and their bounded outcomes are classified for the ledger. Changes
need validation but no schema migration; historic audit records retain the policy outcome seen at execution.

## Security and privacy

Caller identity comes from the bus, not request data. The module evaluates normalized actions, returns `deny` for unknown classes, and rejects
unknown classes, and treats absence, timeout, and malformed responses as denial. Ledger records prove
the seam fired without persisting raw tool arguments, credentials, file content, or response bodies.

## Supported journeys

Before a delegate runs a command, the server classifies it into IR, attaches session and workspace
identity, and calls `execution-policy`. An allow proceeds under the original bounds, an approval verdict
parks for an authorized human, and a denial returns the stable reason without performing the action.

## Tests and failure behavior

Tests under `server-go/modules/execution-policy` and C caller tests cover rule precedence, malformed
frames, timeouts, approval, and denial. Unknown actions, absent policy service, invalid JSON, ambiguous
matches, and response validation failures deny. No transport or parser error becomes an implicit allow.

## Operational diagnostics

Use principal `17` readiness, action class, rule identifier, verdict, denial reason, and request ID.
Correlate with the content-free ledger event and the enforcing caller log. Do not log raw arguments,
environment variables, credentials, source content, or sensitive filesystem paths while diagnosing.

## Compatibility

Event `8449`, stage `1`, verdict meanings, normalized action classes, and denial reason identifiers are
stable contracts. The legacy `policy_check_tool` caller symbol remains a compatibility seam but cannot
authorize locally. Additive rules must preserve fail-closed handling of unknown values.

## Extension and removal

Add an action class in `IR`, wire fixtures, normalization, policy evaluation, enforcement, audit mapping,
and denial tests together. Removing a rule requires proving no caller depends on its reason semantics.
Removing the module requires a reviewed replacement at every action seam; bypass is not a migration.

## Exploration contract implementation status

The legacy tool seam reports source discovery as observe-only metadata when no
host-authenticated contract is available. Baseline forbidden-command, path and
approval checks still decide authorization, including registered shell aliases.
Compound commands and effectful `find`/`rg` invocations are not classified as
pure discovery. The attention guard retains its explicit legacy operator cap;
nonpositive legacy values remain disabled.

The private exploration state machine models literal-zero limits, revision
history, atomic in-process reservations, retry identities, conservative refunds,
and host-observed recovery. A fallback requires a recorded empty/failed indexed
attempt and a matching gap. It is restricted to one class, canonical path scope,
contract revision and one-minute expiry; operator limits still apply. Two
completed constrained turns can lower the adaptive tier without changing
confidence provenance. These APIs are not exposed through model tool JSON.

The session owner now commits the bounded state into `session_state` using
migration 38. Its private operation checks the session directory's principal,
locks the session row, and applies the same accounting across tasks and
processes. Hard directives run before admission, which atomically marks a call
as possibly dispatched; uncertain outcomes cannot refund a charge. Optional
adaptive storage failure falls back to baseline, while configured operator
ceilings still require durable accounting. Final memory assembly provides plan/source commitments and coverage;
C forwards them without deciding memory sufficiency. Native dispatch and the
authenticated `hooks.pre` route consume the Go accounting decision. Literal
operator ceilings live under `exploration` in the operator policy; optional
adaptive ceilings live under `adaptive_exploration` and default to observe.

`context_contract_expand` is registered as a native control tool. It consumes a
reason and references from a host-observed `code_search`/`find_symbol` failure or
empty result. Completed native turns derive starvation from recorded lookup
gaps and budget observations; model declarations and trivial writes cannot
reset it. External hook result bodies are not accepted as proof of a lookup.

Child jobs inherit the host context's `budget_task` root while retaining their
own task/plan revisions and attempt histories. PostgreSQL stores the root usage
once alongside global session usage. Unrelated tasks have separate adaptive
allowances; revisions cannot transfer a task to a fresh root. Existing JSON rows
upgrade their task counters without resetting spent work.

The retained code-context packet supplies an exact index generation when present;
dropped, mixed-generation or unprovenanced context cannot claim one. Native
refresh preserves retained ingress coverage and index observations. Before native discovery admission, the host rechecks the exact generation via
the scoped project-stats route, with a one-second transport timeout. Unknown,
stale, rejected or malformed replies invalidate adaptive freshness. The external `tools.execute` route binds the authenticated
session and observes actual indexed results before exposing expansion references.

The host observes a clean Git worktree at issuance and rechecks it before
discovery admission. A dirty tree or changed commit invalidates the adaptive
binding without resetting operator work counters. The bounded read disables
Git filesystem-monitor commands and treats errors/timeouts as unavailable.

Final provider receipt admission adds the routed provider/model, route and
context-limit commitments. Subsequent receipts for the same plan preserve the
revision and recovery gaps; changed models or limits create a revision without
resetting usage. A private memory-owner probe checks the retained plan before
native admission. Restarted owners, expired handles and scope changes invalidate
adaptive control.

Enforcement additionally requires `AIMEE_EXPLORATION_ENFORCE=1` and a reviewed,
root-owned `/etc/aimee/exploration-calibration.json`. Every ancestor must be
protected from non-root writes; symlinks and non-regular artifacts are rejected.
The file and opt-in are checked again for each admission, so revocation takes
effect without deleting budget history. The artifact pins the supported query
class, project/workspace/directory, clean checkout, exact index generation,
provider/model/route, final context-limit digest and adaptive limits. Only raw
scan caps are eligible in this first activation path. A missing or incompatible
artifact preserves observe mode and baseline operator policy. See the
[calibration contract](../../benchmarks/memory/EXPLORATION_GATE.md).

MR-07 is still in progress. Both disposable process topologies passed at
`6008b3835`; the later receipt/freshness/activation changes need fresh process
acceptance. Native task requirement forwarding, hook freshness parity, generic
external issuance and measured paired workload gates remain open. No reviewed
calibration artifact is installed or shipped. MR-08 and MR-09 have not started.
Component tests are not evidence of task-quality noninferiority.
