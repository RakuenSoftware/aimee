# execution-policy module

## Purpose and non-goals

`execution-policy` is a required Go module process. It makes fail-closed authorization decisions for
tool, filesystem, process, network, credential, and repository actions. It owns the decision and denial
reason; it does not execute the action, keep secrets, resolve workspaces, author optional governance
policy, or implement the append-only audit ledger.

The module is deliberately outside the C communication core. C owns the event-bus transport and the
small enforcement caller that applies the verdict. Go owns all policy logic.

## Public contract

The wire identity is declared in
`src/modules/execution-policy/include/aimee/execution-policy/module_api.h`: event kind `8449`, stage
`1`. `server-go/modules/execution-policy/execution_policy.go` implements the required handler. The C
caller in `src/server/execution_policy_bus.c` serializes the already-classified tool request, calls the
Go process synchronously over the bus, validates the response, and denies on absence, timeout, or a
malformed response.

The existing `policy_check_tool` compatibility contract remains declared in
`src/headers/agent_exec.h`; callers do not gain a local authorization fallback. Schema and argument
validation (`tool_validate`) and side-effect classification (`tool_side_effect`) remain with the
server/tools surface.

Every request and reply for event kind `8449` is classified `ledger` in
`src/modules/process-contracts.json`. The observability bridge therefore leaves a durable record that
the decision seam fired without persisting raw arguments or response bodies.

## Dependencies and consumers

- `config` supplies effective computer-use settings; operator policy uses the module-owned fixed
  locations described below.
- `ir` supplies typed actions evaluated by policy.
- `module-runtime` supervises readiness and the request/reply lifecycle.

Consumers include delegates, gateway, tools, Git, workspace, workflows, and governance. Governance may
author or distribute organizational policy, but the required local execution-policy process remains the
final action boundary.

## Configuration and activation

The required module cannot be hot-disabled. It reads `.aimee-policy.json`, followed by
`$AIMEE_HOME/policy.json`, and understands `forbidden_commands`, `tool_rules`, and `approval_levels`.
Missing optional policy means no operator restriction matched; an unreadable or invalid policy denies.
Computer-use policy is supplied explicitly with each request so the decision uses the daemon's effective
configuration rather than independently re-reading it.

## Security and failure behavior

Authorization precedes mutation. Invalid requests, unavailable module transport, timeout, cancellation,
invalid policy, and invalid responses all fail closed. The module receives bounded JSON and returns only
`allowed` plus a redacted reason. The durable bus record stores event identity, status, sizes, and a
response digest, not raw command arguments, repository content, credentials, or tool output.

Optional semantic classifiers may advise policy but cannot independently allow an action or override a
denial. Source-discovery interception and computer-use restrictions are part of the Go decision engine,
so moving the module out of core does not create a C policy fallback.

## Tests and compatibility

Go unit tests cover ordinary allows, forbidden commands, path restrictions, source-discovery denial,
computer-use allowlists, invalid input, cancellation, and policy-load failure. Existing C enforcement
tests exercise the compatibility caller. Decision codes, action names, denial semantics, and pre-tool
ordering remain compatibility contracts.

New policy sources must join this single fail-closed Go decision contract. A compatibility shim may
transport or apply the verdict, but may not authorize independently or translate a denial into an allow.
