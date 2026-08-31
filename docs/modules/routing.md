# routing module

## Purpose and non-goals

`routing` is a required same-container process and selects an eligible agent, model/provider, tier, or execution target for a
typed request using role, capability, health, policy, cost, memory, and learned evidence. It does not own
HTTP route tables, workflow graph edges, channel delivery, provider JSON translation, or the delegate
execution loop; similarly named local routers remain with those owning modules.

## Public contracts

`src/modules/routing/routing.c` is the canonical vendored implementation of the daemon-side
eligibility and policy surface during the process migration: role dispatch (`agent_route`,
`agent_route_at_tier`, `agent_route_with_caps`), capability/tier selection, delegate pick
(`delegate_pick_for_role`), availability (`agent_is_available_for_routing`), route-block reasons
(`agent_routing_block_reason`), and the route health/policy filters. This was extracted from
`src/server/agent_config.c`; the routing contract is declared in the shared `src/headers/agent_config.h`,
which this module implements while the config/auth half of `agent_config.c` stays in the server and is
reached through the same header (the arrangement by which `memory` owns its contract while DB1/DB2
implement storage). Equal-candidate selection no longer uses those module-local statics in the shipping
server: `server-go/modules/routing` serves the pointer-free `module_api.h` contract from the separately
supervised Go `aimee-module-routing` process, and `server/module_routing_adapter.c` calls it through the
shared core module client. A missing, cancelled, timed-out, or malformed module reply fails the route
closed. The routing C `module_adapter.c` remains a parity fixture while the rest of `routing.c` is
migrated. The routing block is otherwise self-contained: its statics are module-local, and no config
function calls the routing functions. Therefore, `routing.c` has no module-private header. Delegate-specific
route overrides and preflight remain in `src/modules/delegates/delegate_routing.c` (the delegates
module, a routing sibling, calls the same `agent_config.h` role predicates). Advisory
`router_advise.c` remains workflow-owned and is outside this module despite its filename.

## Dependencies and consumers

- `config`: supplies agent rosters, tiers, provider choices, limits, and routing policy.
- `ir`: supplies typed request facts and capability requirements used during selection.
- `learning`: supplies bounded outcome evidence that can improve future selection.
- `memory`: supplies relevant user/project context and recorded provider/delegate evidence.
- `module-runtime`: supplies authenticated attach, request/reply, deadline, cancellation, and lifecycle contracts.

Consumers include primary chat, delegates, gateway, workflows, roundtable, failover/retry, and diagnostics.
HTTP dispatch tables and channel `delivery_router` are consumers of decisions or separate local routers,
not competing owners of agent/provider selection.

## Providers and readiness

The configured `agent_t` roster and health/policy filters are routing inputs, not replaceable routing
providers. Route calls fail closed until the routing process attaches to the server-local bus. A complete
deployment readiness check must additionally prove that attachment, at least one eligible route for each
supported required journey, and a deterministic explanation when candidates are excluded; that readiness
integration remains migration work. A provider outage may remove a candidate; it must not disable the
routing module or cause an unfiltered fallback to a forbidden route.

## Configuration and activation

- `runtime_toggle.supported`: `false`; the routing process is required although individual agents and policy strategies are configurable.

Agent tiers, role mappings, capability flags, provider overrides, health, budgets, and adaptive policies
tune candidate selection. The GUI and generated config must expose only fields read by the active routing
implementation. Disabling an advisory workflow hook is not equivalent to removing required core
`routing` from primary or delegate execution.

## Surfaces

Routing surfaces include agent-list/diagnostic output, route-preflight errors, selected provider/model/tier
in run records, health/policy exclusion reasons, and router advice/audit events. It owns no protocol
listener or generic `/v1` dispatch table. CLI `--via`, `--provider`, and tier selections are inputs that
must pass through the same eligibility and policy filters as automatic choices.

## Data and migrations

Configuration stores rosters and policy; `run` records store selections, failures, costs, outcomes, and
adaptive evidence under their owning schemas. Route-history migrations must preserve requested versus
selected provider/model, exclusion reason, fallback chain, identity, and policy version so later learning
cannot treat an operator override or outage recovery as an unbiased quality outcome.

## Security and privacy

Every candidate remains subject to execution policy, identity, vault credential scope, data-egress rules,
workspace authority, and provider capability restrictions. Explicit route overrides must not bypass
`agent_routing_block_reason`, the core filter called by `delegate_route_preflight`. Diagnostics may show names and exclusion categories but
must not reveal provider secrets, private prompt context, or cross-tenant learning evidence.

## Supported journeys

A primary or delegate request supplies role and capability needs; `agent_route` filters the configured roster by
health and policy, applies explicit bounded overrides or automatic tier/cost/quality selection, records the
decision, and hands one eligible target to execution. On typed failure, failover re-enters the same policy
boundary rather than calling an arbitrary provider directly.

## Tests and failure behavior

`src/tests/test_agent.c`, delegate routing/driver, failover, provider, and route-policy tests cover core
selection and exclusions. Workflow router tests cover their separate owner, not this module. No eligible route must return a concrete preflight
error; a blocked override fails closed; health or credential failure may choose a policy-allowed fallback
but must never silently cross a tier, tenant, capability, or egress boundary.

## Operational diagnostics

Use `delegate_route_preflight` detail and `aimee agent list`, plus health and policy exclusion reasons, selected
provider/model/tier, failover events, latency/cost metrics, and outcome records. Operators should be able
to distinguish no configured candidate, capability mismatch, policy denial, provider outage, budget
exhaustion, and translation failure without reconstructing selection from generic HTTP errors.

## Compatibility

Role names, tier meanings, override precedence, candidate filtering, selected-route audit fields, and
failover semantics are compatibility contracts. Moving `agent_config` and delegate selection into the
module cannot change which agent wins for a fixed fixture unless an approved policy/version change also
updates baselines and explains migration of learning evidence.

## Extension and removal

New routing strategies must plug into one candidate/filter/decision pipeline and emit comparable reasons
and outcomes. Do not relocate HTTP route tables, workflow-internal routers, or delivery routing merely
because their filenames contain `route`; consolidate only duplicated agent/provider selection. Core
routing cannot be optional because gateway and delegates cannot execute without an eligible target.
