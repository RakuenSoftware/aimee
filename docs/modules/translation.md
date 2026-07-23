# translation module

## Purpose and non-goals

`translation` is required core and converts canonical IR to and from provider and client wire shapes without
changing the selected task, identity, permissions, or route. It owns structural mapping for messages,
system blocks, tools, thinking, usage, stop reasons, and streams. It does not choose providers, perform
HTTP retries, authorize tool calls, or own the external protocol listener.

## Public contracts

`src/modules/translation/module.yaml` is the complete module-local ownership inventory: eight shipping
translation units, three canonical public headers, five direct tests, and this module document.
`ownership_complete: true` rejects an unlisted module-local C or private-header file and omission of the
canonical document. Canonical public-header placement remains enforced by the descriptor-derived
header-layout gate. This physical ownership claim does not imply that every exported adapter helper is
selected by a production journey.

The canonical ingress adapters are
`src/modules/translation/aimee_frontend_anthropic.c`,
`src/modules/translation/aimee_frontend_openai.c`, and
`src/modules/translation/aimee_frontend_responses.c`. Their public parse-to-canonical contract is
`src/modules/translation/include/aimee/translation/aimee_frontend.h`, included through the canonical
namespace `aimee/translation`. These adapters only map supported client/provider request shapes into the
IR-owned `aimee_request_t`; they do not listen, dispatch, select a route, or send provider requests.

The canonical egress and response adapters are
`src/modules/translation/aimee_backend_anthropic.c`,
`src/modules/translation/aimee_backend_openai.c`,
`src/modules/translation/aimee_backend_responses.c`, and
`src/modules/translation/aimee_backend_bedrock.c`. Their public wire-to-canonical and
canonical-to-wire contract is
`src/modules/translation/include/aimee/translation/aimee_backend.h`. These backend wire-format
adapters build provider request bodies and parse provider responses; they do not send requests,
select providers, sign AWS requests, load credentials, or retry transport failures. Direct contract
coverage lives in `src/tests/test_aimee_backend.c` and `src/tests/test_aimee_backend_bedrock.c`.

Canonical streaming conversion lives in `src/modules/translation/aimee_ir_stream.c`, with the public
contract at `src/modules/translation/include/aimee/translation/aimee_ir_stream.h`. Its bounded state
machines convert OpenAI Chat chunks and decoded Bedrock ConverseStream event JSON into IR deltas, then
render IR deltas as Anthropic stream events. They do not decode Bedrock binary framing, write HTTP/SSE
responses, or select the live relay. Direct coverage lives in `src/tests/test_aimee_ir_stream.c` and
`src/tests/test_aimee_converse_stream.c`.

Remaining translation seams include the `aimee_ir_build_provider_body` entry point in
`src/headers/aimee_ir_serve.h`.
The target module owns provider-body conversion; the current IR-named symbol remains a
relocation/compatibility seam until callers and installed headers migrate. `anthropic_http.c` still
contains legacy `translate_request` and `build_provider_body` paths beside the typed IR path, while
`openai_chat.c` has its own builder. The mixed `aimee_ir_serve.c` implementation remains explicitly
deferred: it owns rollout gates, memory-stage configuration and registration, and a transitional legacy
response bridge. `router_advise.c` is workflow-owned despite its name and is outside translation.

## Dependencies and consumers

- `ir`: supplies the canonical request, response, block, tool, usage, and streaming representation.
- `module-runtime`: supplies required lifecycle and extension contracts for the core conversion path.

Consumers include gateway and protocol ingress/egress, agent/delegate provider drivers, response
composition, OpenAI and Anthropic compatibility routes, Bedrock backends, and shadow/parity checks.
Provider selection and route failover remain routing concerns; provider transport retries stay with the
provider driver or execution owner. Translation must not silently retry or duplicate a wire call.

## Providers and readiness

Translation is deterministic core with `aimee_ir` implementations selected by source wire and destination driver,
not an optional remote provider. A provider adapter is ready only when request and response mappings cover
its advertised capabilities. Unknown block or stop-reason semantics must fail explicitly or use a tested
loss policy; a successful HTTP connection does not prove translation readiness.

