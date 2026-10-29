# gateway module

## Purpose and non-goals

`gateway` is required core and is the interface point that accepts supported user/client requests, applies
identity and execution policy, runs the canonical IR pipeline, routes execution, and returns or delivers
responses. It is not merely an optional chat-channel daemon, nor does it own provider-specific structural
translation, protocol standards, workflow definitions, or channel implementation details.

## Public contracts

The implementation owners are `src/modules/gateway/gateway_policy.c` and
`src/modules/gateway/gateway_delegate.c`; their canonical include namespace is
`aimee/gateway`. The active gateway contracts are `gateway_policy_apply_ir` and
`gateway_delegate_run_request_pipeline`, declared under
`src/modules/gateway/include/aimee/gateway/gateway_policy.h` and
`src/modules/gateway/include/aimee/gateway/gateway_delegate.h`. They consume the shared canonical
model Request and keep provider-specific rendering at the translation boundary.
The old native-JSON request-stage runner and its provider-specific memory
injection modes had no production callers and are retired. Active ingress stages
are owned by `src/server/aimee_ir_serve.c`; they operate directly on
`aimee_request_t`, not a second request schema.

The descriptor sets `ownership_complete: true`. That latch exhaustively checks the module-local C and
private-header files (the module has no private headers) and requires this canonical document.
Public-header and test entries are explicit audited claims, not auto-discovered completeness domains,
so the latch does not police the public-header inventory. Completeness is a statement about file
ownership only; it does not assert that every owned export has a caller, or that every declared test
runs in every build system. The source liveness, build and test membership, adjacent-surface, and
public-surface audit is recorded in `docs/validation/core-modularization-slice-38.md`.

`gateway_policy_apply_ir` polices canonical tool catalogs, including namespace
children, before provider rendering. The former vendor-JSON request mutation and
model-pinning helpers are retired. `gateway_policy_is_denied_tool` remains a pure
predicate covered by the module tests; live response governance uses the shared
canonical tool-name policy.

The standalone `aimee-gateway` delivery binary (`src/gateway`: Telegram/ntfy/webhook platforms,
pairing, STT, TTS) and the `send_message` MCP tool that stood in for it are retired; nothing shipped
it. Server-side notifications and presence events deliver through the typed notify path (ntfy and
local). The gateway-mutation family remains owned by economizer under
`src/modules/economizer/gateway_mutate*.c`.

## Dependencies and consumers

- `config`: supplies listeners, limits, provider and channel settings consumed by gateway journeys.
- `execution-policy`: authorizes ingress identity, tools, delegation, egress, and delivery actions.
- `ir`: supplies the canonical request, response, block, delta, and tool-call representation.
- `module-runtime`: supplies required lifecycle and extension contracts for core ingress orchestration.
- `protocols`: parses and serializes supported client/agent protocol framing at the boundary.
- `translation`: converts canonical IR to and from selected provider/client wire shapes.

Consumers include interactive users, thin clients, MCP/ACP clients, runtime APIs, delegates, workflows,
and optional channel adapters. Routing is invoked by the gateway journey but remains its own core owner.

## Providers and readiness

The typed ingress stages are applied by `aimee_ir_apply_gateway_request_stages` in
`src/server/aimee_ir_serve.c`. Core gateway readiness requires identity capture, policy,
IR stages, routing handoff, execution, and a
working response path for at least the selected listener. Outbound notification delivery (ntfy,
local) is a provider beneath that boundary and may be unavailable independently.
Readiness must report the exact missing stage rather than marking all of gateway optional.

## Configuration and activation

- `runtime_toggle.supported`: `false`; the core gateway path is required even when optional listeners or delivery platforms are disabled.

Listener, policy, memory-stage, and notification delivery settings tune concrete providers.
A future module profile must omit a platform's code and hide its GUI/config when excluded, while the core
gateway remains. Make compiles both remaining sources into `DATA_SRCS`; CMake builds only the thin
client, which compiles neither. Multiple config keys must not select parallel ingress pipelines with divergent policy or
IR stage ordering.

## Surfaces

Core gateway surfaces include the runtime HTTP/API ingress, canonical stage pipeline, policy decisions,
and delegate orchestration seam. The `ask_user` MCP tool formats a question for the requesting client. Server and KB dashboards
belong to their independently enabled web modules, not to the headless core gateway.

## Data and migrations

Gateway coordinates identity and route correlation; durable records are stored through the relevant
server/DB owners. Migrations must preserve identity and route correlation. Transient IR data remains per
turn.

## Security and privacy

Ingress identity must be captured before policy and propagated through routing, tools, delegation, and
delivery. Prompts and responses require scope, redaction, and retention controls. Client input and
recalled context are untrusted; neither may
bypass `gateway_policy` or execution-policy checks by selecting another ingress handler.

## Supported journeys

A client connects through a supported protocol, establishes identity/session, and submits a request;
`gateway` converts it to IR, runs ordered memory/policy/router stages, executes the eligible target, composes
a response, and serializes or delivers it.

## Tests and failure behavior

The gateway descriptor owns the direct `src/tests/test_gateway_policy.c` and
`src/tests/test_gateway_p4_delegate.c` contracts. Active typed stage coverage is in
`src/tests/test_aimee_ir_serve.c`; dedicated tests of the retired native request runner
and its unused registry were removed with those implementations.
`test_gateway_mutate_wire.c`, mixed ingress/governance tests, and cross-protocol IR tests cover
adjacent boundaries without becoming gateway-owned.

Make registers all three declared tests in `src/tests/Rules.mk`. `scripts/check_module_test_registration.py` pins that per-test registration to a reviewed
baseline. Identity
or policy failure is fail-closed; absent optional delivery returns a typed unsupported/unready result;
stage failure must not fall through to a second, less-policed execution path.

## Operational diagnostics

Use gateway stage traces, `correlation_id` and session identifiers, policy reasons, route/provider selection,
and protocol/translation diagnostics. Health should never log bearer tokens or full private prompts.

## Compatibility

Ingress routes, authentication identity, stage order, IR mutation semantics, response/stream behavior,
and session formats are compatibility contracts.

## Extension and removal

Add listeners as providers around the single policy/IR/routing journey, not independent servers that
duplicate it. Audit similarly named gateway mutation paths for consumers before moving or deleting them.
Core `gateway` cannot be removed because it is the user-to-Aimee execution interface.
