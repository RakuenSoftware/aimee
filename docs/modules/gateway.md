# gateway module

## Purpose and non-goals

`gateway` is required core and is the interface point that accepts supported user/client requests, applies
identity and execution policy, runs the canonical IR pipeline, routes execution, and returns or delivers
responses. It is not merely an optional chat-channel daemon, nor does it own provider-specific structural
translation, protocol standards, workflow definitions, or channel implementation details.

## Public contracts

The canonical orchestration sources are `src/modules/gateway/gateway_pipeline.c`,
`src/modules/gateway/gateway_policy.c`, and `src/modules/gateway/gateway_delegate.c`. Their public
contracts live under `src/modules/gateway/include/aimee/gateway`; consumers use the canonical include
namespace `aimee/gateway`. This canonical include namespace is the only supported public-header route.
The public header inventory is
`src/modules/gateway/include/aimee/gateway/gateway_pipeline.h`,
`src/modules/gateway/include/aimee/gateway/gateway_policy.h`, and
`src/modules/gateway/include/aimee/gateway/gateway_delegate.h`. The `gw_` prefix predates the canonical
namespace and remains as a compatibility name; renaming it requires the compatibility process.
`gw_pipeline_run_request`, `gateway_policy_apply_request`, and
`gateway_delegate_run_request_pipeline` retain their existing contracts.

The descriptor sets `ownership_complete: true`. That latch exhaustively checks the module-local C and
private-header files (the module has no private headers) and requires this canonical document.
Public-header and test entries are explicit audited claims, not auto-discovered completeness domains,
so the latch does not police the public-header inventory. Completeness is a statement about file
ownership only; it does not assert that every owned export has a caller, or that every declared test
runs in every build system. The source liveness, build and test membership, adjacent-surface, and
public-surface audit is recorded in `docs/validation/core-modularization-slice-38.md`.

`gateway_policy_is_denied_tool` has no tracked-tree caller outside
`src/tests/test_gateway_policy.c`; every other export has a tracked production consumer. `src/tests/test_anthropic_http.c` defines its own
`gateway_policy_pin_model` as a link-time stub, so that one test binary shadows the module symbol
rather than linking `gateway_policy.c`.

Gateway main, context, pairing, session-key, and channel/session code remains under `src/gateway` and
awaits a caller and lifecycle audit. That directory contains the `aimee-gateway` delivery binary,
delivery routing, Telegram/ntfy/webhook platforms, pairing, STT, and TTS. It is a distinct ownership surface
from this module despite the shared gateway name; `GATEWAY_SRCS` in `src/Makefile` refers to it, not
to `src/modules/gateway`. The gateway-mutation family remains owned by economizer under
`src/modules/economizer/gateway_mutate*.c`. Optional delivery implementations (`platform_*`,
`delivery_router`, STT, and TTS) require later provider-isolation slices and are not moved wholesale.

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

Core `gateway_pipeline` readiness requires identity capture, policy, IR stages, routing handoff, execution, and a
working response path for at least the selected listener. Telegram, ntfy, webhook, speech, and similar
delivery implementations are providers beneath that boundary and may be unavailable independently.
Readiness must report the exact missing stage rather than marking all of gateway optional.

## Configuration and activation

- `runtime_toggle.supported`: `false`; the core gateway path is required even when optional listeners or delivery platforms are disabled.

Listener, pairing, policy, memory-stage, delivery, STT/TTS, and platform settings tune concrete providers.
A future module profile must omit a platform's code and hide its GUI/config when excluded, while the core
gateway remains. Build membership is currently asymmetric by policy, not by drift: Make compiles all
three sources into `DATA_SRCS`, while no CMake target compiles any of them. `CMakeLists.txt` states
that CMake builds only the `aimee` thin client plus the unit-test suite, and that the `aimee-server`,
`aimee-kb`, `aimee-gateway`, and `aimee-webchat` targets were removed because CMake's source lists had
drifted far behind `src/Makefile` and no CI gate built them. Multiple config keys must not select parallel ingress pipelines with divergent policy or
IR stage ordering.

## Surfaces

Core gateway surfaces include the runtime HTTP/API ingress, canonical stage pipeline, policy decisions,
and delegate orchestration seam. Current `src/gateway` session/pairing, delivery results, and channel
adapters remain inventory surfaces, while platform adapters are optional-provider candidates outside the
target core owner. MCP gateway tools are protocol tools that invoke gateway actions. Server and KB dashboards
belong to their independently enabled web modules, not to the headless core gateway.

## Data and migrations

Gateway owns or coordinates `session_key`, pairing, delivery, correlation, and platform state; durable records
are stored through the relevant server/DB owners. Migrations must preserve identity, session keys,
delivery idempotency, route correlation, and consent/pairing state. Transient IR data remains per turn;
platform retry queues must not replay a message under a different principal after upgrade.

## Security and privacy

Ingress identity must be captured before policy and propagated through routing, tools, delegation, and
delivery. Pairing tokens, webhook secrets, channel identifiers, audio, prompts, and responses require
scope, redaction, and retention controls. Platform input and recalled context are untrusted; neither may
bypass `gateway_policy` or execution-policy checks by selecting another ingress handler.

## Supported journeys

A client connects through a supported protocol, establishes identity/session, and submits a request;
`gateway` converts it to IR, runs ordered memory/policy/router stages, executes the eligible target, composes
a response, and serializes or delivers it. Optional channel and speech providers can wrap that same core
journey; disabling them must leave direct headless API/CLI operation intact.

## Tests and failure behavior

The gateway descriptor owns the direct `src/tests/test_gateway_pipeline.c`,
`src/tests/test_gateway_policy.c`, and `src/tests/test_gateway_p4_delegate.c` contracts.
`test_gateway.c`, platform tests, `test_gateway_mutate_wire.c`,
mixed ingress/governance tests, and cross-protocol IR tests cover adjacent boundaries without becoming
gateway-owned; the `test_gateway*` names that exercise delivery platforms belong to the `src/gateway`
binary rather than to this module.

Make registers all three declared tests in `src/tests/Rules.mk`; CTest registers none of them. That
is an audited current condition, consistent with the module sitting outside CMake's thin-client
profile; CMake does own a unit-test suite, so profile exclusion alone does not establish that these
tests must be absent from it. `scripts/check_module_test_registration.py` pins that per-test registration to a reviewed
baseline. Identity
or policy failure is fail-closed; absent optional delivery returns a typed unsupported/unready result;
stage failure must not fall through to a second, less-policed execution path.

## Operational diagnostics

Use gateway stage traces, `correlation_id` and session identifiers, policy reasons, route/provider selection,
delivery/platform health, queue/retry state, and protocol/translation diagnostics. Health should separate
core ingress readiness from Telegram, ntfy, webhook, STT, or TTS provider readiness and should never log
pairing secrets, bearer tokens, full private prompts, or audio content.

## Compatibility

Ingress routes, authentication identity, stage order, IR mutation semantics, response/stream behavior,
pairing/session formats, and delivery idempotency are compatibility contracts. Separating universal
orchestration under `src/modules/gateway` from optional delivery code must preserve parity tests and external surfaces; platform
extraction must retain explicit capability/readiness reporting.

## Extension and removal

Add listeners or channels as providers around the single policy/IR/routing journey, not independent
servers that duplicate it. Audit similarly named gateway mutation and delivery paths for consumers before
moving or deleting them. Optional platform code can become separate modules, but core `gateway` cannot be
removed because it is the user-to-Aimee execution interface.
