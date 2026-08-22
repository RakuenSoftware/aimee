# execution-policy module

## Purpose and non-goals

`execution-policy` is required core and makes fail-closed authorization decisions for delegate,
tool, filesystem, process, network, credential, and repository actions. It owns the decision
contract and denial reasons, not the action implementation, secret store, workspace resolver,
optional governance policy authoring, or append-only audit ledger.

## Public contracts

`src/modules/execution-policy/execution_policy.c` owns the tool-action policy decision: `policy_load`
(reads the operator policy `.aimee-policy.json`) and `policy_check_tool` (the fail-closed allow/deny
decision), extracted from `src/server/agent_policy.c`. The decision contract is declared in the shared
`src/headers/agent_exec.h`, which this module implements while the server implements the rest of
`agent_policy.c` and reaches the decision through the same header. This is the same arrangement by
which `memory` owns its contract while DB1/DB2 implement storage. Per the module boundary, **schema and argument
validation (`tool_validate`) and side-effect classification (`tool_side_effect`) stay with the
server/`tools` surface** and are not part of this module; likewise the execution trace, metrics, env
introspection, and manifest half of `agent_policy.c`. `pre_tool_check`
(`src/modules/guardrails/guardrails_action_audit.c`) is a guardrails enforcement point that consumes
this decision, and gateway request policing stays a gateway enforcement point. Both consume the
canonical decision vocabulary rather than owning it. Consolidating the distributed enforcement points
onto this single decision engine remains future work.

## Dependencies and consumers

- `config`: supplies effective local authorization, guardrail, approval, and enforcement settings.
- `ir`: supplies typed requests, tool calls, and action facts evaluated by policy.
- `module-runtime`: supplies required lifecycle and readiness contracts for the policy service.

Consumers include [delegates](delegates.md), [gateway](gateway.md), `tools`, `git`, `workspace`, and
the optional workflows and governance modules. Governance may author or distribute organizational
policy, but core execution-policy remains the final local enforcement boundary when governance is absent.
These names are consumers or consolidation evidence, not undeclared dependencies of execution-policy.

## Providers and readiness

The required reference provider is the local deterministic policy path built from `guardrails`,
agent policy, gateway policy, and configuration. Optional semantic classifiers may advise it, but
readiness requires a local decision for every supported action class; timeout, malformed input, or
missing policy material must produce a typed denial rather than an unfiltered execution fallback.
Classifier output is advisory and cannot independently produce an allow or override a core denial.

## Configuration and activation

- `runtime_toggle.supported`: `false`; the required module cannot be hot-disabled, while individual policy inputs remain configurable.

### Config touchpoint

The module consumes `guardrail_mode`, the `guardrails` section from the external config-module
contract, delegate/tool restrictions, approval
rules, and related effective settings. `config` owns parsing and truthful projection; policy owns
interpretation at decision time. Ambiguous duplicated parsing is deferred to the config slice.

## Surfaces

Policy appears through preflight errors, approval prompts, denied tool results, gateway policing,
guardrail diagnostics, and action audit records. The `aimee guardrails` surface exposes evidence;
protocol, delegate, Git, and tool commands remain owned by their modules even when a policy decision
determines whether the requested action proceeds.

## Data and migrations

Current state includes external policy JSON, `session_state_t` guardrail evidence, configuration,
approval state, and audit references spread across owners. Migration into the module must preserve
policy version, principal, action class, input digest, decision, reason, approval identity, and
enforcement point without turning transient secrets or raw untrusted output into policy storage.

## Security and privacy

Inputs include untrusted repository content, tool output, subprocess arguments/environments, and
configuration. Authorization must precede mutation and secret release; an undecidable action fails
closed. `execution-policy` may inspect bounded metadata or secret references but must not become a
secret cache, log raw credentials, or treat optional governance availability as a safety prerequisite.
Full runtime non-bypass across dynamically constructed calls is a **hypothesis, unverified**.

## Supported journeys

A typed action from [delegates](delegates.md), `tools`, `git`, or `workspace` is normalized with its
principal, target, capability, and workspace facts; core policy evaluates it; an approval seam runs
when permitted; and the owning executor receives allow or a concrete denial. Gateway-local policing
uses the same canonical action vocabulary without transferring gateway ownership into this module.

## Tests and failure behavior

Coverage is distributed across `test_guardrails.c`, `test_agent_policy_intercept.c`,
`test_gateway_policy.c`, tool-validation tests, Git guard tests, and workflow native-gate tests.
Malformed actions, missing principal/workspace facts, classifier failure, or unavailable approval
must deny safely; warning-only modes must remain explicit and cannot masquerade as enforcement.

## Operational diagnostics

Diagnostics must report `policy_version`, principal, canonical action/tool name, target class,
workspace, enforcement point, decision, and redacted reason. Operators must be able to distinguish
configuration absence, invalid input, explicit denial, approval timeout, and auxiliary-classifier
failure without logging command environments, repository contents, or credential values.
Cross-module citations and limits are collected in the [Slice 16 validation record](../validation/core-modularization-slice-16.md).

## Compatibility

Decision codes, canonical action names, approval semantics, and pre/post-tool enforcement ordering
are compatibility contracts. Moving guardrails or policy fragments under `execution-policy` must
preserve CLI/API results and audit correlation; a compatibility shim cannot authorize independently
or silently translate a denial into an allow.

## Extension and removal

New policy sources plug into one fail-closed decision contract and may strengthen, not bypass, local
enforcement. `agent_policy`, `gateway_policy`, workflow gates, and guardrails with similarly named
checks require caller and behavior comparison before consolidation. A self-tested policy island with
no production enforcement point is a dead-code candidate, not evidence that the module is optional.
