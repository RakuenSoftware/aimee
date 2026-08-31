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
