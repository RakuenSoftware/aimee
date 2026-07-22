# gateway module

## Purpose and non-goals

`gateway` is required core and is the interface point that accepts supported user/client requests, applies
identity and execution policy, runs the canonical IR pipeline, routes execution, and returns or delivers
responses. It is not merely an optional chat-channel daemon, nor does it own provider-specific structural
translation, protocol standards, workflow definitions, or channel implementation details.

## Public contracts

Current orchestration contracts are split across `src/gateway_pipeline.c`, `src/gateway_policy.c`,
`src/gateway_delegate.c`, and their `src/headers` interfaces. Channel/session implementation lives under
`src/gateway`, while request mutation is partly in `src/modules/economizer/gateway_mutate*.c`. The
descriptor-only `src/modules/gateway` directory is the target core owner; a later move must separate
universal ingress orchestration (`gateway_pipeline`, `gateway_policy`, and `gateway_delegate`) from
optional delivery-platform code (`platform_*`, `delivery_router`, STT, and TTS) rather than moving the tree
wholesale. Gateway main, context, pairing, and session-key paths require a caller/lifecycle audit before
final placement.

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
gateway remains. Multiple config keys must not select parallel ingress pipelines with divergent policy or
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

`test_gateway_pipeline.c`, `test_gateway_policy.c`, `test_gateway.c`, `test_gateway_p4_delegate.c`,
platform tests, `test_gateway_mutate_wire.c`, and cross-protocol IR tests cover the present split. Identity
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