## Configuration and activation

- `runtime_toggle.supported`: `false`; translation is required for every supported non-identical wire path.

Settings may tune reasoning handling, caching, streaming, model fields, or compatibility behavior, but
they must not create a second untracked translation pipeline. Provider and protocol availability determine
which adapters are exercised; configuration fields should be exposed only when a compiled adapter reads
them and their effect is covered by `test_ir_*` or shape tests.

## Surfaces

The target translation surface exposes `provider-body` conversion, response parsers, stream-event maps,
and shape/parity fixtures. It owns no listener, HTTP route table, CLI command, dashboard, credential store,
or network retry loop. OpenAI, Anthropic, Bedrock, MCP, and ACP JSON framing remains with protocols while
the typed field mapping belongs here.

## Data and migrations

Translation owns transient JSON and canonical objects, not an independent durable schema. Stored runs,
transcripts, provider traces, and shadow comparisons rely on stable `aimee_response_t` semantics and may
capture wire payloads. A mapping change must migrate or version any persisted interpretation and refresh
fixtures/baselines without rewriting historical provider evidence as if it used the new mapping.

## Security and privacy

Converters must preserve system/user role boundaries, hidden reasoning, tool identifiers, and untrusted
arguments without promoting content into authority. Provider-specific raw fields may contain credentials
or private prompts and require bounded redaction. Translation must never silently expose `THINKING` as
answer text or drop policy-relevant tool data to satisfy a weaker destination shape.

## Supported journeys

An Anthropic, OpenAI, ACP, or other ingress is parsed into `IR`; after routing and policy, translation emits
the selected provider's request and converts returned response/stream events back into canonical blocks;
the client protocol then serializes them. Native same-wire passthrough may preserve bytes in legacy-parity
mode (covered by `test_ir_legacy_parity.c`), but any mutated request must use the canonical conversion journey.

## Tests and failure behavior

The descriptor owns `src/tests/test_aimee_frontend.c`, `src/tests/test_aimee_backend.c`,
`src/tests/test_aimee_backend_bedrock.c`, `src/tests/test_aimee_ir_stream.c`, and
`src/tests/test_aimee_converse_stream.c` as direct contracts for canonical ingress, backend wire-format,
and streaming adapters. Adjacent coverage includes `test_anthropic_ingress.c`,
`test_anthropic_shape.c`, `test_openai_shape.c`, `test_ir_crossproto_egress.c`,
`test_ir_legacy_parity.c`, and `test_aimee_ir_serve.c`.
Malformed or unsupported input must return a bounded error without partial tool execution; stream mapping
must emit one coherent terminal state and free partially built structures.

## Operational diagnostics

Use `source_wire` and destination wire labels, driver capability reports, shape fixtures, IR shadow mismatches, and
provider-body diagnostics to locate mapping failures. Diagnostics should distinguish parse, canonical
mutation, serialization, provider rejection, and response conversion, and should report a field path or
block type without logging full sensitive payloads.

## Compatibility

External wire shapes, canonical field meanings, cache placement, tool schema mapping, stop reasons,
stream event ordering, and parity-mode bytes are compatibility contracts. Consolidating legacy builders
into `src/modules/translation` must preserve tested provider behavior; removal of a fallback requires a
definition/caller inventory proving no supported journey still selects it.

## Extension and removal

Add an adapter through the shared IR conversion interface and capability contract rather than copying an
ingress handler. The parallel legacy and typed builders in `anthropic_http.c` and `openai_chat.c` are
consolidation candidates, not automatically dead code; parity and mutation branches must be traced first.
Translation cannot be removed while Aimee supports more than one external/provider wire.

The [slice 32 liveness audit](../validation/core-modularization-slice-32.md) proves that every owned source
ships and that each translation unit contains a production path. It separately records seven helpers
whose exact current-worktree callers are tests only. Removing, privatizing, or activating those intended
egress and Bedrock-streaming contracts requires focused compatibility/readiness slices; they are not
silently treated as live merely because their source files ship.
